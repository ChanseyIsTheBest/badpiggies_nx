/* main.c -- Bad Piggies Nintendo Switch wrapper entry point.
 *
 * Unity 2020.3.39f1 / IL2CPP, arm64-v8a. Loads libmain + libunity + libil2cpp,
 * resolves their imports against native Switch implementations, and drives the
 * lifecycle the Java UnityPlayer normally runs (JNI_OnLoad -> initJni -> recreate
 * GFX -> surface changed -> resume/focus -> render loop). The UnityPlayer natives
 * are recovered BY NAME from libunity's RegisterNatives table (jni_fake.c), so no
 * fragile per-function offsets are needed. Engine patches (clock, freerun) live in
 * bp_patches.c and are keyed on bp_offsets.h.
 *
 * Forked from colorsheep_nx / laytonbmr_nx / vln_nx (MIT). The memory/heap/overcommit
 * scaffolding below is inherited verbatim (engine-generation-generic). Bad-Piggies
 * deltas vs the Color Sheep (Unity 6) reference:
 *   - engine version 2020.3.39f1 (older engine; offsets in bp_offsets.h re-derived);
 *   - initJni is 3-ARG (env,thiz,Context) here, NOT the Unity-6 4-arg (Context,int);
 *   - nativeUnityPlayerSetRunning does not exist in 2020.3 (dropped from lifecycle);
 *   - NO FMOD (Unity native audio) -> no FMOD setOutput patch needed;
 *   - Play-Games boot-gate removed (Color-Sheep-specific il2cpp RVA); Bad Piggies'
 *     ads/social init is handled benignly by the fake JNI (see PORT_STATUS milestone S);
 *   - region-granularity 256MB->64MB patch DERIVED + VERIFIED (region_patch.h, 20 sites) --
 *     until re-derived; TODO items flagged inline and in PORT_STATUS.md.
 */
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "libc_shim.h"
#include "bp_unlocks.h"
#include "nx_fonts.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "region_patch.h"
#include "unity_input_hook.h"
#include "diag.h"
#include "bp_offsets.h"

#define DATA_ROOT  GAME_HOME
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* bp_patches.c */
int  bp_install_engine_patches(uintptr_t unity_base);
void bp_start_clock_thread(void);
/* jni_fake.c: recover a UnityPlayer native by name from the captured RegisterNatives table */
void *jni_lookup_unity_native(const char *name);
/* unity_glue.c */
void unity_environment_init(const char *data_root);

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* Strong override of nx_crash_handler.c's weak stub: name addresses that fall inside
 * our loaded .so images (creport can't, since they aren't real modules). */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap, uintptr_t *base_out) {
  const struct { const char *n; so_module *m; } mods[] = {
    { "libmain.so", &main_mod }, { "libunity.so", &unity_mod }, { "libil2cpp.so", &il2cpp_mod },
  };
  for (unsigned i = 0; i < sizeof(mods)/sizeof(*mods); i++) {
    uintptr_t b = (uintptr_t)mods[i].m->load_virtbase;
    if (b && addr >= b && addr < b + mods[i].m->load_size) {
      snprintf(name_out, name_cap, "%s", mods[i].n);
      *base_out = b;
      return 1;
    }
  }
  return 0;
}

extern uintptr_t g_il2cpp_base;       /* libc_shim.c: GC stop-the-world bridge */
extern size_t    g_il2cpp_size;       /* libc_shim.c: bounds guard for the same */
extern void nx_sd_flush(void);        /* libc_shim.c: periodic SD commit       */

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

/* libunity ~19M + libil2cpp ~48M + headroom for relocated segments */
#define SO_REGION_BYTES (240u * 1024 * 1024)

/* ==========================================================================
 * Inherited memory/heap/overcommit scaffolding (verbatim from the vln/cr3_nx
 * base, MIT). Engine-generation-generic; not Color-Sheep-specific.
 * ========================================================================== */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* ==========================================================================
 * Color-Sheep-specific: data layout, module load, region no-op.
 * ========================================================================== */

