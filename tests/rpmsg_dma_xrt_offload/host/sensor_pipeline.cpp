// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// sensor_pipeline - x86 simulation of a 6-sensor ResNet50 frame pipeline split
// across two NPUs:
//
//   sensors 0..N-1  -> T20 RPU_APP (RPURuntime tests/xrt_batches) over the
//                      MDB5 PCIe-DMA rpmsg link, one APU_XRT_BATCH_RUN per frame
//   sensors N..2N-1 -> the local Strix/Krackan NPU through XRT, N runs per frame
//
// A frame is complete when all 2N OFMs are ready; a final step (TBD, simulated
// with --post-us) then consumes them and releases the frame's buffers.
//
// Sync modes. Default: the one sync point per frame is at the finish - the
// final step waits until both sides finished their batch for frame f; the
// sides start independently and the faster one may run ahead (bounded by the
// slots). --lockstep adds a start sync: neither side starts batch f before
// both finished batch f-1, so each frame's two batches start together.
//
// Ping-pong: --slots S (default 2) buffer sets per side. Frame f uses slot
// f % S, so both producers start frame f+1 while the final step still holds
// frame f's OFMs; a producer only waits when the final step is S frames behind.
//
// Setup happens once per side: RPU CREATE (ELF parsed once, hw_context +
// kernel + run on the RPU) and the Strix hw_context + ELF module + kernel.
// Every frame sets all kernel args again (set_arg per run), since each slot's
// BOs are at different addresses.
//
// Threads: RPU producer, Strix producer, final-step consumer. Each side keeps
// its own timings; the report gives per-frame batch time, per-inference model
// time, the frame join (latency / skew) and throughput.

#include "rpu_batch_client.h"

#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_hw_context.h"
#include "xrt/xrt_kernel.h"
#include "xrt/experimental/xrt_elf.h"
#include "xrt/experimental/xrt_ext.h"
#include "xrt/experimental/xrt_module.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

