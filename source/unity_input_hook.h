/* unity_input_hook.h -- patch the game's il2cpp UnityEngine.Input methods to return our
 * Switch touch state. See unity_input_hook.c. */
#ifndef UNITY_INPUT_HOOK_H
#define UNITY_INPUT_HOOK_H

#include <stdint.h>

/* Bind the exported il2cpp runtime API the Input.touches hook needs to allocate a managed
 * UnityEngine.Touch[]. Call BEFORE nx_install_input_hooks. All five come from
 * so_try_find_addr_rx(&il2cpp_mod, "il2cpp_..."); any may be NULL (the hook then degrades
 * safely to reporting zero touches rather than throwing). */
void nx_input_hook_bind_il2cpp(void *array_new, void *domain_get, void *domain_assembly_open,
                               void *assembly_get_image, void *class_from_name,
                               void *gchandle_new);

/* Install the hooks. Call once after libil2cpp is loaded+finalized (il2cpp load_virtbase). */
void nx_install_input_hooks(uintptr_t il2cpp_base);

/* current number of touches being reported to the game (frame-spike diagnostics) */
int nx_hook_touch_count(void);

/* Route UnityEngine.PlayerPrefs (the game's save) through our persistent prefs.kv store, since
 * Unity's native PlayerPrefs never writes to disk on Switch. string_new = il2cpp_string_new. */
void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new);

/* Max simultaneous fingers we report to Unity (Switch panel tracks up to 16; 10 is plenty). */
#define NX_MAX_TOUCH 10

/* One finger, in Unity screen space (bottom-left origin, game px). `id` is the HID finger id:
 * it must stay stable while the finger is down so phases (Began/Moved/Ended) track correctly. */
typedef struct { int id; float x, y; } NxTouchIn;

/* Push ALL currently-down fingers each frame (n may be 0). Phases are derived by matching
 * finger ids against last frame. This is the multi-touch entry point. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n);

/* Single-touch convenience wrapper (stick-cursor path). Equivalent to update_multi with
 * one finger (id 0) when active, or zero fingers when not. */
void nx_input_hook_update(int active, float ux, float uy);

#endif /* UNITY_INPUT_HOOK_H */
