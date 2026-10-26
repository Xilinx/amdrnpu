#!/bin/bash
# SPDX-License-Identifier: MIT
#
# perf views of where sets_host_trace spends time, in user space and in the
# kernel (MDB5 client driver + dw-edma/rave2-hdma), on the x86 host.
#
# Usage (as root: perf_event_paranoid is 4 on this Ubuntu kernel):
#   sudo ./perf-sets.sh summary  -- ./sets_host_trace --ini ... --rounds 20 ...
#   sudo ./perf-sets.sh profile  -- ./sets_host_trace --ini ... --rounds 20 ...
#   sudo ./perf-sets.sh timeline -- ./sets_host_trace --ini ... --rounds 3  ...
#   sudo ./perf-sets.sh utimeline -- ./sets_host_trace --ini ... --rounds 3 ...
#
#   summary   perf trace -s: per syscall count / total / min / avg / max, i.e.
#             how long each DMA pread/pwrite takes in the kernel, blocking
#             (waiting for the DMA interrupt) included.
#   profile   perf record -g (DWARF unwind) of on-CPU time, user and kernel,
#             then a report split by binary/kernel/module and by symbol. Only
#             time on the CPU is sampled; time blocked waiting for a DMA is
#             what "summary" and "timeline" show.
#   timeline  perf record of: the program's own step markers (trace_marker ->
#             ftrace:print), kprobes on the MDB5 client read/write path and
#             the dw-edma completion callback, uprobes on the host's DMA and
#             rpmsg send functions, IRQ entry/exit and context switches. Writes
#             a time-ordered text timeline with perf script.
#   utimeline user-space only: uprobes on the application, nothing in the
#             kernel (no kprobes, no IRQ/sched tracepoints, no ftrace markers,
#             not system-wide). One uprobe on sets_host_trace's tr() records
#             every step's begin/end with name, set, arg, address and size,
#             plus entry/return of the host's MDB5 DMA calls (mdb5_read /
#             mdb5_write, i.e. one pread/pwrite each) and rpmsg_lite_dma_sendto.
#             --no-marker is added to the command so the app does not write
#             to the ftrace buffer either.
#
# Output goes to ./perf-sets.<mode>.* in the current directory.

set -u

mode=${1:-}
shift || true
[ "${1:-}" = "--" ] && shift
if [ -z "$mode" ] || [ $# -eq 0 ]; then
	sed -n '4,34p' "$0"
	exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "run as root (sudo): perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)" >&2
	exit 2
fi

app=$1
out=perf-sets.$mode
tracefs=/sys/kernel/tracing

case "$mode" in
summary)
	perf trace -s -o "$out.txt" -- "$@"
	rc=$?
	echo "== per-syscall summary ($out.txt), DMA = pread64 (from device) / pwrite64 (to device):"
	grep -E "syscall|-----|pread64|pwrite64|write |nanosleep|openat" "$out.txt" | head -20
	exit $rc
	;;

profile)
	perf record -F 4999 -g --call-graph dwarf,16384 -o "$out.data" -- "$@"
	rc=$?
	perf report -i "$out.data" --stdio --no-children --sort dso > "$out.by-dso.txt" 2>/dev/null
	perf report -i "$out.data" --stdio --no-children --sort dso,sym -g none \
		--percent-limit 0.5 > "$out.by-sym.txt" 2>/dev/null
	echo "== on-CPU time by binary / kernel / module ($out.by-dso.txt):"
	grep -E "^ +[0-9]" "$out.by-dso.txt" | head -12
	echo "== top symbols ($out.by-sym.txt):"
	grep -E "^ +[0-9]" "$out.by-sym.txt" | head -25
	echo "full call graphs: perf report -i $out.data"
	exit $rc
	;;