namespace {

using clk = std::chrono::steady_clock;

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Opts {
	std::string ini;
	uint32_t sensors = 3;          // per side
	uint32_t frames = 100;
	uint32_t slots = 2;            // 2 = ping-pong, 1 = serialized with the final step
	double   fps = 0;              // 0 = free-running
	uint32_t post_us = 0;          // simulated final step
	bool lockstep = false;         // also sync batch starts (see top)
	std::string save_dir;          // OFMs of frame 0
	std::string csv;               // per-frame timings
	bool verbose = false, trace_dma = false;
	uint32_t timeout_s = 60, poll_us = 0;

	// T20 RPU (xrt_batches): kernel args wts / ifm / spill / ofm
	bool use_rpu = true;
	std::string rpu_elf, rpu_wts;
	std::vector<std::string> rpu_ifm, rpu_golden;   // [sensor]
	uint64_t rpu_sz_spill = 2408448, rpu_sz_ofm = 1000;
	uint32_t rpu_arg_wts = 0, rpu_arg_ifm = 1, rpu_arg_spill = 2, rpu_arg_ofm = 3;

	// local Strix-class NPU (VAIML xclbin + ctrl.elf)
	bool use_strix = true;
	std::string strix_xclbin, strix_elf, strix_wts;
	std::vector<std::string> strix_ifm, strix_golden;  // [sensor]
	uint64_t strix_sz_ifm = 200704, strix_sz_wts = 26772456, strix_sz_spill = 301056,
		 strix_sz_ofm = 1000;
	int strix_arg_ifm = 3, strix_arg_wts = 4, strix_arg_spill = 5, strix_arg_ofm = 6,
	    strix_arg_ctrl = 7;
};

void usage(const char *argv0)
{
	std::printf(
"Usage: %s --ini INI [RPU options] [Strix options] [options]\n"
"\n"
"Pipeline:\n"
"  --sensors N          sensors per side (default 3; RPU side <= 8)\n"
"  --frames F           frames to run (default 100)\n"
"  --slots S            buffer sets per side (default 2 = ping-pong; 1 = no overlap)\n"
"  --fps R              sensor frame rate (default 0 = as fast as possible)\n"
"  --post-us US         simulated final-step time per frame (default 0)\n"
"  --lockstep           start both sides' batch f together: wait until both\n"
"                       finished batch f-1 (default: sync at the finish only)\n"
"\n"
"T20 RPU side (RPU_APP = xrt_batches.elf):\n"
"  --rpu-elf FILE       AIE control ELF (zlib or raw)\n"
"  --rpu-wts FILE       weights (sent once, CREATE)\n"
"  --rpu-ifm F[,F..]    IFM file: one for all sensors, or one per sensor\n"
"  --rpu-spill SIZE     spill bytes per sensor (default 2408448)\n"
"  --rpu-ofm SIZE       OFM bytes per sensor (default 1000)\n"
"  --rpu-args W,I,S,O   kernel-arg index of wts,ifm,spill,ofm (default 0,1,2,3)\n"
"  --rpu-golden F[,F..] expected OFM: one for all sensors, or one per sensor\n"
"                       (checked every frame)\n"
"  --no-rpu             Strix side only\n"
"\n"
"Local Strix-class NPU side:\n"
"  --strix-dir DIR      VAIML partition dir: DIR/unified-2x4x4.xclbin, DIR/ctrl.elf,\n"
"                       DIR/wts32.bin (each overridable below)\n"
"  --strix-xclbin FILE  --strix-elf FILE  --strix-wts FILE\n"
"  --strix-ifm F[,F..]  IFM file: one for all sensors, or one per sensor\n"
"  --strix-sizes I,W,S,O  ifm,wts,spill,ofm bytes (default 200704,26772456,301056,1000)\n"
"  --strix-args I,W,S,O,C kernel-arg index of ifm,wts,spill,ofm,ctrl-pkt (default 3,4,5,6,7)\n"
"  --strix-golden F[,F..] expected OFM: one for all sensors, or one per sensor\n"
"                       (checked every frame)\n"
"  --no-strix           RPU side only\n"
"\n"
"Options:\n"
"  --save-out DIR       write frame 0's OFMs to DIR/{rpu,strix}_s<i>.bin\n"
"  --csv FILE           per-frame timings (ns since start)\n"
"  --timeout-s N        per-command timeout (default 60)\n"
"  --poll-us N          RPU completion poll interval (default 0 = busy poll)\n"
"  --trace-dma          print the first MDB5 DMA to each (addr, len)\n"
"  --verbose            link bring-up, hexdumps, per-frame lines\n", argv0);
}

std::vector<std::string> split(const std::string &s, char sep = ',')
{
	std::vector<std::string> v;
	std::stringstream ss(s);
	std::string t;
	while (std::getline(ss, t, sep))
		if (!t.empty())
			v.push_back(t);
	return v;
}

template <typename T> std::vector<T> split_num(const std::string &s, size_t n, const char *what)
{
	auto v = split(s);
	if (v.size() != n)
		throw std::runtime_error(std::string(what) + " takes " + std::to_string(n) + " values");
	std::vector<T> r;
	for (auto &x : v)
		r.push_back(static_cast<T>(std::stoull(x, nullptr, 0)));
	return r;
}

Opts parse(int argc, char **argv)
{
	Opts o;
	std::string strix_dir;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto val = [&]() -> std::string {
			if (i + 1 >= argc)
				throw std::runtime_error(a + " needs a value");
			return argv[++i];
		};
		auto num = [&]() { return static_cast<uint32_t>(std::stoul(val(), nullptr, 0)); };
		if      (a == "--ini")          o.ini = val();
		else if (a == "--sensors")      o.sensors = num();
		else if (a == "--frames")       o.frames = num();
		else if (a == "--slots")        o.slots = num();
		else if (a == "--fps")          o.fps = std::stod(val());
		else if (a == "--post-us")      o.post_us = num();
		else if (a == "--lockstep")     o.lockstep = true;
		else if (a == "--save-out")     o.save_dir = val();
		else if (a == "--csv")          o.csv = val();
		else if (a == "--timeout-s")    o.timeout_s = num();
		else if (a == "--poll-us")      o.poll_us = num();
		else if (a == "--trace-dma")    o.trace_dma = true;
		else if (a == "--verbose")      o.verbose = true;
		else if (a == "--rpu-elf")      o.rpu_elf = val();
		else if (a == "--rpu-wts")      o.rpu_wts = val();
		else if (a == "--rpu-ifm")      o.rpu_ifm = split(val());
		else if (a == "--rpu-spill")    o.rpu_sz_spill = std::stoull(val(), nullptr, 0);
		else if (a == "--rpu-ofm")      o.rpu_sz_ofm = std::stoull(val(), nullptr, 0);
		else if (a == "--rpu-golden")   o.rpu_golden = split(val());
		else if (a == "--rpu-args") {
			auto v = split_num<uint32_t>(val(), 4, "--rpu-args");
			o.rpu_arg_wts = v[0]; o.rpu_arg_ifm = v[1]; o.rpu_arg_spill = v[2]; o.rpu_arg_ofm = v[3];
		}
		else if (a == "--no-rpu")       o.use_rpu = false;
		else if (a == "--strix-dir")    strix_dir = val();
		else if (a == "--strix-xclbin") o.strix_xclbin = val();
		else if (a == "--strix-elf")    o.strix_elf = val();
		else if (a == "--strix-wts")    o.strix_wts = val();
		else if (a == "--strix-ifm")    o.strix_ifm = split(val());
		else if (a == "--strix-golden") o.strix_golden = split(val());
		else if (a == "--strix-sizes") {
			auto v = split_num<uint64_t>(val(), 4, "--strix-sizes");
			o.strix_sz_ifm = v[0]; o.strix_sz_wts = v[1]; o.strix_sz_spill = v[2]; o.strix_sz_ofm = v[3];
		}
		else if (a == "--strix-args") {
			auto v = split_num<int>(val(), 5, "--strix-args");
			o.strix_arg_ifm = v[0]; o.strix_arg_wts = v[1]; o.strix_arg_spill = v[2];
			o.strix_arg_ofm = v[3]; o.strix_arg_ctrl = v[4];
		}
		else if (a == "--no-strix")     o.use_strix = false;
		else if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
		else throw std::runtime_error("unknown option: " + a);
	}
	if (!strix_dir.empty()) {
		if (o.strix_xclbin.empty()) o.strix_xclbin = strix_dir + "/unified-2x4x4.xclbin";
		if (o.strix_elf.empty())    o.strix_elf = strix_dir + "/ctrl.elf";
		if (o.strix_wts.empty())    o.strix_wts = strix_dir + "/wts32.bin";
	}

