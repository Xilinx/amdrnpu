/* SPDX-License-Identifier: MIT */
/*
 * USDT (SDT) static trace markers for the amdrnpu userspace library.
 *
 * When systemtap-sdt-dev's <sys/sdt.h> is available (AMDRNPU_HAVE_SDT,
 * set by CMake), each marker compiles to a nop sled plus an ELF .note.stapsdt
 * entry that perf discovers:
 *
 *     perf list | grep sdt_amdrnpu
 *     perf probe  sdt_amdrnpu:cmd_submit_enter
 *     perf record -e 'sdt_amdrnpu:*' -- ./app
 *
 * Without the header the markers are no-ops, so the library never hard-depends
 * on systemtap. Provider name is "amdrnpu".
 */
#ifndef AMDRNPU_TRACE_H_
#define AMDRNPU_TRACE_H_

#ifdef AMDRNPU_HAVE_SDT

#include <sys/sdt.h>

#define AMDRNPU_TRACE0(name)		STAP_PROBE(amdrnpu, name)
#define AMDRNPU_TRACE1(name, a)		STAP_PROBE1(amdrnpu, name, a)
#define AMDRNPU_TRACE2(name, a, b)	STAP_PROBE2(amdrnpu, name, a, b)
#define AMDRNPU_TRACE3(name, a, b, c)	STAP_PROBE3(amdrnpu, name, a, b, c)

#else /* !AMDRNPU_HAVE_SDT */

#define AMDRNPU_TRACE0(name)		do { } while (0)
#define AMDRNPU_TRACE1(name, a)		do { (void)(a); } while (0)
#define AMDRNPU_TRACE2(name, a, b)	do { (void)(a); (void)(b); } while (0)
#define AMDRNPU_TRACE3(name, a, b, c) \
	do { (void)(a); (void)(b); (void)(c); } while (0)

#endif /* AMDRNPU_HAVE_SDT */

/*
 * Evaluate @val once, fire an exit marker carrying it, and return it. Lets a
 * function with several return points keep a single-line exit at each without
 * losing the returned code. Works identically in the no-op build.
 */
#define AMDRNPU_TRACE_RET(name, val) \
	do { int _tr = (val); AMDRNPU_TRACE1(name, _tr); return _tr; } while (0)

#endif /* AMDRNPU_TRACE_H_ */
