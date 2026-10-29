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
// A frame is complete when all 2N OFMs are ready; a final step then consumes
// them and releases the frame's buffers. With --fusion-* the final step runs a
// fusion model on the local NPU over all 2N OFMs (each re-laid out on the host
// from its producer's HW layout into the fusion model's IFM layout); with
// --final-* it (also) runs a single-input model N times on static simulated
// IFMs, as one xrt::runlist or N start/wait runs; otherwise it is simulated
// with --post-us.
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
#include "xrt/experimental/xrt_kernel.h"
#include "xrt/experimental/xrt_module.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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

	// Fusion (final step on the local NPU): all 2N sensor OFMs -> one model.
	// Fusion input k = sensor k (0..N-1 = T20 RPU, N..2N-1 = local Strix NPU).
	bool use_fusion = false;
	std::string fusion_xclbin, fusion_elf, fusion_wts, fusion_golden;
	std::vector<int> fusion_arg_ifm;            // [fusion input]
	int fusion_arg_spill = -1, fusion_arg_wts = -1, fusion_arg_ofm = -1, fusion_arg_ctrl = -1;
	uint64_t fusion_sz_ifm = 0, fusion_sz_wts = 0, fusion_sz_spill = 0, fusion_sz_ofm = 0;
	// HW layouts "FMT:d0,d1,..", e.g. HCWNC8:7,256,7,1,8, and the logical feature C,H,W
	std::string rpu_ofm_layout, strix_ofm_layout, fusion_ifm_layout;
	uint32_t feat_c = 2048, feat_h = 7, feat_w = 7;

	// Final runs (final step on the local NPU, after fusion if both): a
	// single-input model run --final-runs times on static simulated IFMs,
	// submitted as one xrt::runlist (default) or as N start/wait runs.
	bool use_final = false, final_runlist = true;
	std::string final_xclbin, final_elf, final_wts;
	std::vector<std::string> final_ifm, final_golden;   // [run]
	uint32_t final_runs = 6;
	uint64_t final_sz_ifm = 200704, final_sz_wts = 26772456, final_sz_spill = 301056, final_sz_ofm = 1000;
	int final_arg_ifm = 3, final_arg_wts = 4, final_arg_spill = 5, final_arg_ofm = 6, final_arg_ctrl = 7;
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
"Fusion step (local NPU, replaces the simulated final step; needs both sides):\n"
"  --fusion-dir DIR     VAIML partition dir: DIR/unified-2x4x4.xclbin, DIR/ctrl.elf,\n"
"                       DIR/wts32.bin (each overridable below)\n"
"  --fusion-xclbin FILE --fusion-elf FILE --fusion-wts FILE\n"
"  --fusion-ifm-args A0,..  kernel-arg index of fusion input k (one per sensor,\n"
"                       2N values; input k = sensor k: 0..N-1 T20, N..2N-1 Strix)\n"
"  --fusion-args S,W,O,C  kernel-arg index of spill,wts,ofm,ctrl-pkt (-1 = none)\n"
"  --fusion-sizes I,W,S,O ifm (each),wts,spill,ofm bytes\n"
"  --rpu-ofm-layout L   --strix-ofm-layout L   --fusion-ifm-layout L\n"
"                       HW layouts \"FMT:d0,d1,..\" (flexmlrt-hsi.json hw_format /\n"
"                       hw_shape), e.g. HCWNC8:7,256,7,1,8 or NCHW:1,2048,7,8;\n"
"                       OFMs are re-laid out into the fusion IFMs on the host\n"
"  --feat-shape C,H,W   logical feature map carried between them (default 2048,7,7)\n"
"  --fusion-golden FILE expected fusion OFM (checked every frame)\n"
"\n"
"Final runs (local NPU, final step, after fusion if both; needs the Strix side):\n"
"  --final-dir DIR      VAIML partition dir of a single-input model: DIR/unified-2x4x4.xclbin,\n"
"                       DIR/ctrl.elf, DIR/wts32.bin (each overridable below)\n"
"  --final-xclbin FILE  --final-elf FILE  --final-wts FILE\n"
"  --final-runs N       runs per frame (default 6)\n"
"  --final-ifm F[,F..]  simulated IFM: one for all runs, or one per run (loaded once)\n"
"  --final-submit M     runlist (default: one xrt::runlist execute + wait per frame)\n"
"                       or runs (N x start, then N x wait)\n"
"  --final-sizes I,W,S,O  ifm,wts,spill,ofm bytes (default 200704,26772456,301056,1000)\n"
"  --final-args I,W,S,O,C kernel-arg index of ifm,wts,spill,ofm,ctrl-pkt (default 3,4,5,6,7)\n"
"  --final-golden F[,F..] expected OFM: one for all runs, or one per run (checked every frame)\n"
"\n"
"Options:\n"
"  --save-out DIR       write frame 0's OFMs to DIR/{rpu,strix}_s<i>.bin\n"
"                       (and DIR/fusion_ofm.bin, DIR/final_r<i>.bin with those steps)\n"
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
	std::string strix_dir, fusion_dir, fusion_ifm_args, final_dir;
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
		else if (a == "--fusion-dir")   fusion_dir = val();
		else if (a == "--fusion-xclbin") o.fusion_xclbin = val();
		else if (a == "--fusion-elf")   o.fusion_elf = val();
		else if (a == "--fusion-wts")   o.fusion_wts = val();
		else if (a == "--fusion-golden") o.fusion_golden = val();
		else if (a == "--fusion-ifm-args") fusion_ifm_args = val();
		else if (a == "--fusion-args") {
			auto v = split(val());
			if (v.size() != 4)
				throw std::runtime_error("--fusion-args takes 4 values");
			o.fusion_arg_spill = std::stoi(v[0]); o.fusion_arg_wts = std::stoi(v[1]);
			o.fusion_arg_ofm = std::stoi(v[2]); o.fusion_arg_ctrl = std::stoi(v[3]);
		}
		else if (a == "--fusion-sizes") {
			auto v = split_num<uint64_t>(val(), 4, "--fusion-sizes");
			o.fusion_sz_ifm = v[0]; o.fusion_sz_wts = v[1]; o.fusion_sz_spill = v[2]; o.fusion_sz_ofm = v[3];
		}
		else if (a == "--rpu-ofm-layout")    o.rpu_ofm_layout = val();
		else if (a == "--strix-ofm-layout")  o.strix_ofm_layout = val();
		else if (a == "--fusion-ifm-layout") o.fusion_ifm_layout = val();
		else if (a == "--feat-shape") {
			auto v = split_num<uint32_t>(val(), 3, "--feat-shape");
			o.feat_c = v[0]; o.feat_h = v[1]; o.feat_w = v[2];
		}
		else if (a == "--final-dir")    final_dir = val();
		else if (a == "--final-xclbin") o.final_xclbin = val();
		else if (a == "--final-elf")    o.final_elf = val();
		else if (a == "--final-wts")    o.final_wts = val();
		else if (a == "--final-runs")   o.final_runs = num();
		else if (a == "--final-ifm")    o.final_ifm = split(val());
		else if (a == "--final-golden") o.final_golden = split(val());
		else if (a == "--final-submit") {
			const std::string m = val();
			if (m != "runlist" && m != "runs")
				throw std::runtime_error("--final-submit: runlist or runs");
			o.final_runlist = m == "runlist";
		}
		else if (a == "--final-sizes") {
			auto v = split_num<uint64_t>(val(), 4, "--final-sizes");
			o.final_sz_ifm = v[0]; o.final_sz_wts = v[1]; o.final_sz_spill = v[2]; o.final_sz_ofm = v[3];
		}
		else if (a == "--final-args") {
			auto v = split(val());
			if (v.size() != 5)
				throw std::runtime_error("--final-args takes 5 values");
			o.final_arg_ifm = std::stoi(v[0]); o.final_arg_wts = std::stoi(v[1]);
			o.final_arg_spill = std::stoi(v[2]); o.final_arg_ofm = std::stoi(v[3]);
			o.final_arg_ctrl = std::stoi(v[4]);
		}
		else if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
		else throw std::runtime_error("unknown option: " + a);
	}
	if (!strix_dir.empty()) {
		if (o.strix_xclbin.empty()) o.strix_xclbin = strix_dir + "/unified-2x4x4.xclbin";
		if (o.strix_elf.empty())    o.strix_elf = strix_dir + "/ctrl.elf";
		if (o.strix_wts.empty())    o.strix_wts = strix_dir + "/wts32.bin";
	}
	if (!fusion_dir.empty()) {
		if (o.fusion_xclbin.empty()) o.fusion_xclbin = fusion_dir + "/unified-2x4x4.xclbin";
		if (o.fusion_elf.empty())    o.fusion_elf = fusion_dir + "/ctrl.elf";
		if (o.fusion_wts.empty())    o.fusion_wts = fusion_dir + "/wts32.bin";
	}
	o.use_fusion = !o.fusion_elf.empty();
	if (o.use_fusion) {
		if (!o.use_rpu || !o.use_strix)
			throw std::runtime_error("the fusion step needs both sides (no --no-rpu / --no-strix)");
		o.fusion_arg_ifm = split_num<int>(fusion_ifm_args, 2 * o.sensors, "--fusion-ifm-args (2N values)");
		if (o.fusion_xclbin.empty() || o.fusion_wts.empty() || o.fusion_arg_wts < 0 ||
		    o.fusion_arg_ofm < 0 || !o.fusion_sz_ifm || !o.fusion_sz_wts || !o.fusion_sz_ofm)
			throw std::runtime_error("fusion step needs --fusion-dir (or xclbin/elf/wts), "
						 "--fusion-args and --fusion-sizes");
		if (o.rpu_ofm_layout.empty() || o.strix_ofm_layout.empty() || o.fusion_ifm_layout.empty())
			throw std::runtime_error("fusion step needs --rpu-ofm-layout, --strix-ofm-layout "
						 "and --fusion-ifm-layout");
	}
	if (!final_dir.empty()) {
		if (o.final_xclbin.empty()) o.final_xclbin = final_dir + "/unified-2x4x4.xclbin";
		if (o.final_elf.empty())    o.final_elf = final_dir + "/ctrl.elf";
		if (o.final_wts.empty())    o.final_wts = final_dir + "/wts32.bin";
	}
	o.use_final = !o.final_elf.empty();
	if (o.use_final) {
		if (!o.use_strix)
			throw std::runtime_error("final runs use the local NPU: needs the Strix side (no --no-strix)");
		if (o.final_xclbin.empty() || o.final_wts.empty() || o.final_ifm.empty() || !o.final_runs)
			throw std::runtime_error("final runs need --final-dir (or xclbin/elf/wts), --final-ifm "
						 "and --final-runs > 0");
		for (auto *v : {&o.final_ifm, &o.final_golden}) {
			if (v->size() == 1)
				v->resize(o.final_runs, (*v)[0]);
			if (!v->empty() && v->size() != o.final_runs)
				throw std::runtime_error("--final-ifm / --final-golden: give 1 or --final-runs files");
		}
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
	// fusion (inside the final step)
	uint64_t fus_pack = 0, fus_exec = 0, fus_d2h = 0;
	uint32_t fusion_mismatch = 0;
	// final runs (inside the final step)
	uint64_t fin_exec = 0, fin_d2h = 0;
	uint32_t final_mismatch = 0;
};