	if (!o.use_rpu && !o.use_strix)
		throw std::runtime_error("--no-rpu and --no-strix: nothing to run");
	if (!o.sensors || !o.frames || !o.slots)
		throw std::runtime_error("--sensors, --frames and --slots must be > 0");
	auto check_ifm = [&](std::vector<std::string> &v, const char *opt) {
		if (v.empty())
			return;
		if (v.size() == 1)
			v.resize(o.sensors, v[0]);
		if (v.size() != o.sensors)
			throw std::runtime_error(std::string(opt) + ": give 1 or " +
						 std::to_string(o.sensors) + " files");
	};
	if (o.use_rpu) {
		if (o.rpu_elf.empty() || o.rpu_wts.empty() || o.rpu_ifm.empty())
			throw std::runtime_error("RPU side needs --rpu-elf, --rpu-wts, --rpu-ifm (or --no-rpu)");
		check_ifm(o.rpu_ifm, "--rpu-ifm");
		check_ifm(o.rpu_golden, "--rpu-golden");
		if (APU_XRT_BATCH_RUN_NUM_ARGS(o.sensors, 3u) > APU_XRT_BATCH_MAX_ARGS)
			throw std::runtime_error("RPU RUN with " + std::to_string(o.sensors) +
						 " sensors x 3 kargs exceeds one rpmsg buffer (max 8)");
	}
	if (o.use_strix) {
		if (o.strix_xclbin.empty() || o.strix_elf.empty() || o.strix_wts.empty() ||
		    o.strix_ifm.empty())
			throw std::runtime_error("Strix side needs --strix-dir (or --strix-xclbin/elf/wts) "
						 "and --strix-ifm (or --no-strix)");
		check_ifm(o.strix_ifm, "--strix-ifm");
		check_ifm(o.strix_golden, "--strix-golden");
	}
	return o;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
std::vector<uint8_t> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f)
		throw std::runtime_error("cannot open: " + path);
	std::vector<uint8_t> v(static_cast<size_t>(f.tellg()));
	f.seekg(0);
	if (!v.empty() && !f.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(v.size())))
		throw std::runtime_error("short read: " + path);
	return v;
}

void write_file(const std::string &path, const void *p, size_t n)
{
	std::ofstream f(path, std::ios::binary);
	if (!f.write(static_cast<const char *>(p), static_cast<std::streamsize>(n)))
		throw std::runtime_error("cannot write: " + path);
}

std::vector<uint8_t> load_elf(const std::string &path)
{
	uint8_t *d = nullptr;
	uint64_t n = 0;
	int rc = rbc_load_elf(path.c_str(), &d, &n);
	if (rc)
		throw std::runtime_error("load ELF " + path + ": " + std::strerror(-rc));
	std::vector<uint8_t> v(d, d + n);
	std::free(d);
	return v;
}

int top1(const uint8_t *p, size_t n)
{
	const auto *s = reinterpret_cast<const int8_t *>(p);
	return static_cast<int>(std::max_element(s, s + n) - s);
}

// ---------------------------------------------------------------------------
// Shared pipeline state
// ---------------------------------------------------------------------------
struct FrameStat {
	uint64_t arrival = 0;          // sensor data available (fps) / first producer start
	// RPU
	uint64_t rpu_start = 0, rpu_sync_wait = 0, rpu_slot_wait = 0, rpu_h2d = 0, rpu_exec = 0, rpu_d2h = 0, rpu_end = 0;
	apu_xrt_batch_result rres{};
	// Strix
	uint64_t strix_start = 0, strix_sync_wait = 0, strix_slot_wait = 0, strix_h2d = 0, strix_setarg = 0,
		 strix_exec = 0, strix_d2h = 0, strix_end = 0;
	// final step
	uint64_t ready = 0, post_start = 0, post_end = 0;
	uint32_t rpu_mismatch = 0, strix_mismatch = 0;
};

std::atomic<bool> g_sigint{false};
rbc_client *g_rbc = nullptr;

void on_sigint(int)
{
	g_sigint = true;
	if (g_rbc)
		rbc_request_stop(g_rbc);
}

class Pipeline {
public:
	explicit Pipeline(const Opts &o) : o_(o), fs_(o.frames) {}

	std::vector<FrameStat> &stats() { return fs_; }
	uint64_t t0() const { return t0_; }
	void set_t0(uint64_t t) { t0_ = t; }

	// Producer side: block until frame f's sensor data has arrived, (with
	// --lockstep) both sides finished batch f-1, and frame f's slot is free
	// (the final step is done with frame f - slots). Returns false on abort.
	bool acquire(uint32_t f, uint64_t *sync_wait, uint64_t *slot_wait, uint64_t *start)
	{
		if (o_.fps > 0) {
			const uint64_t at = t0_ + static_cast<uint64_t>(f * 1e9 / o_.fps);
			while (now_ns() < at && !aborted())
				std::this_thread::sleep_for(std::chrono::nanoseconds(
					std::min<uint64_t>(at - now_ns(), 10000000ull)));
		}
		uint64_t t = now_ns();
		std::unique_lock<std::mutex> lk(m_);
		if (o_.lockstep)
			wait(lk, [&] {
				return ((!o_.use_rpu || rpu_done_ >= f) && (!o_.use_strix || strix_done_ >= f)) || abort_;
			});
		*sync_wait = now_ns() - t;
		t = now_ns();
		wait(lk, [&] { return f < consumed_ + o_.slots || abort_; });
		*slot_wait = now_ns() - t;
		*start = now_ns();
		auto &a = fs_[f].arrival;
		if (o_.fps > 0)
			a = t0_ + static_cast<uint64_t>(f * 1e9 / o_.fps);
		else if (!a || *start < a)
			a = *start;
		return !abort_ && !g_sigint;
	}

	void produced(bool rpu, uint32_t f)
	{
		std::lock_guard<std::mutex> lk(m_);
		(rpu ? rpu_done_ : strix_done_) = f + 1;
		cv_.notify_all();
	}

