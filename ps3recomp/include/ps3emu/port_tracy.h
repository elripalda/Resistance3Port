/* port_tracy.h -- optional Tracy timeline zones (CMake -DPORT_TRACY=ON).
 *
 * Normal builds do not define PORT_TRACY: every macro below expands to
 * nothing and no Tracy code is compiled or linked. In a Tracy build the
 * functions live in project/port_tracy.cpp. Zones are coarse on purpose:
 *   PT_SYSCALL / PT_HLE  one zone per lv2 syscall / HLE import call (PPU)
 *   PT_SPU_JOB           one WWS job, entry -> return to the policy module
 *   PT_SPU_WAIT          one blocking SPU channel wait
 *   PT_SPIN              a guest spin-wait, merged from its laps (never per lap)
 * Frame marks ("RSX flip") come from a poller thread in port_tracy.cpp. */
#ifndef PORT_TRACY_H
#define PORT_TRACY_H

#include <stdint.h>

enum port_tracy_kind {
    PT_SYSCALL = 0,
    PT_HLE,
    PT_SPU_JOB,
    PT_SPU_WAIT,
    PT_SPIN,
    PT_KIND_COUNT
};

#ifdef PORT_TRACY
#ifdef __cplusplus
extern "C" {
#endif
uint64_t port_tracy_now(void);
/* Live zone on the calling thread; begin/end must pair up on that thread. */
void port_tracy_begin(int kind, uint64_t value);
void port_tracy_end(void);
/* A finished span [t0, t1] (times from port_tracy_now) with no zones inside. */
void port_tracy_span(int kind, uint64_t t0, uint64_t t1, uint64_t value);
/* Called on every lap of a spin loop; laps closer than ~20 us merge into one span. */
void port_tracy_spin_lap(int kind);
#ifdef __cplusplus
}
#endif
#define PT_NOW()               port_tracy_now()
#define PT_BEGIN(kind, value)  port_tracy_begin((kind), (uint64_t)(value))
#define PT_END()               port_tracy_end()
#define PT_SPAN(kind, t0, v)   port_tracy_span((kind), (t0), port_tracy_now(), (uint64_t)(v))
#define PT_SPIN_LAP(kind)      port_tracy_spin_lap(kind)
#else
#define PT_NOW()               0ull
#define PT_BEGIN(kind, value)  ((void)0)
#define PT_END()               ((void)0)
#define PT_SPAN(kind, t0, v)   ((void)0)
#define PT_SPIN_LAP(kind)      ((void)0)
#endif

#endif /* PORT_TRACY_H */