// ---------------------------------------------------------------------------
// HW tensor layouts (flexmlrt-hsi.json hw_format / hw_shape)
// ---------------------------------------------------------------------------
// "HCWNC8:7,256,7,1,8": one token per dim, row-major. N/H/W index the logical
// batch/row/column; the C tokens, outer to inner, are the mixed-radix digits
// of the logical channel. Positions beyond the logical C,H,W are padding.
struct Layout {
	std::string spec;
	std::vector<char> kind;      // 'N' 'H' 'W' 'C'
	std::vector<uint64_t> dim, stride;
	uint64_t bytes = 1;

	explicit Layout(const std::string &s) : spec(s)
	{
		auto colon = s.find(':');
		if (colon == std::string::npos)
			throw std::runtime_error("layout '" + s + "': expected FMT:d0,d1,..");
		const std::string fmt = s.substr(0, colon);
		for (size_t i = 0; i < fmt.size(); ++i) {
			if (!std::strchr("NHWC", fmt[i]))
				throw std::runtime_error("layout '" + s + "': unknown dim '" + fmt[i] + "'");
			kind.push_back(fmt[i]);
			while (i + 1 < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i + 1])))
				++i;                          // block size is given by the shape
		}
		for (auto &d : split(s.substr(colon + 1)))
			dim.push_back(std::stoull(d));
		if (dim.size() != kind.size())
			throw std::runtime_error("layout '" + s + "': " + std::to_string(kind.size()) +
						 " dims in format, " + std::to_string(dim.size()) + " in shape");
		stride.assign(dim.size(), 1);
		for (size_t i = dim.size(); i-- > 0;) {
			stride[i] = bytes;
			bytes *= dim[i];
		}
	}

	uint64_t extent(char k) const
	{
		uint64_t e = 1;
		for (size_t i = 0; i < kind.size(); ++i)
			if (kind[i] == k)
				e *= dim[i];
		return e;
	}

	uint64_t offset(uint64_t c, uint64_t h, uint64_t w) const
	{
		uint64_t off = 0;
		for (size_t i = kind.size(); i-- > 0;) {
			uint64_t idx = 0;
			switch (kind[i]) {
			case 'H': idx = h; break;
			case 'W': idx = w; break;
			case 'C': idx = c % dim[i]; c /= dim[i]; break;
			default: break;                  // N = 0
			}
			off += idx * stride[i];
		}
		return off;
	}
};