	// Consumer side: block until both producers finished frame f.
	bool wait_ready(uint32_t f)
	{
		std::unique_lock<std::mutex> lk(m_);
		wait(lk, [&] {
			return ((!o_.use_rpu || rpu_done_ > f) && (!o_.use_strix || strix_done_ > f)) || abort_;
		});
		return !abort_ && !g_sigint;
	}

	void release(uint32_t f)
	{
		std::lock_guard<std::mutex> lk(m_);
		consumed_ = f + 1;
		cv_.notify_all();
	}

	void abort()
	{
		std::lock_guard<std::mutex> lk(m_);
		abort_ = true;
		cv_.notify_all();
	}

	bool aborted()
	{
		std::lock_guard<std::mutex> lk(m_);
		return abort_ || g_sigint;
	}

	uint32_t consumed()
	{
		std::lock_guard<std::mutex> lk(m_);
		return consumed_;
	}

private:
	// Wait for pred under lk. The 50 ms timeout only lets SIGINT (whose
	// handler cannot notify) end the wait.
	template <typename Pred> void wait(std::unique_lock<std::mutex> &lk, Pred pred)
	{
		while (!pred() && !g_sigint)
			cv_.wait_for(lk, std::chrono::milliseconds(50), pred);
	}

	const Opts &o_;
	std::vector<FrameStat> fs_;
	uint64_t t0_ = 0;
	std::mutex m_;
	std::condition_variable cv_;
	uint32_t rpu_done_ = 0, strix_done_ = 0, consumed_ = 0;
	bool abort_ = false;
};

// ---------------------------------------------------------------------------
// T20 RPU side
// ---------------------------------------------------------------------------
struct RpuSide {
	const Opts &o;
	rbc_client *c = nullptr;
	uint32_t ctx_id = 0;
	bool created = false;

	uint64_t elf_pa = 0, wts_pa = 0;
	std::vector<std::vector<uint8_t>> ifm;          // [sensor] host data
	uint64_t sz_ifm = 0;
	struct Slot {
		std::vector<uint64_t> ifm_pa, spill_pa, ofm_pa;
		std::vector<std::vector<uint8_t>> ofm;      // [sensor] host copy for the final step
	};
	std::vector<Slot> slots;
	std::vector<std::vector<uint8_t>> golden;   // [sensor], empty = no check

	// setup timings
	uint64_t setup_sync_ns = 0;
	rbc_cmd_stat create{}, destroy{};
	bool destroyed = false;

	explicit RpuSide(const Opts &opts) : o(opts) {}

	void setup()
	{
		rbc_config cfg;
		int rc = rbc_config_load(&cfg, o.ini.empty() ? nullptr : o.ini.c_str());
		if (rc)
			throw std::runtime_error("config " + o.ini + ": " + std::strerror(-rc));
		rbc_open_opts oo{o.verbose, o.trace_dma, o.timeout_s, o.poll_us};
		rc = rbc_open(&c, &cfg, &oo);
		if (rc)
			throw std::runtime_error(std::string("RPU link open: ") + std::strerror(-rc));
		g_rbc = c;

		auto elf = load_elf(o.rpu_elf);
		auto wts = read_file(o.rpu_wts);
		for (auto &p : o.rpu_ifm) {
			ifm.push_back(read_file(p));
			if (sz_ifm && ifm.back().size() != sz_ifm)
				throw std::runtime_error("--rpu-ifm files differ in size");
			sz_ifm = ifm.back().size();
		}
		for (auto &p : o.rpu_golden)
			golden.push_back(read_file(p));

		chk(rbc_alloc(c, RBC_POOL_RPU, elf.size(), &elf_pa), "alloc ELF");
		chk(rbc_alloc(c, RBC_POOL_DEV, wts.size(), &wts_pa), "alloc wts");
		slots.resize(o.slots);
		for (auto &s : slots) {
			s.ifm_pa.resize(o.sensors); s.spill_pa.resize(o.sensors); s.ofm_pa.resize(o.sensors);
			s.ofm.assign(o.sensors, std::vector<uint8_t>(o.rpu_sz_ofm));
			for (uint32_t i = 0; i < o.sensors; ++i) {
				chk(rbc_alloc(c, RBC_POOL_DEV, sz_ifm, &s.ifm_pa[i]), "alloc ifm");
				chk(rbc_alloc(c, RBC_POOL_DEV, o.rpu_sz_spill, &s.spill_pa[i]), "alloc spill");
				chk(rbc_alloc(c, RBC_POOL_DEV, o.rpu_sz_ofm, &s.ofm_pa[i]), "alloc ofm");
			}
		}

		// once: ELF + weights to the device, spills zeroed, CREATE
		uint64_t t = now_ns();
		chk(rbc_write(c, elf_pa, elf.data(), elf.size()), "sync ELF");
		chk(rbc_write(c, wts_pa, wts.data(), wts.size()), "sync wts");
		setup_sync_ns = now_ns() - t;
		for (auto &s : slots)
			for (uint32_t i = 0; i < o.sensors; ++i)
				chk(rbc_zero(c, s.spill_pa[i], o.rpu_sz_spill), "zero spill");

		rbc_karg fixed{o.rpu_arg_wts, wts_pa, wts.size()};
		chk(rbc_create(c, elf_pa, elf.size(), &fixed, 1, &ctx_id, &create), "CREATE");
		created = true;
	}