/* Bad Piggies 2.4.3297 ships data.unity3d (UnityFS) + sharedassets*.resource +
 * sharedassets0.resource + the il2cpp metadata. NO OBB, NO split levels. All staged
 * flat under assets/bin/Data/ by tools/stage_sd.py. */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/data.unity3d",
    "assets/bin/Data/sharedassets0.resource",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/boot.config",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see README.md).\n"
                  "Run tools/stage_sd.py to assemble it from your own APK(s).", files[i]);
  }
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* Region-granularity 256MB->64MB patch (region_patch.h, 20 verified sites). Needed on a 4GB
 * Switch (256MB block reservations don't fit); harmless on 8GB. Self-verifying + all-or-
 * nothing: any `from` mismatch patches NOTHING (stock 256MB), never a partial/corrupting mix. */
static int nx_patch_unity_regions(uintptr_t ub) {
  int bad = 0;
  for (int i = 0; i < BP_REGION_PATCH_N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + BP_REGION_PATCH[i].off);
    if (cur != BP_REGION_PATCH[i].from) {
      debugPrintf("[region] mismatch[%d] @+0x%x: have 0x%08x want 0x%08x\n",
                  i, (unsigned)BP_REGION_PATCH[i].off, cur, BP_REGION_PATCH[i].from);
      bad++;
    }
  }
  if (bad) {   /* all-or-nothing: a partial patch would mix 256MB/64MB paths -> corruption */
    debugPrintf("[region] %d/%d sites mismatched -> patching NOTHING (stock 256MB regions)\n",
                bad, BP_REGION_PATCH_N);
    return 0;
  }
  for (int i = 0; i < BP_REGION_PATCH_N; i++)
    so_patch_code((void *)(ub + BP_REGION_PATCH[i].off),
                  &BP_REGION_PATCH[i].to, sizeof BP_REGION_PATCH[i].to);
  debugPrintf("[region] granularity 256MB->64MB patched in-memory (%d sites)\n", BP_REGION_PATCH_N);
  return BP_REGION_PATCH_N;
}

/* Force Unity native audio onto OpenSL ES. AndroidAudio::GetAndroidAudioOutputType must
 * return 2 for the audio-driver factory to build an OpenSL(22) output (our opensles.c shim);
 * otherwise it builds a Java AudioTrack(21) which is silent on Switch. Our faked JNI doesn't
 * set the "OpenSL available" flags the function checks, so it returns 1 -> AudioTrack. Overwrite
 * its entry with `movz w0,#2 ; ret`. Self-verified against the entry word; skips on any mismatch
 * (stock/updated build left intact). Full trace in bp_offsets.h. */
static int nx_force_opensl_audio(uintptr_t ub) {
  volatile uint32_t *p = (volatile uint32_t *)(ub + BP_OFF_AndroidAudioOutputType);
  if (*p != BP_AAOT_ENTRY_W0) {
    debugPrintf("[audio] GetAndroidAudioOutputType guard mismatch @+0x%x: 0x%08x != 0x%08x -> NOT forcing OpenSL\n",
                (unsigned)BP_OFF_AndroidAudioOutputType, *p, BP_AAOT_ENTRY_W0);
    return 0;
  }
  uint32_t stub[2] = { BP_AAOT_STUB_W0, BP_AAOT_STUB_W1 };  /* movz w0,#2 ; ret */
  so_patch_code((void *)p, stub, sizeof stub);
  debugPrintf("[audio] forced native output to OpenSL ES (GetAndroidAudioOutputType -> 2)\n");
  return 1;
}

/* engine entry points (resolved by name from RegisterNatives, post-JNI_OnLoad) */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;
/* (Unity 6's nativeUnityPlayerSetRunning does not exist in 2020.3.) */

/* Save persistence: commit the SD periodically. (Color Sheep saves via PlayerPrefs;
 * the managed PlayerPrefs flush hook is a TODO -- see below -- but committing the SD
 * still persists whatever reached the prefs file.) */
