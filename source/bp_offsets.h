/* bp_offsets.h -- Bad Piggies (Unity 2020.3.39f1) libunity.so engine offsets.
 *
 * All values are LINK-TIME VADDRs into libunity.so (runtime addr = load_virtbase + off),
 * same convention as the colorsheep_nx / laytonbmr_nx references.
 *
 * ============================ HOW THIS FILE WORKS ============================
 * Every engine patch in bp_patches.c is SELF-VERIFYING and FAIL-SAFE:
 *   - If an offset below is 0, its installer SKIPS the patch entirely (feature off).
 *   - If an offset is set but the guard opcode word(s) at that address don't match,
 *     the installer SKIPS and logs a mismatch (never corrupts the engine).
 * So this file can ship with un-derived offsets = 0: the wrapper still builds and boots
 * as far as it can, and each patch "lights up" only once its offset + guard are filled in
 * and confirmed against THIS binary. A wrong value can never silently corrupt state.
 *
 * ============================ DERIVATION STATUS =============================
 *   [VERIFIED] recovered directly from this binary (opcode words confirmed).
 *   [TODO]     not yet derived for 2020.3.39f1 -- offset left 0 -> installer skips.
 *
 * The engine offsets marked [TODO] are derived with the symbols-kit method
 * (tools/HOWTO_SYMBOLS.md): name the function in a symbolized 2020.3.39f1 reference,
 * fingerprint its constants/prologue, locate the same code in this libunity.so, then
 * read the guard word(s) back out to fill the *_W0 macros. NOTE: the provided symbols
 * kit is for Unity 2022.3.62f2 -- for this 2020.3 build you need a 2020.3.39f1 reference
 * pair (2020.3.39f1 is the most common Unity version in the SoLoader scene, so a
 * symbolized libunity + known offset tables are widely available). See PORT_STATUS.md.
 */
#ifndef BP_OFFSETS_H
#define BP_OFFSETS_H

#include <stdint.h>

/* --- boot / JNI (exported; resolved by NAME at runtime) ----------------------
 * JNI_OnLoad is an exported symbol in this build -> so_try_find_addr_rx("JNI_OnLoad").
 * The UnityPlayer natives (initJni/nativeRender/...) are recovered by name from the
 * captured RegisterNatives table (jni_fake.c), so NO static offset is needed for them.
 *
 * [VERIFIED] fn RVAs below were read straight out of this binary's JNINativeMethod
 * table (name -> signature -> fnptr triples). They are a CROSS-CHECK for the runtime
 * capture -- bp_patches.c/main.c do not depend on them -- but they pin the .text region
 * the natives live in and confirm initJni's 3-arg (Landroid/content/Context;)V shape. */
#define BP_RVA_initJni                     0x3744b4   /* (Landroid/content/Context;)V  [VERIFIED] */
#define BP_RVA_nativeRecreateGfxState      0x374718   /* (ILandroid/view/Surface;)V    [VERIFIED] */
#define BP_RVA_nativeSendSurfaceChanged    0x37477c   /* ()V                           [VERIFIED] */
#define BP_RVA_nativeRender                0x3747c8   /* ()Z                           [VERIFIED] */
#define BP_RVA_nativeInjectEvent           0x37481c   /* (Landroid/view/InputEvent;)Z  [VERIFIED] */
#define BP_RVA_nativeResume                0x3745f4   /* ()V                           [VERIFIED] */
#define BP_RVA_nativePause                 0x37459c   /* ()Z                           [VERIFIED] */
#define BP_RVA_nativeFocusChanged          0x3746b0   /* (Z)V                          [VERIFIED] */
#define BP_RVA_nativeDone                  0x37451c   /* ()Z                           [VERIFIED] */
#define BP_RVA_nativeApplicationUnload     0x37466c   /* ()V                           [VERIFIED] */

/* --- engine clock: TimeManager::Update [VERIFIED] ----------------------------
 * Located in the game binary by fingerprinting the symbolized 2020.3.39f1 reference
 * (libunity_sym.so: TimeManager::Update(double) @ ref 0x427ff4) and matching its
 * invariant prologue into this build. Confirmed byte-identical structure:
 *   entry:  stp d9,d8,[sp,#-0x30]! ; str x20,[sp,#0x10] ; stp x19,x30,[sp,#0x20]
 *           ldr x8,[x0,#0xc8] ; ldr w9,[x0,#0xd0] ; ldrb w10,[x0,#0xf8]     (field bumps)
 *           add x8,#1 ; add w9,#1 ; str x8 ; str w9 ; cbz w10,body ; <epilogue> ; ret
 *   body:   entry+0x3c (mov v8.16b,v0.16b ...), relies on the entry's 0x30 frame.
 * The hook (bp_patches.c) detours entry -> bp_time_update_hook, replays the field
 * bumps + pause-gate, then calls bp_tm_call_body (bp_tm_trampoline.s) which rebuilds
 * the 0x30 frame and branches to body with a live monotonic newTime. */