	// One frame: IFMs in, RUN (all sensors, every karg set on the RPU), OFMs out.
	void frame(uint32_t slot_idx, FrameStat &fs)
	{
		Slot &s = slots[slot_idx];
		uint64_t t = now_ns();
		for (uint32_t i = 0; i < o.sensors; ++i)
			chk(rbc_write(c, s.ifm_pa[i], ifm[i].data(), sz_ifm), "sync ifm");
		fs.rpu_h2d = now_ns() - t;

		const uint32_t kidx[3] = {o.rpu_arg_ifm, o.rpu_arg_spill, o.rpu_arg_ofm};
		std::vector<rbc_karg> e(o.sensors * 3u);
		for (uint32_t i = 0; i < o.sensors; ++i) {
			e[i * 3 + 0] = {0, s.ifm_pa[i], sz_ifm};
			e[i * 3 + 1] = {0, s.spill_pa[i], o.rpu_sz_spill};
			e[i * 3 + 2] = {0, s.ofm_pa[i], o.rpu_sz_ofm};
		}
		rbc_cmd_stat st;
		chk(rbc_run(c, ctx_id, o.sensors, 3, kidx, e.data(), &st), "RUN");
		fs.rpu_exec = st.exec_ns;
		fs.rres = st.res;

		t = now_ns();
		for (uint32_t i = 0; i < o.sensors; ++i)
			chk(rbc_read(c, s.ofm[i].data(), s.ofm_pa[i], o.rpu_sz_ofm), "read ofm");
		fs.rpu_d2h = now_ns() - t;
	}

	void teardown()
	{
		if (created) {
			rbc_clear_stop(c);
			if (rbc_destroy(c, ctx_id, &destroy) == 0)
				destroyed = true;
			else
				std::cerr << "RPU DESTROY failed\n";
		}
		g_rbc = nullptr;
		rbc_close(c);
		c = nullptr;
	}

	static void chk(int rc, const char *what)
	{
		if (rc)
			throw std::runtime_error(std::string("RPU ") + what + ": " + std::strerror(-rc));
	}
};

// ---------------------------------------------------------------------------
// Local Strix-class NPU side (XRT)
// ---------------------------------------------------------------------------
struct StrixSide {
	const Opts &o;
	xrt::device dev;
	xrt::hw_context hwctx;
	xrt::elf elf;
	xrt::module mod;
	xrt::kernel kern;
	xrt::bo wts;
	std::vector<xrt::run> runs;                 // one per sensor, args set every frame
	std::vector<std::vector<uint8_t>> ifm;      // [sensor] host data
	// One batch = one slot: an IFM, spill and OFM BO per sensor ([sensor]).
	struct Slot { std::vector<xrt::bo> ifm, spill, ofm; };
	std::vector<Slot> slots;
	std::vector<std::vector<uint8_t>> golden;   // [sensor], empty = no check

	uint64_t hwctx_ns = 0, elf_ns = 0, wts_ns = 0;

	explicit StrixSide(const Opts &opts) : o(opts) {}