// Byte gather table src -> dst for a logical C x H x W int8 feature map.
std::vector<std::pair<uint32_t, uint32_t>> relayout_map(const Layout &src, const Layout &dst,
							 uint32_t c, uint32_t h, uint32_t w)
{
	for (const Layout *l : {&src, &dst})
		if (l->extent('C') < c || l->extent('H') < h || l->extent('W') < w || l->extent('N') != 1)
			throw std::runtime_error("layout " + l->spec + " cannot hold C,H,W " + std::to_string(c) +
						 "," + std::to_string(h) + "," + std::to_string(w));
	std::vector<std::pair<uint32_t, uint32_t>> m;
	m.reserve(static_cast<size_t>(c) * h * w);
	for (uint32_t ci = 0; ci < c; ++ci)
		for (uint32_t hi = 0; hi < h; ++hi)
			for (uint32_t wi = 0; wi < w; ++wi)
				m.emplace_back(static_cast<uint32_t>(src.offset(ci, hi, wi)),
					       static_cast<uint32_t>(dst.offset(ci, hi, wi)));
	return m;
}

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
// Fusion step on the local NPU: 2N sensor OFMs -> one VAIML model
// ---------------------------------------------------------------------------
// The consumer handles one frame at a time, so a single buffer set is enough:
// all kernel args are bound once at setup and every frame only re-lays out the
// 2N OFMs into the fusion IFMs, runs, and reads the OFM back.
struct FusionSide {
	const Opts &o;
	xrt::device dev;
	xrt::hw_context hwctx;
	xrt::elf elf;
	xrt::module mod;
	xrt::kernel kern;
	xrt::run run;
	xrt::bo wts, spill, ofm;
	std::vector<xrt::bo> ifm;                   // [fusion input]
	std::vector<std::pair<uint32_t, uint32_t>> map_rpu, map_strix;
	std::vector<uint8_t> golden;
	bool shared_hwctx = false;

