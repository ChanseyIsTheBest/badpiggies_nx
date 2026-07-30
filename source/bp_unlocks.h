/* bp_unlocks.h -- optional content unlocks (see bp_unlocks.c). All OFF unless config.txt
 * turns them on. Call once after libil2cpp is loaded and finalized. */
#ifndef BP_UNLOCKS_H
#define BP_UNLOCKS_H
#include <stdint.h>
int bp_install_unlocks(uintptr_t il2cpp_base);
#endif