	void setup()
	{
		uint64_t t = now_ns();
		dev = xrt::device{0};
		xrt::xclbin xclbin{o.strix_xclbin};
		hwctx = xrt::hw_context{dev, dev.register_xclbin(xclbin)};
		hwctx_ns = now_ns() - t;

		t = now_ns();
		auto bytes = load_elf(o.strix_elf);
		elf = xrt::elf{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
		mod = xrt::module{elf};
		kern = xrt::ext::kernel{hwctx, mod, "DPU"};
		for (uint32_t i = 0; i < o.sensors; ++i)
			runs.emplace_back(kern);
		elf_ns = now_ns() - t;

		t = now_ns();
		wts = xrt::ext::bo{dev, o.strix_sz_wts};
		{
			auto d = read_file(o.strix_wts);
			std::memset(wts.map<char *>(), 0, o.strix_sz_wts);
			std::memcpy(wts.map<char *>(), d.data(), std::min<size_t>(d.size(), o.strix_sz_wts));
		}
		wts.sync(XCL_BO_SYNC_BO_TO_DEVICE);
		wts_ns = now_ns() - t;

		for (auto &p : o.strix_ifm) {
			ifm.push_back(read_file(p));
			ifm.back().resize(o.strix_sz_ifm, 0);
		}
		for (auto &p : o.strix_golden)
			golden.push_back(read_file(p));

		slots.resize(o.slots);
		for (auto &s : slots)
			for (uint32_t i = 0; i < o.sensors; ++i) {
				s.ifm.push_back(xrt::ext::bo{dev, o.strix_sz_ifm});
				s.ofm.push_back(xrt::ext::bo{dev, o.strix_sz_ofm});
				std::memset(s.ofm.back().map<char *>(), 0, o.strix_sz_ofm);
				s.ofm.back().sync(XCL_BO_SYNC_BO_TO_DEVICE);
				if (o.strix_arg_spill >= 0 && o.strix_sz_spill)
					s.spill.push_back(xrt::ext::bo{dev, o.strix_sz_spill});
			}
	}

	// One frame: IFMs in, all runs queued (args set per run), wait all, OFMs out.
	void frame(uint32_t slot_idx, FrameStat &fs)
	{
		Slot &s = slots[slot_idx];
		uint64_t t = now_ns();
		for (uint32_t i = 0; i < o.sensors; ++i) {
			std::memcpy(s.ifm[i].map<char *>(), ifm[i].data(), o.strix_sz_ifm);
			s.ifm[i].sync(XCL_BO_SYNC_BO_TO_DEVICE);
		}
		fs.strix_h2d = now_ns() - t;

		t = now_ns();
		for (uint32_t i = 0; i < o.sensors; ++i) {
			auto &r = runs[i];
			r.set_arg(0, 3);
			r.set_arg(1, 0);
			r.set_arg(2, 0);
			r.set_arg(o.strix_arg_ifm, s.ifm[i]);
			r.set_arg(o.strix_arg_wts, wts);
			r.set_arg(o.strix_arg_ofm, s.ofm[i]);
			if (!s.spill.empty())
				r.set_arg(o.strix_arg_spill, s.spill[i]);
			if (o.strix_arg_ctrl >= 0)
				r.set_arg(o.strix_arg_ctrl, 0);
		}
		fs.strix_setarg = now_ns() - t;

		t = now_ns();
		for (auto &r : runs)
			r.start();
		for (uint32_t i = 0; i < o.sensors; ++i) {
			auto state = runs[i].wait(std::chrono::seconds(o.timeout_s));
			if (state != ERT_CMD_STATE_COMPLETED)
				throw std::runtime_error("Strix run " + std::to_string(i) + " state " +
							 std::to_string(static_cast<int>(state)));
		}
		fs.strix_exec = now_ns() - t;

		t = now_ns();
		for (auto &b : s.ofm)
			b.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
		fs.strix_d2h = now_ns() - t;
	}
};

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
struct Agg {
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	void print(const char *name, const char *note = "") const
	{
		if (v.empty()) {
			std::printf("  %-34s %10s %10s %10s %10s %10s  %s\n", name, "-", "-", "-", "-", "-", note);
			return;
		}
		auto s = v;
		std::sort(s.begin(), s.end());
		double sum = 0;
		for (double x : s) sum += x;
		auto pct = [&](double p) { return s[std::min(s.size() - 1, static_cast<size_t>(p * (s.size() - 1) + 0.5))]; };
		std::printf("  %-34s %10.3f %10.3f %10.3f %10.3f %10.3f  %s\n", name, sum / s.size() / 1e6,
			    s.front() / 1e6, pct(0.5) / 1e6, pct(0.99) / 1e6, s.back() / 1e6, note);
	}
};

void one(const char *name, uint64_t ns, const char *note = "")
{
	std::printf("  %-34s %10.3f  %s\n", name, ns / 1e6, note);
}

void report(const Opts &o, const RpuSide *rpu, const StrixSide *strix,
	    const std::vector<FrameStat> &fs, uint32_t n, uint64_t t0)
{
	std::printf("\n== Setup (once), ms\n");
	if (rpu) {
		one("RPU sync ELF + weights", rpu->setup_sync_ns, "host -> T20 DDR over MDB5");
		one("RPU CREATE (host send -> done)", rpu->create.exec_ns);
		if (rpu->create.res.prof_magic == APU_XRT_BATCH_PROF_MAGIC) {
			one("  RPU ELF parse", rpu->create.res.elf_parse_ns);
			one("  RPU hw_context + kernel + run", rpu->create.res.hwctx_ns, "once per context, shared by all batches/entries");
		}
		if (rpu->destroyed)
			one("RPU DESTROY (host send -> done)", rpu->destroy.exec_ns);
	}
	if (strix) {
		one("Strix xclbin + hw_context", strix->hwctx_ns);
		one("Strix ELF parse + module + kernel", strix->elf_ns, "incl. one xrt::run per sensor, reused every batch");
		one("Strix weights BO + sync", strix->wts_ns);
	}
	if (!n)
		return;

	Agg r_wait, r_h2d, r_exec, r_rexec, r_inf, r_inf_max, r_bind, r_d2h, r_total;
	Agg s_wait, s_h2d, s_set, s_exec, s_inf, s_d2h, s_total;
	Agg r_sync, s_sync, window, start_skew;
	Agg lat, skew, post, held;
	uint32_t rpu_late = 0, mism_r = 0, mism_s = 0;
	for (uint32_t f = 0; f < n; ++f) {
		const auto &x = fs[f];
		if (rpu) {
			r_sync.add(x.rpu_sync_wait); r_wait.add(x.rpu_slot_wait); r_h2d.add(x.rpu_h2d); r_exec.add(x.rpu_exec);
			r_d2h.add(x.rpu_d2h); r_total.add(x.rpu_end - x.rpu_start);
			if (x.rres.prof_magic == APU_XRT_BATCH_PROF_MAGIC) {
				r_rexec.add(x.rres.exec_ns);
				r_inf.add(static_cast<double>(x.rres.exec_ns) / o.sensors);
				r_inf_max.add(x.rres.exec_max_ns);
				r_bind.add(x.rres.bind_ns);
			}
			mism_r += x.rpu_mismatch;
		}
		if (strix) {
			s_sync.add(x.strix_sync_wait); s_wait.add(x.strix_slot_wait); s_h2d.add(x.strix_h2d); s_set.add(x.strix_setarg);
			s_exec.add(x.strix_exec); s_inf.add(static_cast<double>(x.strix_exec) / o.sensors);
			s_d2h.add(x.strix_d2h); s_total.add(x.strix_end - x.strix_start);
			mism_s += x.strix_mismatch;
		}
		if (rpu && strix) {
			skew.add(x.rpu_end > x.strix_end ? x.rpu_end - x.strix_end : x.strix_end - x.rpu_end);
			rpu_late += x.rpu_end > x.strix_end;
			start_skew.add(x.rpu_start > x.strix_start ? x.rpu_start - x.strix_start
								   : x.strix_start - x.rpu_start);
			window.add(std::max(x.rpu_end, x.strix_end) - std::min(x.rpu_start, x.strix_start));
		}
		lat.add(x.ready - x.arrival);
		post.add(x.post_end - x.post_start);
		held.add(x.post_end - x.arrival);
	}

	std::printf("\n== Per frame (%u frames, %u sensors per side, %u slot%s, %s), host-measured, ms\n",
		    n, o.sensors, o.slots, o.slots > 1 ? "s" : "",
		    o.lockstep ? "lockstep: start + finish sync" : "finish sync only");
	std::printf("  %-34s %10s %10s %10s %10s %10s\n", "", "avg", "min", "p50", "p99", "max");
	if (rpu) {
		std::printf("  T20 RPU batch (%u sensors):\n", o.sensors);
		if (o.lockstep && strix)
			r_sync.print("waiting for Strix batch f-1", "lockstep start sync");
		r_wait.print("waiting for a free slot", "final step behind");
		r_h2d.print("IFMs to device", "MDB5 DMA");
		r_exec.print("RUN (host send -> done)");
		r_rexec.print("  RPU AIE execution, all sensors", "start -> wait, summed");
		r_bind.print("  RPU set_arg, all sensors", "every karg, every sensor");
		r_d2h.print("OFMs from device", "MDB5 DMA");
		r_total.print("batch total", "host: batch start -> OFMs on host");
		r_inf.print("model run time / inference", "RPU-measured, per sensor");
		r_inf_max.print("model run time, slowest sensor");
	}
	if (strix) {
		std::printf("  local Strix NPU batch (%u sensors):\n", o.sensors);
		if (o.lockstep && rpu)
			s_sync.print("waiting for T20 batch f-1", "lockstep start sync");
		s_wait.print("waiting for a free slot", "final step behind");
		s_h2d.print("IFMs to device", "memcpy + BO sync");
		s_set.print("set_arg, all runs");
		s_exec.print("execution, all runs", "first start -> last wait");
		s_d2h.print("OFMs from device", "BO sync");
		s_total.print("batch total", "host: batch start -> OFMs on host");
		s_inf.print("model run time / inference", "execution / sensors (runs queued)");
	}
	std::printf("  frame:\n");
	lat.print("arrival -> all 2N OFMs ready", o.fps > 0 ? "latency vs sensor clock" : "latency");
	if (rpu && strix) {
		char note[64];
		std::snprintf(note, sizeof(note), "RPU finished last in %u/%u", rpu_late, n);
		start_skew.print("RPU vs Strix start skew");
		skew.print("RPU vs Strix finish skew", note);
		window.print("frame batch window", "first batch start -> last batch done");

	}
	post.print("final step", "simulated (--post-us)");
	held.print("arrival -> OFMs released");

	const uint64_t span = fs[n - 1].post_end - std::min(t0, fs[0].arrival);
	std::printf("\n== Throughput: %.2f frames/s, %.1f inferences/s (%u frames in %.3f s)\n",
		    n / (span / 1e9), n * o.sensors * ((rpu ? 1 : 0) + (strix ? 1 : 0)) / (span / 1e9),
		    n, span / 1e9);
	if ((rpu && !rpu->golden.empty()) || (strix && !strix->golden.empty()))
		std::printf("== OFM check: RPU %u mismatching, Strix %u mismatching (of %u frames x %u sensors)\n",
			    mism_r, mism_s, n, o.sensors);
}

void write_csv(const std::string &path, const std::vector<FrameStat> &fs, uint32_t n, uint64_t t0)
{
	std::ofstream f(path);
	auto r = [&](uint64_t t) { return t ? static_cast<int64_t>(t - t0) : -1; };
	f << "frame,arrival,rpu_start,rpu_sync_wait,rpu_slot_wait,rpu_h2d,rpu_exec,rpu_aie_exec,rpu_d2h,rpu_end,"
	     "strix_start,strix_sync_wait,strix_slot_wait,strix_h2d,strix_setarg,strix_exec,strix_d2h,strix_end,"
	     "ready,post_start,post_end\n";
	for (uint32_t i = 0; i < n; ++i) {
		const auto &x = fs[i];
		f << i << ',' << r(x.arrival) << ',' << r(x.rpu_start) << ',' << x.rpu_sync_wait << ',' << x.rpu_slot_wait << ','
		  << x.rpu_h2d << ',' << x.rpu_exec << ',' << x.rres.exec_ns << ',' << x.rpu_d2h << ','
		  << r(x.rpu_end) << ',' << r(x.strix_start) << ',' << x.strix_sync_wait << ',' << x.strix_slot_wait << ','
		  << x.strix_h2d << ',' << x.strix_setarg << ',' << x.strix_exec << ',' << x.strix_d2h
		  << ',' << r(x.strix_end) << ',' << r(x.ready) << ',' << r(x.post_start) << ','
		  << r(x.post_end) << '\n';
	}
}

} // namespace