	uint64_t hwctx_ns = 0, elf_ns = 0, wts_ns = 0;

	explicit FusionSide(const Opts &opts) : o(opts) {}

	void setup(StrixSide &strix)
	{
		const Layout rpu_l(o.rpu_ofm_layout), strix_l(o.strix_ofm_layout), fus_l(o.fusion_ifm_layout);
		if (rpu_l.bytes != o.rpu_sz_ofm || strix_l.bytes != o.strix_sz_ofm || fus_l.bytes != o.fusion_sz_ifm)
			throw std::runtime_error("layout sizes (rpu " + std::to_string(rpu_l.bytes) + ", strix " +
						 std::to_string(strix_l.bytes) + ", fusion ifm " + std::to_string(fus_l.bytes) +
						 ") do not match --rpu-ofm / --strix-sizes / --fusion-sizes");
		map_rpu = relayout_map(rpu_l, fus_l, o.feat_c, o.feat_h, o.feat_w);
		map_strix = relayout_map(strix_l, fus_l, o.feat_c, o.feat_h, o.feat_w);

		uint64_t t = now_ns();
		dev = strix.dev;
		xrt::xclbin xclbin{o.fusion_xclbin};
		// raw compare: xrt::uuid's operator== would pull in libuuid
		const xrt::uuid fus_id = xclbin.get_uuid(), strix_id = strix.hwctx.get_xclbin_uuid();
		if (std::memcmp(fus_id.get(), strix_id.get(), sizeof(xuid_t)) == 0) {
			hwctx = strix.hwctx;                // same overlay: share the Strix context
			shared_hwctx = true;
		} else {
			hwctx = xrt::hw_context{dev, dev.register_xclbin(xclbin)};
		}
		hwctx_ns = now_ns() - t;

		t = now_ns();
		auto bytes = load_elf(o.fusion_elf);
		elf = xrt::elf{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
		mod = xrt::module{elf};
		kern = xrt::ext::kernel{hwctx, mod, "DPU"};
		run = xrt::run{kern};
		elf_ns = now_ns() - t;

		t = now_ns();
		wts = xrt::ext::bo{dev, o.fusion_sz_wts};
		{
			auto d = read_file(o.fusion_wts);
			std::memset(wts.map<char *>(), 0, o.fusion_sz_wts);
			std::memcpy(wts.map<char *>(), d.data(), std::min<size_t>(d.size(), o.fusion_sz_wts));
		}
		wts.sync(XCL_BO_SYNC_BO_TO_DEVICE);
		wts_ns = now_ns() - t;

		for (size_t k = 0; k < o.fusion_arg_ifm.size(); ++k) {
			ifm.push_back(xrt::ext::bo{dev, o.fusion_sz_ifm});
			std::memset(ifm.back().map<char *>(), 0, o.fusion_sz_ifm);   // padding stays 0
		}
		ofm = xrt::ext::bo{dev, o.fusion_sz_ofm};
		std::memset(ofm.map<char *>(), 0, o.fusion_sz_ofm);
		ofm.sync(XCL_BO_SYNC_BO_TO_DEVICE);

		run.set_arg(0, 3);
		run.set_arg(1, 0);
		run.set_arg(2, 0);
		for (size_t k = 0; k < ifm.size(); ++k)
			run.set_arg(o.fusion_arg_ifm[k], ifm[k]);
		run.set_arg(o.fusion_arg_wts, wts);
		run.set_arg(o.fusion_arg_ofm, ofm);
		if (o.fusion_arg_spill >= 0 && o.fusion_sz_spill) {
			spill = xrt::ext::bo{dev, o.fusion_sz_spill};
			run.set_arg(o.fusion_arg_spill, spill);
		}
		if (o.fusion_arg_ctrl >= 0)
			run.set_arg(o.fusion_arg_ctrl, 0);

		if (!o.fusion_golden.empty())
			golden = read_file(o.fusion_golden);
	}

	// ofm_r / ofm_s: the frame's N RPU and N Strix OFMs (host memory).
	const uint8_t *frame(const std::vector<const uint8_t *> &ofm_r, const std::vector<const uint8_t *> &ofm_s,
			     FrameStat &fs)
	{
		uint64_t t = now_ns();
		for (size_t k = 0; k < ifm.size(); ++k) {
			const bool from_rpu = k < ofm_r.size();
			const uint8_t *src = from_rpu ? ofm_r[k] : ofm_s[k - ofm_r.size()];
			auto *dst = ifm[k].map<uint8_t *>();
			for (const auto &e : from_rpu ? map_rpu : map_strix)
				dst[e.second] = src[e.first];
			ifm[k].sync(XCL_BO_SYNC_BO_TO_DEVICE);
		}
		fs.fus_pack = now_ns() - t;

		t = now_ns();
		run.start();
		auto state = run.wait(std::chrono::seconds(o.timeout_s));
		if (state != ERT_CMD_STATE_COMPLETED)
			throw std::runtime_error("fusion run state " + std::to_string(static_cast<int>(state)));
		fs.fus_exec = now_ns() - t;

		t = now_ns();
		ofm.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
		fs.fus_d2h = now_ns() - t;

		const auto *out = ofm.map<const uint8_t *>();
		if (!golden.empty())
			fs.fusion_mismatch = std::memcmp(out, golden.data(), std::min<size_t>(o.fusion_sz_ofm, golden.size())) != 0;
		return out;
	}
};

// ---------------------------------------------------------------------------
// Final runs on the local NPU: one single-input model, N runs per frame
// ---------------------------------------------------------------------------
// Simulated workload for the final step: the IFMs are static (filled and
// synced once), every run has its own IFM/spill/OFM BO and its args are bound
// once. Per frame: one xrt::runlist execute + wait (the N runs execute
// atomically, in order) or N x start then N x wait; then the N OFMs are synced
// back and checked.
struct FinalSide {
	const Opts &o;
	xrt::device dev;
	xrt::hw_context hwctx;
	xrt::elf elf;
	xrt::module mod;
	xrt::kernel kern;
	xrt::bo wts;
	std::vector<xrt::bo> ifm, spill, ofm;       // [run]
	std::vector<xrt::run> runs;                 // [run]
	xrt::runlist rlist;
	std::vector<std::vector<uint8_t>> golden;   // [run], empty = no check
	bool shared_hwctx = false;

