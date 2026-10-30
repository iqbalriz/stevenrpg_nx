/* su_stubs.c -- the parts of dcr_sea_nx's code that served its C# mod and its
 * IL patches (Disney Crossy Road only): nothing to do for this game. MIT. */
#include <stdint.h>

/* dcr_icall_hooks.c: every dlsym into libmono passes through here first */
void *dcr_ilpatch_interpose(const char *sym, void *real) {
  (void)sym;
  return real;
}

/* dcr_boot.c's frame loop and dcr_main.c's boot */
void dcr_mod_frame(uint64_t frame) { (void)frame; }
void dcr_mod_jitlog_install(void) {}

/* jit_arena.c: the "jitlog" debug aid (a map of the JIT code) was the mod's */
int dcr_jitlog_on(void) { return 0; }