timeline)
	# Same clock for perf and the program's CLOCK_MONOTONIC step markers.
	echo mono > "$tracefs/trace_clock" 2>/dev/null

	probes=()
	add_probe() {   # add_probe <perf probe args...>; skip quietly if it fails
		if perf probe -q "$@" 2>/dev/null; then
			probes+=("$*")
		else
			echo "  (skip probe: $*)"
		fi
	}
	echo "== adding probes"
	# Kernel: MDB5 client read/write syscalls, the per-transfer SG path, and
	# the dw-edma completion callback (DMA done, IRQ/tasklet context).
	for f in mdb5_dma_ops_read mdb5_dma_ops_write mdb5_dma_sgdma_single_xfer \
		 mdb5_dma_io_req_prepare.constprop.0; do
		add_probe -a "$f"
		add_probe -a "$f%return"
	done
	add_probe -a amd_mdb5_dma_comp_result_cb
	add_probe -a rave2_hdma_pci_address
	# User space: the host's MDB5 pread/pwrite wrappers and the rpmsg send.
	for f in mdb5_read mdb5_write rpmsg_lite_dma_sendto; do
		add_probe -x "$app" -a "$f"
		add_probe -x "$app" -a "$f%return"
	done

	events=(-e ftrace:print -e irq:irq_handler_entry -e irq:irq_handler_exit
		-e sched:sched_switch)
	perf list 2>/dev/null | grep -q '^  probe:' && events+=(-e 'probe:*')
	perf list 2>/dev/null | grep -q '^  probe_' && events+=(-e 'probe_*:*')

	perf record -a -k mono "${events[@]}" -o "$out.data" -- "$@"
	rc=$?

	perf script -i "$out.data" -F comm,pid,cpu,time,event,trace > "$out.txt" 2>/dev/null
	echo "== timeline: $out.txt ($(wc -l < "$out.txt") events). Host steps are the"
	echo "   ftrace:print 'sets r=<round> B|E <step>' lines; kernel probes and IRQs"
	echo "   are interleaved by timestamp. First lines:"
	head -40 "$out.txt"

	echo "== removing probes"
	perf probe -q -d 'probe:*' 2>/dev/null
	perf probe -q -d 'probe_*:*' 2>/dev/null
	exit $rc
	;;

utimeline)
	case " $* " in *" --no-marker "*) ;; *) set -- "$@" --no-marker ;; esac
	grp=probe_$(basename "$app" | tr -c 'A-Za-z0-9_\n' '_')
	perf probe -q -d "$grp:*" 2>/dev/null   # leftovers from an aborted run

	echo "== adding user-space probes to $app"
	ok=0
	# One probe on the trace layer: every step's B/E/I with its arguments.
	perf probe -q -x "$app" -a 'tr ph name:string set arg addr bytes' && ok=1 ||
		echo "  (skip probe: tr - is $app built from sets_host_trace.c with -g?)"
	for f in mdb5_read mdb5_write rpmsg_lite_dma_sendto; do
		perf probe -q -x "$app" -a "$f" 2>/dev/null || echo "  (skip probe: $f)"
		perf probe -q -x "$app" -a "$f%return" 2>/dev/null || echo "  (skip probe: $f%return)"
	done
	[ "$ok" -eq 1 ] || { perf probe -q -d "$grp:*" 2>/dev/null; exit 1; }

	perf record -k mono -e "$grp:*" -o "$out.data" -- "$@"
	rc=$?

	perf script -i "$out.data" -F comm,pid,time,event,trace > "$out.txt" 2>/dev/null
	echo "== user-space timeline: $out.txt ($(wc -l < "$out.txt") events)."
	echo "   $grp:tr = step begin/end (ph 0=begin 1=end 2=instant), name, set, arg, addr, bytes;"
	echo "   mdb5_read/mdb5_write(+__return) = each DMA pread/pwrite. First lines:"
	head -40 "$out.txt"

	echo "== removing probes"
	perf probe -q -d "$grp:*" 2>/dev/null
	exit $rc
	;;

*)
	echo "unknown mode '$mode' (summary | profile | timeline | utimeline)" >&2
	exit 2
	;;
esac
