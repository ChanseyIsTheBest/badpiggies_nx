/* bp_patches.c -- Bad Piggies engine patches + native clock (self-verifying).
 *
 * Gets the game past the Unity-on-Android boot walls when there is no Android
 * Choreographer / display vsync to feed:
 *
 *   1. ChoreographerBase::Get() -> NULL, so the frame loop free-runs on our clock
 *      instead of blocking forever on a frame callback that never fires.
 *   2. TimeManager::Update -> detoured to a hook that advances the engine clock from
 *      a monotonic wall clock, so deltaTime is real and async scene-loads finish.
 *   3. A background clock thread keeps the clock advancing while nativeRender blocks
 *      in a synchronous scene-load job, and -- once the AndroidVSync counter offset is
 *      derived -- bumps that counter at ~60Hz so AndroidVSync::WaitForLastPresentation
 *      proceeds at panel rate.
 *
 * Plus a few OPTIONAL stubs (physmem / big.LITTLE / native-audio props) that default
 * OFF (offset 0) and only apply if their 2020.3 offset is filled in and the guard matches.
 *
 * EVERY patch is SELF-VERIFYING + FAIL-SAFE: offset 0 -> skipped; guard mismatch ->
 * skipped + logged; never a partial/corrupting write. Offsets/guards live in bp_offsets.h.
 * so_patch_code(): reference so-loader (MIT). Adapted from colorsheep_nx cs_patches.c,
 * trimmed for Unity 2020.3.39f1 (no FMOD; no U6-specific ProcFS-reader crashes).
 */
#include <stdint.h>
#include <time.h>
#include <switch.h>
#include "so_util.h"
#include "bp_offsets.h"
#include "util.h"          /* install_bionic_tls, BIONIC_TLS_SIZE */
#include "diag.h"

extern volatile int jni_quit_requested;   /* jni_fake.c */

/* AArch64 opcodes we emit */
#define A64_MOVZ_X0_0   0xd2800000u   /* movz x0, #0 (return NULL) */
#define A64_RET         0xd65f03c0u   /* ret                       */

static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- engine clock ---------------------------------------------------------- */
/* Set by the trampoline installer to (unity_base + body RVA); read by bp_tm_trampoline.s. */
uint64_t g_tm_body_target = 0;
/* Frame-correct entry into TimeManager::Update's body (bp_tm_trampoline.s). */
extern void bp_tm_call_body(void *tm, double newTime);

static int g_tm_hooked = 0;
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;                 /* captured TimeManager instance   */
static Mutex   g_clock_lock;
static volatile uint64_t g_last_main_tick_ns = 0;
static volatile uint64_t *g_vsync_counter = NULL;   /* set iff offset derived  */
#define CLOCK_STALL_NS 100000000ULL          /* 100ms main-thread silence == stalled */

/* Re-run TimeManager::Update's body with a live monotonic newTime (seconds since our
 * first tick, matching the engine's seconds-since-startup epoch -> wall-rate delta). */
static void bp_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double newTime = (double)(now - g_clk_base_ns) / 1e9;
  if (g_tm_body_target) bp_tm_call_body(tm, newTime);
}

/* Detour target (installed over the entry): replay the prologue's frameCount++/aux++/
 * pause-gate exactly (fields [VERIFIED] 0xc8/0xd0/0xf8), then tick the body via the
 * frame-correct trampoline. When paused, the stock function just rets -- so do we. */
void bp_time_update_hook(void *tm, double newTime_ignored) {
  (void)newTime_ignored;
  g_tm = tm;
  g_last_main_tick_ns = nx_now_ns();
  *(volatile uint64_t *)((char *)tm + BP_TM_FIELD_FRAMECOUNT) += 1;
  *(volatile uint32_t *)((char *)tm + BP_TM_FIELD_AUX)        += 1;
  if (*(volatile uint8_t *)((char *)tm + BP_TM_FIELD_PAUSE) != 0) return;
  mutexLock(&g_clock_lock);
  bp_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}