#define BP_OFF_TimeManager_Update_entry    0x187db4     /* [VERIFIED] game RVA            */
#define BP_OFF_TimeManager_Update_body     0x187df0     /* [VERIFIED] entry + 0x3c        */
#define BP_TM_ENTRY_W0                     0x6dbd23e9u  /* [VERIFIED] stp d9,d8,[sp,#-0x30]! */
#define BP_TM_FIELD_FRAMECOUNT             0xc8         /* [VERIFIED] u64 */
#define BP_TM_FIELD_AUX                    0xd0         /* [VERIFIED] u32 */
#define BP_TM_FIELD_PAUSE                  0xf8         /* [VERIFIED] u8  */

/* --- Choreographer free-run [RESOLVED: no native patch needed on 2020.3] -----
 * Unity 6 (Layton) had a native ChoreographerBase::Get() that the frame loop blocked
 * on, and patched it to `movz x0,#0 ; ret`. That function DOES NOT EXIST in 2020.3 --
 * this generation paces frames with Swappy over the NDK/Java Choreographer, which the
 * substrate already reports as unavailable at two layers, so Swappy falls back to its
 * timer path and never blocks:
 *   - imports.c  : AChoreographer_getInstance()      -> NULL   (NDK path)
 *   - jni_fake.c : Choreographer.getInstance()/Swappy -> null  (Java path)
 *   - imports.c  : pthread_cond_timedwait clock fix           (prevents the Swappy
 *                  condvar hang -- the "~16ms wait becomes infinite" failure)
 * The same-generation ZooKeeper DX port (Unity 2022.3.62f2, identical Swappy stack and
 * identical TimeManager field offsets 0xc8/0xd0/0xf8) ships NO native choreographer
 * patch for exactly this reason. Left at 0 -> installer skips; this is intentional, not
 * a TODO. */
#define BP_OFF_Choreographer_Get           0            /* N/A on 2020.3 (see above) */
#define BP_CHOREO_GET_W0                   0x00000000u  /* N/A */
#define BP_CHOREO_GET_W1                   0x00000000u  /* N/A */

/* --- FrameTimeTracker: Looper/Handler ctor deadlock [VERIFIED -- THE FRAME-2 HANG]
 * This is the actual frame-2 wall, and it is NOT a Choreographer or lost-wakeup problem.
 *
 * Chain (symbolized against a 2020.3.39f1 reference pair; every ret@ in the watchdog
 * dump lands exactly where the reference predicts, incl. EnableFrameTimeTracker+0x58
 * == the insn after `bl RuntimeStaticBase::InitializeImpl`):
 *
 *   UnityPlayerLoop            (0x35e6f4)
 *     UnityPause(int)          (0x35ea88)   <- calls Enable... at 0x35ebd8
 *       EnableFrameTimeTracker (0x36fb20)
 *         RuntimeStaticBase::InitializeImpl (0x298a40)   <- lazily news s_FrameTimeTracker
 *           FrameTimeTracker::FrameTimeTracker(char const*) (0x3709d0)
 *             Looper::Looper / Monitor::Monitor / Looper::Start (0x34bc00)
 *             pthread_mutex_lock ; pthread_cond_wait ; pthread_mutex_unlock
 *
 * The ctor spins up an Android Looper/HandlerThread over JNI and then blocks on a
 * Monitor condvar waiting for that thread to signal "ready". Under fake JNI the
 * HandlerThread is never actually spawned -- the STALL thread list has no Looper
 * thread at all -- so the signal never comes. imports.c's 16ms COND_WAIT_CAP makes
 * this a 62Hz spin instead of a hard park (matching UnityMain's ~382 waits/dump),
 * but the predicate can never become true, so it spins forever. Main therefore never
 * reaches the scene-load queue, which is why PreloadManager / AsyncRead / the job
 * workers all sit idle on their work semaphores -- they are SYMPTOMS, not the cause.
 *
 * Fix: stub EnableFrameTimeTracker() to an immediate `ret`.
 *
 * SAFE: s_FrameTimeTracker is referenced in exactly TWO places in all of libunity --
 * EnableFrameTimeTracker() and DisableFrameTimeTracker() (0x36fbd0) -- and BOTH
 * null-check it. Nothing else dereferences it. Stubbed, the static stays NULL and
 * Disable no-ops. WaitVSync() is unaffected (it reads s_VsyncMonitor, a different
 * static). FrameTimeTracker only feeds FrameTimingManager telemetry, which BP never
 * reads. The stub rets before touching sp/x30, so there is no stack imbalance. */
