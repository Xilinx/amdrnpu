/* SPDX-License-Identifier: MIT */
/*
 * USDT (SDT) static trace markers for the tessai-agent userspace library.
 *
 * When systemtap-sdt-dev's <sys/sdt.h> is available (TESSAI_AGENT_HAVE_SDT,
 * set by CMake), each marker compiles to a nop sled plus an ELF .note.stapsdt
 * entry that perf discovers:
 *
 *     perf list | grep sdt_tessai_agent
 *     perf probe  sdt_tessai_agent:cmd_submit_enter
 *     perf record -e 'sdt_tessai_agent:*' -- ./app
 *
 * Without the header the markers are no-ops, so the library never hard-depends
 * on systemtap. Provider name is "tessai_agent".
 */
#ifndef TESSAI_AGENT_TRACE_H_
#define TESSAI_AGENT_TRACE_H_

#ifdef TESSAI_AGENT_HAVE_SDT

#include <sys/sdt.h>

#define TESSAI_TRACE0(name)		STAP_PROBE(tessai_agent, name)
#define TESSAI_TRACE1(name, a)		STAP_PROBE1(tessai_agent, name, a)
#define TESSAI_TRACE2(name, a, b)	STAP_PROBE2(tessai_agent, name, a, b)
#define TESSAI_TRACE3(name, a, b, c)	STAP_PROBE3(tessai_agent, name, a, b, c)

#else /* !TESSAI_AGENT_HAVE_SDT */

#define TESSAI_TRACE0(name)		do { } while (0)
#define TESSAI_TRACE1(name, a)		do { (void)(a); } while (0)
#define TESSAI_TRACE2(name, a, b)	do { (void)(a); (void)(b); } while (0)
#define TESSAI_TRACE3(name, a, b, c) \
	do { (void)(a); (void)(b); (void)(c); } while (0)

#endif /* TESSAI_AGENT_HAVE_SDT */

/*
 * Evaluate @val once, fire an exit marker carrying it, and return it. Lets a
 * function with several return points keep a single-line exit at each without
 * losing the returned code. Works identically in the no-op build.
 */
#define TESSAI_TRACE_RET(name, val) \
	do { int _tr = (val); TESSAI_TRACE1(name, _tr); return _tr; } while (0)

#endif /* TESSAI_AGENT_TRACE_H_ */