static AppletHookCookie g_applet_cookie;
static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnExitRequest)
    nx_sd_flush();
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  debugPrintf("[boot] === badpiggies_nx start (Unity 2020.3.39f1) ===\n");

  /* CWD fix: title-override leaves cwd at the .nro folder or SD root; Unity reads many
   * files via relative paths, so chdir into DATA_ROOT. */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_data = stat("assets/bin/Data/data.unity3d", &st) == 0;
    debugPrintf("[boot] cwd '%s' -> chdir(%s)=%d; reachable(rel): metadata=%d data.unity3d=%d\n",
                cwd, DATA_ROOT, rc, reach_meta, reach_data);
  }

  /* config.txt (render resolution) -- create with defaults on first run. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    if (read_config(cfg) != 0) write_config(cfg);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20), (unsigned)((tot - used) >> 20));
  }

  /* Stack-region overcommit arena. Any failure -> heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool);
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole -> heap-backed only\n");
    }
  }

  /* Render resolution: FORCED to 1920x1080 in EVERY mode (docked and handheld).
   *
   * Docked is natively 1080p. In handheld the panel is 720p, so the compositor
   * downscales our 1080p buffer to it -- i.e. we supersample, which costs GPU but
   * never looks worse than rendering 720p directly.
   *
   * config.txt's screen_width/height is deliberately IGNORED here. That is the whole
   * point of "no matter what": a config.txt left on the SD card from an earlier build
   * (or written back by write_config) could otherwise pin the game to 1280x720 and
   * silently defeat this.
   *
   * TOUCH NEEDS NO SPECIAL CASE. The Switch touch panel always reports in a FIXED
   * 1280x720 coordinate space regardless of what we render at, and
   * android_native_feed_hid scales panel -> screen_width/screen_height, so touch
   * follows the render resolution automatically (here: x1.5 on both axes).
   * (Touch only exists in handheld -- a docked Switch has no touchscreen at all.) */
  screen_width  = BP_FORCE_SCREEN_W;
  screen_height = BP_FORCE_SCREEN_H;
  debugPrintf("[gfx] boot mode=%s render=%dx%d (FORCED); touch panel 1280x720 -> scale x%.2f/%.2f\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height,
              (double)screen_width / 1280.0, (double)screen_height / 720.0);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays from older
   * builds, plus the single hidden scratch the probe is now redirected to (libc_shim.c
   * casetest_redirect). Prevents junk accumulating on the SD card each launch. */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, DATA_ROOT "/%s", de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  debugPrintf("[boot] ===== badpiggies_nx build %s %s =====\n", __DATE__, __TIME__);
  debugPrintf("[boot] fixes: ftt-stub gc-offsets asset-redirect touch-hooks 1080p rename-posix "
              "map-dedup fd-cache(.so-only) synth-proc-cache cjk-fonts+opendir\n");
  /* Extract the console's shared fonts and expose them where Unity's dynamic font fallback
   * looks (/system/fonts + fonts.xml). MUST run before the modules load: libunity reads the
   * font config while building its fallback list, and an empty list is exactly why the
   * Simplified Chinese banner text rendered blank while the Latin UI was fine. */
  nx_fonts_init(GAME_HOME);

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_size = il2cpp_mod.load_size;
  debugPrintf("[boot] libmain=%p libunity=%p libil2cpp=%p\n",
              main_mod.load_virtbase, unity_mod.load_virtbase, il2cpp_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Engine patches (bp_patches.c): Choreographer freerun + TimeManager clock.
   * Both self-verifying against bp_offsets.h. */
  {
    int n = bp_install_engine_patches((uintptr_t)unity_mod.load_virtbase);
    debugPrintf("[boot] engine patches applied: %d/8\n", n);
  }

  /* Region granularity (no-op until re-derived; 8GB Switch runs fine at 256MB). */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  /* --- AUDIO: force Unity native audio onto OpenSL ES --------------------------
   * Bad Piggies 2020.3 uses Unity's NATIVE audio (no FMOD), so there is no FMOD patch.
   * Native audio picks OpenSL(22, our opensles.c shim) vs Java AudioTrack(21, silent on
   * Switch) by AndroidAudio::GetAndroidAudioOutputType returning 2. Our faked JNI doesn't
   * set the "OpenSL available" flags it checks, so it would return 1 -> AudioTrack -> silence.
   * Force it to return 2 (self-verified). See bp_offsets.h for the full trace. */
  nx_force_opensl_audio((uintptr_t)unity_mod.load_virtbase);

  /* --- BOOT GATES: none pinned yet ---------------------------------------------
   * The Color-Sheep Play-Games Authenticate->ret gate was keyed to a Color-Sheep il2cpp
   * RVA and does not apply here. Bad Piggies' ads/social/billing init (Rovio "beacon",
   * AppLovin, Talkweb) is routed through the fake JNI, which hands back benign opaque
   * objects. If any native init blocks boot, pin the offending il2cpp method from dump.cs
   * and stub it here, self-verified (see PORT_STATUS milestone S). */

  /* Main thread runs init_array + the engine lifecycle; give it a stable bionic TLS. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST (exported symbol): runs
   * jni::Initialize() (caches the JavaVM) AND registers the UnityPlayer natives,
   * which our fake RegisterNatives captures for by-name resolution below. */
  {
    fn_jnionload Unity_JNI_OnLoad =
      (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!Unity_JNI_OnLoad) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] calling libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. We call libil2cpp's exported
   * JNI_OnLoad by name (simpler + version-proof vs. hardcoding the VM-global RVAs
   * the reference replicated). TODO: if this mis-binds (a PLT log call at its head
   * bound wrong), fall back to replicating the two VM-global stores directly --
   * derive their RVAs from this libil2cpp's JNI_OnLoad. */
  {
    fn_jnionload Il2cpp_JNI_OnLoad =
      (fn_jnionload)so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (Il2cpp_JNI_OnLoad) {
      debugPrintf("[boot] calling libil2cpp JNI_OnLoad(fake_vm)...\n");
      Il2cpp_JNI_OnLoad(fake_vm, NULL);
    } else {
      debugPrintf("[boot] WARNING: libil2cpp JNI_OnLoad not found (managed JNI may fail)\n");
    }
  }

  /* Resolve the UnityPlayer natives BY NAME from the captured RegisterNatives table. */
  #define RESOLVE_NATIVE(var, cast, name) do {                         \
      var = (cast)jni_lookup_unity_native(name);                       \
      if (!(var)) fatal_error("UnityPlayer native not captured: %s", name); \
    } while (0)
  RESOLVE_NATIVE(Unity_initJni,                  fn_initJni,  BP_NATIVE_initJni);
  RESOLVE_NATIVE(Unity_nativeRecreateGfxState,   fn_gfxstate, BP_NATIVE_nativeRecreateGfxState);
  RESOLVE_NATIVE(Unity_nativeSendSurfaceChanged, fn_v,        BP_NATIVE_nativeSendSurfaceChanged);
  RESOLVE_NATIVE(Unity_nativeRender,             fn_z,        BP_NATIVE_nativeRender);
  RESOLVE_NATIVE(Unity_nativeResume,             fn_v,        BP_NATIVE_nativeResume);
  RESOLVE_NATIVE(Unity_nativeFocusChanged,       fn_vz,       BP_NATIVE_nativeFocusChanged);
  RESOLVE_NATIVE(Unity_nativeDone,               fn_z,        BP_NATIVE_nativeDone);
  RESOLVE_NATIVE(Unity_nativeApplicationUnload,  fn_v,        BP_NATIVE_nativeApplicationUnload);
  #undef RESOLVE_NATIVE
  debugPrintf("[boot] natives resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  install_bionic_tls(main_tls);

  /* Unity 2020.3 initJni is 3-ARG (env,thiz,Context) -- NO trailing int (unlike U6). */
  debugPrintf("[boot] initJni(ctx)...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface changed; resume + focus\n");

  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true\n");

  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* CRITICAL ORDER: disable the Boehm GC + install the clock BEFORE the first
   * nativeRender. First-frame managed allocs can trigger a GC whose stop-the-world
   * uses POSIX signals Switch never delivers -> nativeRender would never return. */
  {
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) { il2cpp_gc_set_mode(1); debugPrintf("[boot] il2cpp_gc_set_mode(DISABLED)\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
    if (il2cpp_gc_disable)  { il2cpp_gc_disable();   debugPrintf("[boot] il2cpp_gc_disable() -> GC OFF\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found\n");
  }

  /* il2cpp method hooks (Bad Piggies RVAs [VERIFIED] from Il2CppDumper -- unity_input_hook.c):
   *  - UnityEngine.Input.* : return our Switch touch state each frame (touchCount / GetTouch /
   *    mousePosition / GetMouseButton*); the deterministic path used by this lineage.
   *  - UnityEngine.PlayerPrefs.* : route the game's save through our persistent prefs.kv,
   *    since Unity's native PlayerPrefs never reaches disk on Switch.
   * All RVAs were confirmed to land on real function prologues in this libil2cpp.so. */
  /* Input.touches needs to allocate a managed UnityEngine.Touch[], so hand the hook the
   * exported il2cpp runtime API it needs. Must precede nx_install_input_hooks. */
  bp_install_unlocks((uintptr_t)il2cpp_mod.load_virtbase);

  nx_input_hook_bind_il2cpp(
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_array_new"),
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get"),
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_assembly_open"),
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_assembly_get_image"),
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_from_name"),
      (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gchandle_new"));

  nx_install_input_hooks((uintptr_t)il2cpp_mod.load_virtbase);
  nx_install_playerprefs_hooks((uintptr_t)il2cpp_mod.load_virtbase,
                               (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_string_new"));

  bp_start_clock_thread();
  debugPrintf("[boot] GC off + clock thread started; entering render loop\n");

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");
  diag_watchdog_start();

  int frame = 0;
  uint64_t next_frame_ns = 0;
  {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    next_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid();

    /* Time the engine's frame. When one blows the budget, print what happened DURING it --
     * the per-frame delta of the shim counters -- plus whether a finger was down. A tap that
     * hitches will show its own cause here (sd=1 -> an SD commit, mmap=N -> heap growth or a
     * file map, gc=N -> a stop-the-world, open=N -> a burst of file opens). Costs one
     * clock_gettime pair per frame and prints nothing on a healthy frame. */
    struct timespec _t0, _t1; clock_gettime(CLOCK_MONOTONIC, &_t0);
    unsigned _o0 = nx_stat_open, _c0 = nx_stat_commit, _m0 = nx_stat_mmap, _g0 = nx_stat_gcstop;

    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;

    clock_gettime(CLOCK_MONOTONIC, &_t1);
    {
      long _ms = (long)((_t1.tv_sec - _t0.tv_sec) * 1000 + (_t1.tv_nsec - _t0.tv_nsec) / 1000000);
      if (_ms >= 20) {   /* >20ms = a visibly dropped frame at 60fps */
        debugPrintf("[perf] frame %d took %ldms  open=%u sd=%u mmap=%u gc=%u  touch=%d\n",
                    frame, _ms,
                    nx_stat_open - _o0, nx_stat_commit - _c0,
                    nx_stat_mmap - _m0, nx_stat_gcstop - _g0,
                    nx_hook_touch_count());
      }
    }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
    if ((frame % 120) == 0) nx_sd_flush();   /* commit any pending save ~every 2s */
    /* FRAME LIMITER (~60fps). No real display vsync -> pace to 16.6ms so we don't
     * flood the compositor (an unthrottled present flood crashes vi). */
    next_frame_ns += 16666667ULL;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if ((int64_t)(next_frame_ns - now) > 0) svcSleepThread(next_frame_ns - now);
    else next_frame_ns = now;
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