static int bp_install_timemanager_clock(uintptr_t ub) {
  if (!BP_OFF_TimeManager_Update_entry || !BP_OFF_TimeManager_Update_body) {
    debugPrintf("[bp] TimeManager clock: offset not derived (0) -- SKIPPED (clock via thread only)\n");
    return 0;
  }
  volatile uint32_t *entry = (volatile uint32_t *)(ub + BP_OFF_TimeManager_Update_entry);
  if (entry[0] != BP_TM_ENTRY_W0) {
    debugPrintf("[bp] TimeManager guard mismatch: %08x want %08x -- clock hook SKIPPED\n",
                entry[0], BP_TM_ENTRY_W0);
    return 0;
  }
  g_tm_body_target = (uint64_t)(ub + BP_OFF_TimeManager_Update_body);
  hook_arm64((uintptr_t)entry, (uintptr_t)&bp_time_update_hook);  /* 16-byte detour; body@+0x3c safe */
  g_tm_hooked = 1;
  if (BP_OFF_ANDROID_VSYNC_COUNTER)
    g_vsync_counter = (volatile uint64_t *)(ub + BP_OFF_ANDROID_VSYNC_COUNTER);
  debugPrintf("[bp] TimeManager::Update hooked @libunity+0x%x (body+0x%x) via trampoline\n",
              BP_OFF_TimeManager_Update_entry, BP_OFF_TimeManager_Update_body);
  return 1;
}

/* ---- Choreographer free-run ------------------------------------------------ */
static int bp_install_choreographer_freerun(uintptr_t ub) {
  if (!BP_OFF_Choreographer_Get) {
    debugPrintf("[bp] Choreographer::Get: offset not derived (0) -- SKIPPED\n");
    return 0;
  }
  volatile uint32_t *get = (volatile uint32_t *)(ub + BP_OFF_Choreographer_Get);
  if (get[0] != BP_CHOREO_GET_W0 || get[1] != BP_CHOREO_GET_W1) {
    debugPrintf("[bp] Choreographer::Get guard mismatch: %08x/%08x want %08x/%08x -- SKIPPED\n",
                get[0], get[1], BP_CHOREO_GET_W0, BP_CHOREO_GET_W1);
    return 0;
  }
  uint32_t patch[2] = { A64_MOVZ_X0_0, A64_RET };
  so_patch_code((void *)get, patch, sizeof patch);
  debugPrintf("[bp] Choreographer::Get -> NULL @libunity+0x%x (freerun)\n", BP_OFF_Choreographer_Get);
  return 1;
}

/* ---- FrameTimeTracker stub (THE frame-2 hang) ------------------------------
 * EnableFrameTimeTracker() lazily constructs s_FrameTimeTracker, whose ctor starts an
 * Android Looper/HandlerThread over JNI and then blocks on a Monitor condvar waiting
 * for that thread to signal ready. Fake JNI never spawns the thread, so the signal
 * never arrives and UnityMain spins in the 16ms-capped cond_wait forever, never
 * reaching the scene-load queue. Stub it to `ret`. See bp_offsets.h for the full
 * chain + the proof that no other code dereferences s_FrameTimeTracker. */
static int bp_install_frametimetracker_stub(uintptr_t ub) {
  if (!BP_OFF_EnableFrameTimeTracker) {
    debugPrintf("[bp] EnableFrameTimeTracker: offset not derived (0) -- SKIPPED\n");
    return 0;
  }
  volatile uint32_t *fn = (volatile uint32_t *)(ub + BP_OFF_EnableFrameTimeTracker);
  if (fn[0] != BP_EFTT_W0) {
    debugPrintf("[bp] EnableFrameTimeTracker guard mismatch: %08x want %08x -- SKIPPED\n",
                fn[0], BP_EFTT_W0);
    return 0;
  }
  uint32_t stub[1] = { A64_RET };
  so_patch_code((void *)fn, stub, sizeof stub);
  debugPrintf("[bp] EnableFrameTimeTracker -> ret @libunity+0x%x (Looper/Monitor deadlock)\n",
              BP_OFF_EnableFrameTimeTracker);
  return 1;
}

