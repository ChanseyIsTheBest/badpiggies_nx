/* config.h -- Bad Piggies Nintendo Switch wrapper configuration.
 *
 * Forked from the colorsheep_nx / laytonbmr_nx / vln_nx SoLoader lineage (MIT).
 * The loader-tuning constants (heap split, mmap arena, overcommit window) are
 * engine-generation properties and are inherited UNCHANGED. Only the game-identity
 * and path constants below are Bad-Piggies-specific.
 *
 * Target: Bad Piggies 2.4.3297, Unity 2020.3.39f1 / IL2CPP / arm64-v8a.
 * No PAIRIP, no FMOD (Unity native audio), single split-AssetBundle layout.
 *
 * MIT license -- see LICENSE.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Newlib heap for the engine/libc++/il2cpp managed heaps; the rest -> .so loader.
 * 2020.3 is a lighter engine than Unity 6; the inherited split is comfortable. */
#define MEMORY_MB 768

/* mmap arena. Unity reserves aligned pools by over-mmapping then trimming head/tail;
 * we back anon mmaps from an aligned arena with a per-page bitmap so sub-range munmap
 * frees only trimmed pages. ALIGN must match Unity's region granularity.
 * NOTE: stock 2020.3 uses 256MB regions; until region_patch.h is derived for this
 * build the arena is aligned to 64MB (harmless) but the granularity patch is a no-op
 * (8GB Switch only). See region_patch.h + PORT_STATUS.md milestone R. */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)1792 * 1024 * 1024)  /* heap-backed cap (28x64MB) */

/* Stack-region overcommit arena (libc_shim.c). Engine-generic; inherited. */
#define OC_WINDOW_BYTES     ((size_t)1536 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 384 * 1024 * 1024)
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* --- inherited SoLoader leftovers (unused by Unity, kept for base parity) --- */
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

/* --- Bad Piggies game identity (JNI Context shim: getPackageName/versionCode) ---
 * These keep the CS_* macro names the inherited jni_fake.c / unity_jni.c reference,
 * with Bad-Piggies values. Package confirmed from resources.arsc: com.rovio.BadPiggies.
 * (A Talkweb/Chinese distribution variant also exists; if getPackageName gating ever
 * matters, confirm against the exact APK's AndroidManifest -- see PORT_STATUS milestone J.) */
#define CS_PACKAGE       "com.rovio.BadPiggies"
#define CS_VERSION_NAME  "2.4.3297"
#define CS_VERSION_CODE  3297         /* TODO: confirm real APK versionCode if it matters */
#define CS_APP_GUID      "00000000-0000-0000-0000-000000000000"  /* TODO: unity_app_guid if needed */

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "sdmc:/switch/badpiggies_nx/debug.log"

/* Game data root == the .nro's own folder (SoLoader SD convention):
 * nro + libs + assets all live in sdmc:/switch/badpiggies_nx/. */
#define GAME_HOME   "sdmc:/switch/badpiggies_nx"

/* on-hardware file logging (debug.log). Set to 0 for release builds. */
#define DEBUG_LOG 0

/* FORCED render resolution -- applied in main.c in both docked and handheld mode.
 * Change these two numbers if you ever want a different fixed resolution; touch
 * scaling and DPI reporting both derive from screen_width/screen_height, so they
 * follow automatically and need no edits. */
#define BP_FORCE_SCREEN_W 1920
#define BP_FORCE_SCREEN_H 1080

extern int screen_width;
extern int screen_height;

typedef struct {
  char language[16];    /* "auto" => follow Switch system language, else ISO code e.g. "fr" */
  int  cheats;          /* 1 => enable the game's built-in cheat menu (has "Unlock Field of
                         *      Dreams"). IAP cannot work on this port at all. */
  int  unlock_field_of_dreams; /* 1 => GetSandboxUnlocked always true (unlocks S-F + all sandboxes) */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