	uint64_t hwctx_ns = 0, elf_ns = 0, bo_ns = 0;

	explicit FinalSide(const Opts &opts) : o(opts) {}

	void setup(StrixSide &strix)
	{
		uint64_t t = now_ns();
		dev = strix.dev;
		xrt::xclbin xclbin{o.final_xclbin};
		const xrt::uuid id = xclbin.get_uuid(), strix_id = strix.hwctx.get_xclbin_uuid();
		if (std::memcmp(id.get(), strix_id.get(), sizeof(xuid_t)) == 0) {
			hwctx = strix.hwctx;
			shared_hwctx = true;
		} else {
			hwctx = xrt::hw_context{dev, dev.register_xclbin(xclbin)};
		}
		hwctx_ns = now_ns() - t;

		t = now_ns();
		auto bytes = load_elf(o.final_elf);
		elf = xrt::elf{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
		mod = xrt::module{elf};
		kern = xrt::ext::kernel{hwctx, mod, "DPU"};
		elf_ns = now_ns() - t;

		t = now_ns();
		wts = xrt::ext::bo{dev, o.final_sz_wts};
		{
			auto d = read_file(o.final_wts);
			std::memset(wts.map<char *>(), 0, o.final_sz_wts);
			std::memcpy(wts.map<char *>(), d.data(), std::min<size_t>(d.size(), o.final_sz_wts));
		}
		wts.sync(XCL_BO_SYNC_BO_TO_DEVICE);
		for (uint32_t r = 0; r < o.final_runs; ++r) {
			auto d = read_file(o.final_ifm[r]);
			ifm.push_back(xrt::ext::bo{dev, o.final_sz_ifm});
			std::memset(ifm.back().map<char *>(), 0, o.final_sz_ifm);
			std::memcpy(ifm.back().map<char *>(), d.data(), std::min<size_t>(d.size(), o.final_sz_ifm));
			ifm.back().sync(XCL_BO_SYNC_BO_TO_DEVICE);
			ofm.push_back(xrt::ext::bo{dev, o.final_sz_ofm});
			std::memset(ofm.back().map<char *>(), 0, o.final_sz_ofm);
			ofm.back().sync(XCL_BO_SYNC_BO_TO_DEVICE);
			if (o.final_arg_spill >= 0 && o.final_sz_spill)
				spill.push_back(xrt::ext::bo{dev, o.final_sz_spill});

			xrt::run run{kern};
			run.set_arg(0, 3);
			run.set_arg(1, 0);
			run.set_arg(2, 0);
			run.set_arg(o.final_arg_ifm, ifm[r]);
			run.set_arg(o.final_arg_wts, wts);
			run.set_arg(o.final_arg_ofm, ofm[r]);
			if (!spill.empty())
				run.set_arg(o.final_arg_spill, spill[r]);
			if (o.final_arg_ctrl >= 0)
				run.set_arg(o.final_arg_ctrl, 0);
			runs.push_back(run);
		}
		if (o.final_runlist) {
			rlist = xrt::runlist{hwctx};
			for (auto &r : runs)
				rlist.add(r);
		}
		for (auto &p : o.final_golden)
			golden.push_back(read_file(p));
		bo_ns = now_ns() - t;
	}

	void frame(FrameStat &fs)
	{
		uint64_t t = now_ns();
		if (o.final_runlist) {
			rlist.execute();
			if (rlist.wait(std::chrono::seconds(o.timeout_s)) == std::cv_status::timeout)
				throw std::runtime_error("final runlist timeout");
		} else {
			for (auto &r : runs)
				r.start();
			for (size_t i = 0; i < runs.size(); ++i) {
				auto state = runs[i].wait(std::chrono::seconds(o.timeout_s));
				if (state != ERT_CMD_STATE_COMPLETED)
					throw std::runtime_error("final run " + std::to_string(i) + " state " +
								 std::to_string(static_cast<int>(state)));
			}
		}
		fs.fin_exec = now_ns() - t;

		t = now_ns();
		for (auto &b : ofm)
			b.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
		fs.fin_d2h = now_ns() - t;

		for (size_t i = 0; i < golden.size(); ++i)
			fs.final_mismatch += std::memcmp(ofm[i].map<const uint8_t *>(), golden[i].data(),
							 std::min<size_t>(o.final_sz_ofm, golden[i].size())) != 0;
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

void report(const Opts &o, const RpuSide *rpu, const StrixSide *strix, const FusionSide *fusion,
	    const FinalSide *fin, const std::vector<FrameStat> &fs, uint32_t n, uint64_t t0)
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
	if (fusion) {
		one("Fusion xclbin + hw_context", fusion->hwctx_ns, fusion->shared_hwctx ? "shares the Strix hw_context" : "");
		one("Fusion ELF parse + module + kernel", fusion->elf_ns, "args bound once");
		one("Fusion weights BO + sync", fusion->wts_ns);
	}
	if (fin) {
		one("Final xclbin + hw_context", fin->hwctx_ns, fin->shared_hwctx ? "shares the Strix hw_context" : "");
		one("Final ELF parse + module + kernel", fin->elf_ns);
		one("Final BOs + IFMs + runs (+ runlist)", fin->bo_ns, "args bound once, static simulated IFMs");
	}
	if (!n)
		return;

	Agg r_wait, r_h2d, r_exec, r_rexec, r_inf, r_inf_max, r_bind, r_d2h, r_total;
	Agg s_wait, s_h2d, s_set, s_exec, s_inf, s_d2h, s_total;
	Agg r_sync, s_sync, window, start_skew;
	Agg lat, skew, post, held;
	Agg f_pack, f_exec, f_d2h, f_total;
	Agg n_exec, n_inf, n_d2h, n_total;
	uint32_t rpu_late = 0, mism_r = 0, mism_s = 0, mism_f = 0, mism_n = 0;
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
		if (fusion) {
			f_pack.add(x.fus_pack); f_exec.add(x.fus_exec); f_d2h.add(x.fus_d2h);
			f_total.add(x.fus_pack + x.fus_exec + x.fus_d2h);
			mism_f += x.fusion_mismatch;
		}
		if (fin) {
			n_exec.add(x.fin_exec); n_inf.add(static_cast<double>(x.fin_exec) / o.final_runs);
			n_d2h.add(x.fin_d2h); n_total.add(x.fin_exec + x.fin_d2h);
			mism_n += x.final_mismatch;
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
	if (fusion) {
		std::printf("  fusion (%zu sensor OFMs -> 1, local NPU):\n", fusion->ifm.size());
		f_pack.print("re-layout OFMs + IFMs to device", "host gather + BO sync");
		f_exec.print("execution", "start -> wait");
		f_d2h.print("OFM from device", "BO sync");
		f_total.print("fusion total");
	}
	if (fin) {
		std::printf("  final runs (%u x, %s, local NPU):\n", o.final_runs,
			    o.final_runlist ? "one xrt::runlist" : "N start/wait");
		n_exec.print("execution, all runs", o.final_runlist ? "runlist execute -> wait" : "first start -> last wait");
		n_inf.print("model run time / inference", "execution / runs");
		n_d2h.print("OFMs from device", "BO sync");
		n_total.print("final runs total");
	}
	const bool real_final = fusion || fin;
	post.print("final step", real_final ? "fusion / final runs + checks (+ --post-us)" : "simulated (--post-us)");
	held.print("arrival -> OFMs released", real_final ? "latency incl. final step" : "");

	const uint64_t span = fs[n - 1].post_end - std::min(t0, fs[0].arrival);
	std::printf("\n== Throughput: %.2f frames/s, %.1f inferences/s (%u frames in %.3f s)\n",
		    n / (span / 1e9), n * o.sensors * ((rpu ? 1 : 0) + (strix ? 1 : 0)) / (span / 1e9),
		    n, span / 1e9);
	if ((rpu && !rpu->golden.empty()) || (strix && !strix->golden.empty()))
		std::printf("== OFM check: RPU %u mismatching, Strix %u mismatching (of %u frames x %u sensors)\n",
			    mism_r, mism_s, n, o.sensors);
	if (fusion && !fusion->golden.empty())
		std::printf("== Fusion OFM check: %u of %u frames mismatching\n", mism_f, n);
	if (fin && !fin->golden.empty())
		std::printf("== Final runs OFM check: %u mismatching (of %u frames x %u runs)\n", mism_n, n, o.final_runs);
}

void write_csv(const std::string &path, const std::vector<FrameStat> &fs, uint32_t n, uint64_t t0)
{
	std::ofstream f(path);
	auto r = [&](uint64_t t) { return t ? static_cast<int64_t>(t - t0) : -1; };
	f << "frame,arrival,rpu_start,rpu_sync_wait,rpu_slot_wait,rpu_h2d,rpu_exec,rpu_aie_exec,rpu_d2h,rpu_end,"
	     "strix_start,strix_sync_wait,strix_slot_wait,strix_h2d,strix_setarg,strix_exec,strix_d2h,strix_end,"
	     "ready,post_start,post_end,fus_pack,fus_exec,fus_d2h,fin_exec,fin_d2h\n";
	for (uint32_t i = 0; i < n; ++i) {
		const auto &x = fs[i];
		f << i << ',' << r(x.arrival) << ',' << r(x.rpu_start) << ',' << x.rpu_sync_wait << ',' << x.rpu_slot_wait << ','
		  << x.rpu_h2d << ',' << x.rpu_exec << ',' << x.rres.exec_ns << ',' << x.rpu_d2h << ','
		  << r(x.rpu_end) << ',' << r(x.strix_start) << ',' << x.strix_sync_wait << ',' << x.strix_slot_wait << ','
		  << x.strix_h2d << ',' << x.strix_setarg << ',' << x.strix_exec << ',' << x.strix_d2h
		  << ',' << r(x.strix_end) << ',' << r(x.ready) << ',' << r(x.post_start) << ','
		  << r(x.post_end) << ',' << x.fus_pack << ',' << x.fus_exec << ',' << x.fus_d2h << ','
		  << x.fin_exec << ',' << x.fin_d2h << '\n';
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
	std::unique_ptr<FusionSide> fusion;
	std::unique_ptr<FinalSide> final_runs;
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
		if (o.use_fusion) {
			fusion = std::make_unique<FusionSide>(o);
			fusion->setup(*strix);
			std::printf("Fusion kernel created (%zu inputs, %s hw_context; %s -> %s, %s -> %s)\n",
				    fusion->ifm.size(), fusion->shared_hwctx ? "shares the Strix" : "own",
				    o.rpu_ofm_layout.c_str(), o.fusion_ifm_layout.c_str(),
				    o.strix_ofm_layout.c_str(), o.fusion_ifm_layout.c_str());
		}
		if (o.use_final) {
			final_runs = std::make_unique<FinalSide>(o);
			final_runs->setup(*strix);
			std::printf("Final runs created (%u runs per frame as %s, %s hw_context)\n", o.final_runs,
				    o.final_runlist ? "one xrt::runlist" : "N start/wait",
				    final_runs->shared_hwctx ? "shares the Strix" : "own");
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

				auto differs = [](const uint8_t *p, size_t n, const std::vector<uint8_t> &g) {
					return std::memcmp(p, g.data(), std::min(n, g.size())) != 0;
				};
				for (size_t i = 0; i < ofm_r.size() && !rpu->golden.empty(); ++i)
					x.rpu_mismatch += differs(ofm_r[i], o.rpu_sz_ofm, rpu->golden[i]);
				for (size_t i = 0; i < ofm_s.size() && !strix->golden.empty(); ++i)
					x.strix_mismatch += differs(ofm_s[i], o.strix_sz_ofm, strix->golden[i]);

				// Next step: fuse all 2N OFMs of frame f into one output.
				const uint8_t *fused = fusion ? fusion->frame(ofm_r, ofm_s, x) : nullptr;
				if (fused && f == 0 && !o.save_dir.empty())
					write_file(o.save_dir + "/fusion_ofm.bin", fused, o.fusion_sz_ofm);
				if (final_runs)
					final_runs->frame(x);
				if (o.post_us)
					std::this_thread::sleep_for(std::chrono::microseconds(o.post_us));

				if (f == 0 || o.verbose) {
					std::printf("frame %u: top1", f);
					for (size_t i = 0; i < ofm_r.size(); ++i)
						std::printf(" rpu%zu=%d", i, top1(ofm_r[i], o.rpu_sz_ofm));
					for (size_t i = 0; i < ofm_s.size(); ++i)
						std::printf(" strix%zu=%d", i, top1(ofm_s[i], o.strix_sz_ofm));
					if (final_runs)
						for (size_t i = 0; i < final_runs->ofm.size(); ++i)
							std::printf(" final%zu=%d", i,
								    top1(final_runs->ofm[i].map<const uint8_t *>(), o.final_sz_ofm));
					std::printf("\n");
				}
				if (f == 0 && !o.save_dir.empty()) {
					for (size_t i = 0; i < ofm_r.size(); ++i)
						write_file(o.save_dir + "/rpu_s" + std::to_string(i) + ".bin", ofm_r[i], o.rpu_sz_ofm);
					for (size_t i = 0; i < ofm_s.size(); ++i)
						write_file(o.save_dir + "/strix_s" + std::to_string(i) + ".bin", ofm_s[i], o.strix_sz_ofm);
					if (final_runs)
						for (size_t i = 0; i < final_runs->ofm.size(); ++i)
							write_file(o.save_dir + "/final_r" + std::to_string(i) + ".bin",
								   final_runs->ofm[i].map<const uint8_t *>(), o.final_sz_ofm);
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

	report(o, rpu.get(), strix.get(), fusion.get(), final_runs.get(), fs, n, pl.t0());
	if (!o.csv.empty())
		write_csv(o.csv, fs, n, pl.t0());

	bool ok = !failed && n == o.frames && (!rpu || rpu->destroyed);
	for (uint32_t f = 0; f < n; ++f)
		ok &= !fs[f].rpu_mismatch && !fs[f].strix_mismatch && !fs[f].fusion_mismatch && !fs[f].final_mismatch;
	std::printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