#define BP_OFF_EnableFrameTimeTracker      0x36fb20     /* [VERIFIED] game RVA               */
#define BP_EFTT_W0                         0xd10083ffu  /* [VERIFIED] sub sp,sp,#0x20 (guard) */

/* --- AndroidVSync presentation counter [N/A on 2020.3] -----------------------
 * The u64 presentation counter that a native AndroidVSync waiter spins on is a Unity-6
 * mechanism (Layton bumped it). 2020.3/2022.3 do not block the player loop on it -- the
 * TimeManager clock hook feeding live time is the whole fix (ZooKeeper 2022.3 precedent:
 * no counter bump). The clock thread's optional bump stays guarded on this being non-zero,
 * so it is inert here. Left 0 intentionally. */
#define BP_OFF_ANDROID_VSYNC_COUNTER       0            /* N/A on 2020.3 (see above) */

/* --- optional stubs (enable only if they block boot on 2020.3) ---------------
 * These addressed Unity-6000.2-specific early-boot NULL derefs (/proc parsing through a
 * not-yet-ready allocator). The 2020.3 allocator is simpler and typically does NOT hit
 * them, so they default OFF. If the boot log shows a fault in a /proc reader or CPU
 * detection, derive the offset here and it self-verifies. */
#define BP_OFF_GetPhysicalMemoryMB         0            /* TODO/optional */
#define BP_PHYSMEM_W0                      0x00000000u
#define BP_PHYSMEM_MB                      2048         /* report if enabled */

#define BP_OFF_GetBigLittleConfiguration   0            /* TODO/optional (may not exist in 2020.3) */
#define BP_GBL_W0                          0x00000000u

/* --- native audio: force OpenSL ES output [APPLIED + VERIFIED] ---------------
 * Unity 2020.3 native audio (no FMOD here) selects its Android output driver via
 * AndroidAudio::GetAndroidAudioOutputType (libunity 0x364c9c). Its sole caller does:
 *     bl GetAndroidAudioOutputType ; cmp w0,#2 ; w21 = (w0==2) ? 22 : 21
 * and feeds w21 to the audio-driver factory, where (verified against the same enum
 * ZooKeeper DX uses) 22 = OpenSL ES and 21 = Java AudioTrack. Our opensles.c shim backs
 * OpenSL; AudioTrack has no JVM consumer on Switch and is SILENT. The function only returns
 * 2 when real-Android AudioManager "OpenSL available" flags are set (globals populated from
 * a live AudioManager); our faked JNI does not set them, so it returns 1 -> AudioTrack ->
 * silence. Fix: force it to return 2 (movz w0,#2 ; ret) so Unity always builds the OpenSL
 * output. AudioTrack is never viable here, so forcing OpenSL is strictly correct and cannot
 * regress. Applied + self-verified in main.c (nx_force_opensl_audio). This is the native-audio
 * analog of ZooKeeper's FMOD_OUTPUTTYPE->OpenSL force; BP has no FMOD, so no FMOD patch. */
#define BP_OFF_AndroidAudioOutputType          0x364c9c
#define BP_AAOT_ENTRY_W0                       0xd10183ffu  /* sub sp,sp,#0x60 (guard) */
#define BP_AAOT_STUB_W0                        0x52800040u  /* movz w0, #2              */
#define BP_AAOT_STUB_W1                        0xd65f03c0u  /* ret                      */

/* Sample-rate / frames-per-buffer are supplied by jni_fake.c getproperty_value (24000/256,
 * the proven ZooKeeper values); the SDL device opens at 48000 and resamples per player, so
 * the reported rate is cosmetic. No getter stub needed. */
#define BP_AUDIO_NATIVE_SAMPLE_RATE            24000
#define BP_AUDIO_NATIVE_FRAMES_PER_BUFFER      256

/* --- frame rate (DISABLED by default) ----------------------------------------
 * Bad Piggies' physics stepping should be verified before forcing 60fps: if any per-frame
 * step isn't scaled by Time.deltaTime, 60fps = double speed (the exact issue Color Sheep hit).
 * Leave 0/disabled until confirmed on hardware. */
#define BP_ENABLE_60FPS                    0
#define BP_OFF_GetVSyncBasedTargetFrameRate 0           /* TODO if BP_ENABLE_60FPS */
#define BP_TARGET_FPS                      60

#endif /* BP_OFFSETS_H */
