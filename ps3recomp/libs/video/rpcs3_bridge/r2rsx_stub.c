/* Fallback for builds that do not link the RPCS3 RSX library (rpcs3_rsx with
 * r2rsx_bridge.cpp): RPCS3 mode is simply unavailable. */
#ifndef PS3RECOMP_HAVE_RPCS3_RSX
#include "r2rsx_bridge.h"

int  r2rsx_enabled(void) { return 0; }
void r2rsx_set_window(void* hwnd, uint32_t w, uint32_t h) { (void)hwnd; (void)w; (void)h; }
int  r2rsx_context_allocate(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }
void r2rsx_iomap(uint32_t ea, uint32_t io, uint32_t size) { (void)ea; (void)io; (void)size; }
void r2rsx_iounmap(uint32_t io, uint32_t size) { (void)io; (void)size; }
void r2rsx_set_display_buffer(uint32_t id, uint32_t o, uint32_t p, uint32_t w, uint32_t h)
{ (void)id; (void)o; (void)p; (void)w; (void)h; }
void r2rsx_set_flip_mode(uint32_t mode) { (void)mode; }
uint32_t r2rsx_get_flip_status(void) { return 0; }
void r2rsx_set_flip_status(uint32_t s) { (void)s; }
uint64_t r2rsx_last_flip_time(void) { return 0; }
uint64_t r2rsx_vblank_count(void) { return 0; }
uint32_t r2rsx_current_display_buffer(void) { return 0; }
void r2rsx_set_flip_handler(uint32_t opd) { (void)opd; }
void r2rsx_set_vblank_handler(uint32_t opd) { (void)opd; }
void r2rsx_set_user_handler(uint32_t opd) { (void)opd; }
void r2rsx_set_queue_handler(uint32_t opd) { (void)opd; }
void r2rsx_set_tile(uint32_t i, uint32_t l, uint32_t o, uint32_t s, uint32_t p, uint32_t c,
                    uint32_t b, uint32_t k, int bound)
{ (void)i; (void)l; (void)o; (void)s; (void)p; (void)c; (void)b; (void)k; (void)bound; }
void r2rsx_set_zcull(uint32_t i, uint32_t o, uint32_t w, uint32_t h, uint32_t cs, uint32_t zf,
                     uint32_t af, uint32_t zd, uint32_t zcf, uint32_t sf, uint32_t sr,
                     uint32_t sm, int bound)
{ (void)i; (void)o; (void)w; (void)h; (void)cs; (void)zf; (void)af; (void)zd; (void)zcf;
  (void)sf; (void)sr; (void)sm; (void)bound; }
void r2rsx_wait_get(uint32_t put, unsigned max_ms) { (void)put; (void)max_ms; }
void r2rsx_prof_enable(int on) { (void)on; }
int  r2rsx_prof_sample(double* f, double* d, double* u) { (void)f; (void)d; (void)u; return 0; }
#endif