/* ---- optional: GetPhysicalMemoryMB -> const (skip /proc/meminfo parse) ------ */
static int bp_install_physmem_stub(uintptr_t ub) {
  if (!BP_OFF_GetPhysicalMemoryMB) return 0;
  volatile uint32_t *fn = (volatile uint32_t *)(ub + BP_OFF_GetPhysicalMemoryMB);
  if (fn[0] != BP_PHYSMEM_W0) {
    debugPrintf("[bp] GetPhysicalMemoryMB guard mismatch: %08x want %08x -- SKIPPED\n",
                fn[0], BP_PHYSMEM_W0);
    return 0;
  }
  uint32_t stub[2] = { 0x52800000u | ((uint32_t)BP_PHYSMEM_MB << 5), A64_RET }; /* movz w0,#MB ; ret */
  so_patch_code((void *)fn, stub, sizeof stub);
  debugPrintf("[bp] GetPhysicalMemoryMB -> %d MB @libunity+0x%x\n", BP_PHYSMEM_MB, BP_OFF_GetPhysicalMemoryMB);
  return 1;
}

/* ---- optional: GetBigLittleConfiguration -> empty (uniform cores) ----------- */
static int bp_install_biglittle_stub(uintptr_t ub) {
  if (!BP_OFF_GetBigLittleConfiguration) return 0;
  volatile uint32_t *fn = (volatile uint32_t *)(ub + BP_OFF_GetBigLittleConfiguration);
  if (fn[0] != BP_GBL_W0) {
    debugPrintf("[bp] GetBigLittleConfiguration guard mismatch: %08x want %08x -- SKIPPED\n",
                fn[0], BP_GBL_W0);
    return 0;
  }
  uint32_t stub[3] = { 0xd2800060u /* movz x0,#3 */, 0xd28000e1u /* movz x1,#7 */, A64_RET };
  so_patch_code((void *)fn, stub, sizeof stub);
  debugPrintf("[bp] GetBigLittleConfiguration -> 3-core config @libunity+0x%x\n", BP_OFF_GetBigLittleConfiguration);
  return 1;
}

/* ---- native audio ----------------------------------------------------------
 * No engine patch here. BP uses Unity NATIVE audio (no FMOD); the OpenSL-vs-AudioTrack
 * selection is forced to OpenSL in main.c (nx_force_opensl_audio), and sample-rate/frames
 * come from jni_fake.c getproperty_value (24000/256, resampled to 48000 by opensles.c).
 * The earlier "stub the native-output getters to constants" plan proved unnecessary. */

/* Install all currently-derived libunity engine patches. Returns count applied. */
int bp_install_engine_patches(uintptr_t unity_base) {
  int n = 0;
  n += bp_install_choreographer_freerun(unity_base);
  n += bp_install_timemanager_clock(unity_base);
  n += bp_install_frametimetracker_stub(unity_base); /* THE frame-2 hang */
  n += bp_install_physmem_stub(unity_base);          /* optional (off unless derived) */
  n += bp_install_biglittle_stub(unity_base);        /* optional (off unless derived) */
  return n;
}

/* ---- background clock thread ----------------------------------------------- */
static Thread g_clock_thr;
static void bp_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  uint64_t vsync_last_ns = 0;
  while (!jni_quit_requested) {
    svcSleepThread(4000000ULL);              /* ~4ms poll */
    if (g_vsync_counter) {
      uint64_t now = nx_now_ns();
      if (!vsync_last_ns) vsync_last_ns = now;
      while ((int64_t)(now - vsync_last_ns) >= 16666667LL) {
        __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
        vsync_last_ns += 16666667ULL;
      }
    }
    /* Drive the engine clock only while the main thread is parked in a synchronous
     * scene-load (else the per-frame hook owns the clock). trylock: never invert order. */
    void *tm = g_tm;
    if (tm && g_tm_hooked && g_tm_body_target &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      bp_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}

void bp_start_clock_thread(void) {
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, bp_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
}
