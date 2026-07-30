/* bp_unlocks.c -- optional content unlocks, patched into the game's own il2cpp code.
 *
 * Bad Piggies gates some content behind in-app purchases. On this port IAP cannot work at all:
 * there is no Google Play billing, and the ads/analytics/Firebase layers are stubs. So the
 * purchase path is not merely inconvenient here, it is absent. These switches let the owner of
 * the game reach content whose assets are already sitting in their own APK
 * (Episode_Sandbox_Levels*.unity3d and friends) on their own console, offline. It is the same
 * idea as a save editor for a single-player game: nothing here touches other players, no
 * encryption is broken, and it changes nothing outside this console's save file.
 *
 * Everything is OFF by default and driven from config.txt.
 *
 * Rather than forge a purchase flag ourselves -- which risks a save state the game's own code
 * never produces -- the preferred switch just re-enables the developer cheat menu that ships in
 * the binary. Its entries are right there in global-metadata.dat:
 *     "Open Cheats", "Unlock Field of Dreams", "Unlock all levels", "Unlock all sandboxes",
 *     "Unlock All Free Levels", "Unlock Little Pig Adventure", ...
 * Picking "Unlock Field of Dreams" from that menu runs Rovio's own unlock routine, so the save
 * ends up exactly as the game expects.
 *
 * Patched functions (RVAs from dump.cs, prologues verified against the shipped libil2cpp):
 *   BuildCustomizationLoader.get_CheatsEnabled  0xC2F1B8   ldr x8,[x0,#0x20] ; ldrb w0,[x8,#0x13]
 *   LevelInfo.IsContentLimited(int,int)         0x7B006C   stp x24,x23,[sp,#-0x40]!
 */
#include <stdint.h>
#include <switch.h>
#include "util.h"
#include "config.h"
#include "so_util.h"
#include "bp_unlocks.h"

#define A64_RET        0xd65f03c0u   /* ret          */
#define A64_MOVZ_W0_1  0x52800020u   /* movz w0, #1  */
#define A64_MOVZ_W0_0  0x52800000u   /* movz w0, #0  */

/* dump.cs RVAs, inside libil2cpp's executable "il2cpp" section (0x6a8ed8..0x15a39a0). */
#define RVA_get_CheatsEnabled    0x00C2F1B8u
#define RVA_GetSandboxUnlocked   0x006FBCE8u

/* First instruction word each site must still have, so we never scribble on the wrong address
 * if the binary is not the build these offsets came from. Mismatch => skip, loudly. */
#define GUARD_CheatsEnabled     0xf9401008u   /* ldr x8, [x0, #0x20]        */
#define GUARD_GetSandboxUnlocked 0xa9be4ff4u  /* stp x20, x19, [sp, #-0x20]! */

static int patch_ret_const(uintptr_t base, uint32_t rva, uint32_t guard,
                           uint32_t retval_insn, const char *what) {
  volatile uint32_t *fn = (volatile uint32_t *)(base + rva);
  if (fn[0] != guard) {
    debugPrintf("[unlock] %s guard mismatch: %08x want %08x -- SKIPPED\n", what, fn[0], guard);
    return 0;
  }
  uint32_t stub[2] = { retval_insn, A64_RET };
  so_patch_code((void *)fn, stub, sizeof stub);
  debugPrintf("[unlock] %s -> return %s  @libil2cpp+0x%x\n",
              what, (retval_insn == A64_MOVZ_W0_1) ? "true" : "false", rva);
  return 1;
}

int bp_install_unlocks(uintptr_t il2cpp_base) {
  int n = 0;

  if (config.cheats) {
    /* KNOWN BROKEN: the flag flips, but the cheat menu's entry does not respond in this
     * build's UI, so "Unlock Field of Dreams" cannot be reached this way. Kept because the
     * patch itself is correct and harmless; use unlock_field_of_dreams instead. */
    /* CheatsEnabled == true unlocks the game's built-in cheat menu, which contains an explicit
     * "Unlock Field of Dreams" entry (plus "Unlock all levels", "Unlock all sandboxes", ...).
     * Using it means Rovio's own code performs the unlock and writes the save. */
    n += patch_ret_const(il2cpp_base, RVA_get_CheatsEnabled, GUARD_CheatsEnabled,
                         A64_MOVZ_W0_1, "BuildCustomizationLoader.CheatsEnabled");
    if (n) debugPrintf("[unlock] cheat flag set -- NOTE: the cheat menu is known not to open in "
                       "this build; use unlock_field_of_dreams=1 instead\n");
  }

  if (config.unlock_field_of_dreams) {
    /* GameProgress.GetSandboxUnlocked(string id) is the read side of the sandbox unlock flags;
     * Field of Dreams is id "S-F". The widely-quoted Assembly-CSharp edit for this game calls
     * GameProgress.SetSandboxUnlocked("S-F", true) from Awake/ChangePlayer -- i.e. it flips the
     * same flag this getter reads. Patching the getter reaches the identical outcome from
     * native code, with no il2cpp managed-invoke and no forged managed string.
     *
     * Caveat, stated plainly: the getter takes the sandbox id as an argument, and returning a
     * constant cannot distinguish "S-F" from the others without a trampoline to reach the
     * original for the rest. So this unlocks ALL sandboxes, not only Field of Dreams. That is a
     * superset of what was asked; if you want strictly S-F, say so and I will add a trampoline
     * that inspects the Il2CppString and defers to the original for every other id. */
    n += patch_ret_const(il2cpp_base, RVA_GetSandboxUnlocked, GUARD_GetSandboxUnlocked,
                         A64_MOVZ_W0_1, "GameProgress.GetSandboxUnlocked (all sandboxes incl. S-F)");
  }


  if (!config.cheats && !config.unlock_field_of_dreams)
    debugPrintf("[unlock] none enabled (set unlock_field_of_dreams=1 in config.txt)\n");
  return n;
}