int main(int argc, char **argv)
{
	Opts o;
	try {
		o = parse(argc, argv);
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << "\n\n";
		usage(argv[0]);
		return 2;
	}

	std::unique_ptr<RpuSide> rpu;
	std::unique_ptr<StrixSide> strix;
	Pipeline pl(o);
	auto &fs = pl.stats();
	std::atomic<bool> failed{false};

	std::signal(SIGINT, on_sigint);
	std::printf("sensor_pipeline: %u frames, %u sensors on %s%s%s, %u slot%s, %s, fps %s, final step %u us\n",
		    o.frames, o.sensors, o.use_rpu ? "T20 RPU" : "", o.use_rpu && o.use_strix ? " + " : "",
		    o.use_strix ? "local Strix NPU" : "", o.slots, o.slots > 1 ? "s" : "",
		    o.lockstep ? "lockstep" : "finish sync only",
		    o.fps > 0 ? std::to_string(o.fps).c_str() : "free-running", o.post_us);

	try {
		if (o.use_rpu) {
			rpu = std::make_unique<RpuSide>(o);
			rpu->setup();
			std::printf("RPU   context %u created (ELF parsed once on the RPU)\n", rpu->ctx_id);
		}
		if (o.use_strix) {
			strix = std::make_unique<StrixSide>(o);
			strix->setup();
			std::printf("Strix hw_context + kernel created (ELF parsed once)\n");
		}
	} catch (const std::exception &e) {
		std::cerr << "setup failed: " << e.what() << "\n";
		if (rpu && rpu->c)
			rpu->teardown();
		return 1;
	}

	pl.set_t0(now_ns());

	auto producer = [&](bool is_rpu) {
		try {
			for (uint32_t f = 0; f < o.frames; ++f) {
				auto &x = fs[f];
				uint64_t sync, wait, start;
				if (!pl.acquire(f, &sync, &wait, &start))
					return;
				const uint32_t slot = f % o.slots;
				if (is_rpu) {
					x.rpu_sync_wait = sync; x.rpu_slot_wait = wait; x.rpu_start = start;
					rpu->frame(slot, x);
					x.rpu_end = now_ns();
				} else {
					x.strix_sync_wait = sync; x.strix_slot_wait = wait; x.strix_start = start;
					strix->frame(slot, x);
					x.strix_end = now_ns();
				}
				pl.produced(is_rpu, f);
			}
		} catch (const std::exception &e) {
			std::cerr << (is_rpu ? "RPU" : "Strix") << " producer: " << e.what() << "\n";
			failed = true;
			pl.abort();
		}
	};

	// Final step (TBD): gets all 2N OFMs of a frame, then releases its slot.
	auto consumer = [&]() {
		try {
			for (uint32_t f = 0; f < o.frames; ++f) {
				if (!pl.wait_ready(f))
					return;
				auto &x = fs[f];
				x.ready = now_ns();
				x.post_start = x.ready;
				const uint32_t slot = f % o.slots;

				std::vector<const uint8_t *> ofm_r, ofm_s;
				if (rpu)
					for (auto &b : rpu->slots[slot].ofm)
						ofm_r.push_back(b.data());
				if (strix)
					for (auto &b : strix->slots[slot].ofm)
						ofm_s.push_back(b.map<const uint8_t *>());

				// ---- next step goes here: all 2N OFMs of frame f ----
				auto differs = [](const uint8_t *p, size_t n, const std::vector<uint8_t> &g) {
					return std::memcmp(p, g.data(), std::min(n, g.size())) != 0;
				};
				for (size_t i = 0; i < ofm_r.size() && !rpu->golden.empty(); ++i)
					x.rpu_mismatch += differs(ofm_r[i], o.rpu_sz_ofm, rpu->golden[i]);
				for (size_t i = 0; i < ofm_s.size() && !strix->golden.empty(); ++i)
					x.strix_mismatch += differs(ofm_s[i], o.strix_sz_ofm, strix->golden[i]);
				if (o.post_us)
					std::this_thread::sleep_for(std::chrono::microseconds(o.post_us));

				if (f == 0 || o.verbose) {
					std::printf("frame %u: top1", f);
					for (size_t i = 0; i < ofm_r.size(); ++i)
						std::printf(" rpu%zu=%d", i, top1(ofm_r[i], o.rpu_sz_ofm));
					for (size_t i = 0; i < ofm_s.size(); ++i)
						std::printf(" strix%zu=%d", i, top1(ofm_s[i], o.strix_sz_ofm));
					std::printf("\n");
				}
				if (f == 0 && !o.save_dir.empty()) {
					for (size_t i = 0; i < ofm_r.size(); ++i)
						write_file(o.save_dir + "/rpu_s" + std::to_string(i) + ".bin", ofm_r[i], o.rpu_sz_ofm);
					for (size_t i = 0; i < ofm_s.size(); ++i)
						write_file(o.save_dir + "/strix_s" + std::to_string(i) + ".bin", ofm_s[i], o.strix_sz_ofm);
				}
				x.post_end = now_ns();
				pl.release(f);
				if (!o.verbose && f > 0 && ((f + 1) % 100 == 0 || f + 1 == o.frames)) {
					std::printf("frame %u done\n", f);
					std::fflush(stdout);
				}
			}
		} catch (const std::exception &e) {
			std::cerr << "final step: " << e.what() << "\n";
			failed = true;
			pl.abort();
		}
	};

	std::vector<std::thread> th;
	if (rpu)
		th.emplace_back(producer, true);
	if (strix)
		th.emplace_back(producer, false);
	th.emplace_back(consumer);
	for (auto &t : th)
		t.join();

	const uint32_t n = pl.consumed();
	if (rpu)
		rpu->teardown();
	std::signal(SIGINT, SIG_DFL);

	report(o, rpu.get(), strix.get(), fs, n, pl.t0());
	if (!o.csv.empty())
		write_csv(o.csv, fs, n, pl.t0());

	bool ok = !failed && n == o.frames && (!rpu || rpu->destroyed);
	for (uint32_t f = 0; f < n; ++f)
		ok &= !fs[f].rpu_mismatch && !fs[f].strix_mismatch;
	std::printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
