/* unity_entrypoints.h -- UnityPlayer native method typedefs for Bad Piggies
 * (Unity 2020.3.39f1, arm64-v8a).
 *
 * Like colorsheep_nx, natives are resolved with NO hardcoded offsets:
 *   - JNI_OnLoad is EXPORTED by libunity -> so_try_find_addr_rx(&unity_mod,"JNI_OnLoad").
 *   - initJni / nativeRender / ... are file-local, but libunity's JNI_OnLoad hands
 *     their {name -> fn} table to RegisterNatives, which the fake JNI captures
 *     (jni_fake.c: jni_lookup_unity_native). We recover the runtime pointers by name
 *     after JNI_OnLoad runs -- version-proof, no per-build offset diffing.
 *
 * SIGNATURES BELOW ARE AUTHORITATIVE: recovered directly from this build's
 * JNINativeMethod table (name -> signature-string -> fnptr triples read out of
 * libunity.so's read-only data). fn RVAs are recorded in bp_offsets.h as a cross-check
 * only -- the RegisterNatives capture is the source of truth for the pointers.
 *
 *   native                         signature                       fn RVA
 *   ----------------------------   -----------------------------   ---------
 *   initJni                        (Landroid/content/Context;)V    0x3744b4   <-- 3-ARG
 *   nativeRecreateGfxState         (ILandroid/view/Surface;)V      0x374718
 *   nativeSendSurfaceChangedEvent  ()V                             0x37477c
 *   nativeRender                   ()Z                             0x3747c8
 *   nativeInjectEvent              (Landroid/view/InputEvent;)Z    0x37481c
 *   nativeResume                   ()V                             0x3745f4
 *   nativePause                    ()Z                             0x37459c
 *   nativeFocusChanged             (Z)V                            0x3746b0
 *   nativeDone                     ()Z                             0x37451c
 *   nativeApplicationUnload        ()V                             0x37466c
 *
 * !! KEY DIVERGENCE FROM colorsheep_nx (Unity 6) !!
 *   Unity 6 initJni is (Context,int) -- a 4-arg native. This 2020.3 build's initJni
 *   is (Context) -- a 3-arg native. main.c MUST call initJni(env,thiz,ctx) with NO
 *   trailing int. Passing a 4th arg (as the Unity-6 port does) leaves a garbage value
 *   where the callee expects nothing and can corrupt the frame. fn_initJni reflects the
 *   3-arg form below.
 *
 *   Also: nativeUnityPlayerSetRunning does NOT exist in 2020.3 (Unity-6 lifecycle only).
 *   It is simply absent from the lifecycle here.
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

typedef void     (*fn_initJni)(void*,void*,void*);            /* env,thiz,Context  (3-arg!) */
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);   /* env,thiz,int,Surface       */
typedef void     (*fn_v)(void*,void*);                        /* env,thiz -> void           */
typedef uint8_t  (*fn_z)(void*,void*);                        /* env,thiz -> bool           */
typedef void     (*fn_vz)(void*,void*,int32_t);               /* env,thiz,int -> void        */
typedef uint8_t  (*fn_inject)(void*,void*,void*);             /* env,thiz,InputEvent -> bool */
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);
typedef int      (*fn_jnionload)(void* /*vm*/, void* /*reserved*/);

/* Names as they appear in the UnityPlayer RegisterNatives table. main.c looks each
 * up via jni_lookup_unity_native() after JNI_OnLoad has run. */
#define BP_NATIVE_initJni                  "initJni"
#define BP_NATIVE_nativeRecreateGfxState   "nativeRecreateGfxState"
#define BP_NATIVE_nativeSendSurfaceChanged "nativeSendSurfaceChangedEvent"
#define BP_NATIVE_nativeRender             "nativeRender"
#define BP_NATIVE_nativeInjectEvent        "nativeInjectEvent"
#define BP_NATIVE_nativeResume             "nativeResume"
#define BP_NATIVE_nativePause              "nativePause"
#define BP_NATIVE_nativeFocusChanged       "nativeFocusChanged"
#define BP_NATIVE_nativeDone               "nativeDone"
#define BP_NATIVE_nativeApplicationUnload  "nativeApplicationUnload"

#endif /* UNITY_ENTRYPOINTS_H */
