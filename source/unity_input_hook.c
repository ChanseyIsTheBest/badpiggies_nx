/* unity_input_hook.c -- feed Switch touch straight into UnityEngine.Input (Color Sheep).
 *
 * Unity 6's nativeInjectEvent path is a dead end here: it ACCEPTS our fake MotionEvent
 * (inject_ret=1) but never reads its coordinates, so Input.touchCount / GetTouch /
 * mousePosition stay empty and the game's UI never sees a tap. Instead we bypass the JNI
 * event system entirely and patch the game's own il2cpp Input methods to return OUR touch
 * state directly. The game reads these every frame, so it sees the Switch touchscreen.
 *
 * RVAs are Color Sheep's (Unity 6000.2.10f1), taken from Il2CppDumper's script.json;
 * runtime addr = il2cpp load_virtbase + RVA (same mapping as every other il2cpp patch).
 * The hook bodies are engine-generic and unchanged from the reference; only the RVAs
 * differ. UnityEngine.Touch's layout was confirmed identical to the reference (0x44 bytes,
 * same field offsets) against dump.cs, so NxTouch transfers verbatim.
 *
 * (The reference's LanguageParam.getCurrentLanguage hook is REMOVED: that is a Layton
 * game-specific class; Color Sheep has no such type.)
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "unity_input_hook.h"

int   debugPrintf(char *fmt, ...);
extern int screen_width, screen_height;
int so_patch_code(void *dst, const void *src, unsigned long len);   /* so_util.c */

/* ---- touch state, written by android_native_feed_hid every frame ----------
 * Multi-touch: g_hook_touch[0..g_hook_count) are the fingers Unity will see this frame
 * (Color Sheep needs several at once -- you hold multiple colour pads together). The
 * mouse-emulation globals mirror the PRIMARY finger so the GetMouseButton* hooks and any
 * mouse-driven UI keep working exactly as before. */
typedef struct { int id; int slot; float x, y; int phase; } HookTouch;
/* id   = raw Switch HID finger_id -- used ONLY to match a finger against last frame.
 * slot = the DENSE 0-based index we report as UnityEngine.Touch.fingerId.
 *
 * These must be kept apart. Horizon assigns HID finger_ids that climb as new fingers
 * land (they are not re-densified), whereas Android's Unity hands out dense 0-based
 * fingerIds. Bad Piggies uses fingerId as an ARRAY INDEX (GuiManager.FindTouch(int
 * touchIdIndex)), so passing a raw HID id straight through threw IndexOutOfRange-
 * Exception out of GuiManager.Update() every frame a finger was down -- which killed
 * touch input entirely. A finger keeps its slot for as long as it stays down; the slot
 * is released only after its Ended frame has been published. */
static HookTouch g_hook_touch[NX_MAX_TOUCH];

/* slot allocator: slot_hid[s] = the HID finger_id holding slot s, or -1 if free */
static int  slot_hid[NX_MAX_TOUCH];
static int  slot_ready = 0;
static void slot_init(void) {
  if (slot_ready) return;
  for (int s = 0; s < NX_MAX_TOUCH; s++) slot_hid[s] = -1;
  slot_ready = 1;
}
static int slot_find(int hid) {
  for (int s = 0; s < NX_MAX_TOUCH; s++) if (slot_hid[s] == hid) return s;
  return -1;
}
static int slot_take(int hid) {                    /* lowest free slot, like Android */
  for (int s = 0; s < NX_MAX_TOUCH; s++) if (slot_hid[s] < 0) { slot_hid[s] = hid; return s; }
  return -1;
}

int   g_hook_count = 0;                 /* Input.touchCount (0..NX_MAX_TOUCH)            */
int   g_hook_btn   = 0;                 /* Input.GetMouseButton(0)  (primary finger)     */
int   g_hook_btn_down = 0, g_hook_btn_up = 0;  /* GetMouseButtonDown/Up(0), 1-frame edges */
int   g_hook_phase = 3;                 /* primary finger's TouchPhase                   */
float g_hook_x = 0.0f, g_hook_y = 0.0f; /* Unity screen space (bottom-left origin, px)   */

/* ---- Unity value types (AArch64 return conventions matter) ---------------- */
typedef struct { float x, y; }    NxV2;
typedef struct { float x, y, z; } NxV3;                 /* HFA -> s0,s1,s2      */
typedef struct {                                        /* UnityEngine.Touch, 0x44 bytes */
  int32_t m_FingerId;       NxV2 m_Position;   NxV2 m_RawPosition; NxV2 m_PositionDelta;
  float   m_TimeDelta;      int32_t m_TapCount; int32_t m_Phase;   int32_t m_Type;
  float   m_Pressure;       float m_maxPressure; float m_Radius;   float m_RadiusVariance;
  float   m_AltitudeAngle;  float m_AzimuthAngle;
} NxTouch;                                               /* > 16 bytes -> sret (x8) */

/* ---- the hooks: il2cpp calling convention = (real args..., MethodInfo*) ---- */
static int32_t hk_touchCount(void *mi){ (void)mi;
  /* If Input.touches is unavailable, report 0: the game bounds its touches[] loops by
   * touchCount, so 0 keeps it from ever indexing an array we cannot build. */
  extern int nx_touches_available(void);
  return nx_touches_available() ? g_hook_count : 0; }
static NxV3    hk_mousePosition(void *mi){ (void)mi; NxV3 v = { g_hook_x, g_hook_y, 0.0f }; return v; }
static uint8_t hk_getMouseButton(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn : 0; }
static uint8_t hk_getMouseButtonDown(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_down : 0; }
static uint8_t hk_getMouseButtonUp(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_up : 0; }
/* One place that turns our finger state into a UnityEngine.Touch, so Input.GetTouch and
 * Input.touches can never disagree about what a touch looks like. */
static void fill_touch(NxTouch *t, const HookTouch *h) {
  memset(t, 0, sizeof *t);
  t->m_FingerId   = h->slot;                   /* DENSE 0-based id (see HookTouch) -- never the raw HID id */
  t->m_Position.x = h->x; t->m_Position.y = h->y;
  t->m_RawPosition = t->m_Position;
  t->m_Phase = h->phase; t->m_TapCount = 1;
  t->m_Pressure = 1.0f; t->m_maxPressure = 1.0f;
}

static NxTouch hk_getTouch(int32_t index, void *mi){
  (void)mi; NxTouch t; memset(&t, 0, sizeof t);
  if (index >= 0 && index < g_hook_count) fill_touch(&t, &g_hook_touch[index]);
  return t;
}

/* ---- UnityEngine.Input.touches -- THE api this game actually uses --------------------
 * Bad Piggies reads Input.touches[i] (bounded by Input.touchCount), NOT Input.GetTouch:
 * GuiManager.TouchInput/FindTouch and the gesture code (getDrag/getDownPos/isGestureDone/
 * isDoubleClickDone) between them call get_touches from 15 sites. With touchCount hooked
 * but touches NOT hooked, the game looped `i < 1` and indexed Unity's real, EMPTY touch
 * array -> IndexOutOfRangeException out of GuiManager.Update() on every frame a finger was
 * down, which is why touch did nothing at all.
 *
 * get_touches returns a managed Touch[], so we must allocate a real il2cpp array.
 * Il2CppArray layout (confirmed against the game's own code in FindTouch:
 *   ldr w8,[x0,#0x18] = max_length ; add x1, x0+i*0x44, #0x20 = &elem[i]):
 *     0x00 klass  0x08 monitor  0x10 bounds  0x18 max_length  0x20 first element
 * Element stride is sizeof(Touch) == 0x44, which is exactly our NxTouch. */
#define IL2CPP_ARRAY_DATA_OFF 0x20

typedef void *(*fn_array_new)(void *klass, uintptr_t length);
typedef uint32_t (*fn_gchandle_new)(void *obj, int pinned);
typedef void *(*fn_domain_get)(void);
typedef void *(*fn_domain_assembly_open)(void *domain, const char *name);
typedef void *(*fn_assembly_get_image)(void *assembly);
typedef void *(*fn_class_from_name)(void *image, const char *ns, const char *name);

static fn_array_new            p_array_new;
static fn_gchandle_new         p_gchandle_new;
static fn_domain_get           p_domain_get;
static fn_domain_assembly_open p_domain_assembly_open;
static fn_assembly_get_image   p_assembly_get_image;
static fn_class_from_name      p_class_from_name;

void nx_input_hook_bind_il2cpp(void *array_new, void *domain_get, void *domain_assembly_open,
                               void *assembly_get_image, void *class_from_name,
                               void *gchandle_new) {
  p_array_new            = (fn_array_new)array_new;
  p_gchandle_new         = (fn_gchandle_new)gchandle_new;
  p_domain_get           = (fn_domain_get)domain_get;
  p_domain_assembly_open = (fn_domain_assembly_open)domain_assembly_open;
  p_assembly_get_image   = (fn_assembly_get_image)assembly_get_image;
  p_class_from_name      = (fn_class_from_name)class_from_name;
}

static void *g_touch_klass = NULL;
static int   g_touches_state = 0;      /* 0 = not tried yet, 1 = ready, -1 = unavailable */

/* Resolve UnityEngine.Touch lazily, on the first get_touches call: by then il2cpp is
 * definitely up and the Unity assemblies are loaded. Touch lives in
 * UnityEngine.InputLegacyModule; the others are fallbacks in case of a different split. */
static void touches_resolve(void) {
  g_touches_state = -1;
  if (!p_array_new || !p_domain_get || !p_domain_assembly_open ||
      !p_assembly_get_image || !p_class_from_name) {
    debugPrintf("[input] Input.touches: il2cpp API not bound -- reporting 0 touches\n");
    return;
  }
  void *dom = p_domain_get();
  if (!dom) { debugPrintf("[input] Input.touches: il2cpp_domain_get() failed\n"); return; }
  static const char *asms[] = { "UnityEngine.InputLegacyModule", "UnityEngine.CoreModule", "UnityEngine" };
  for (unsigned i = 0; i < sizeof asms / sizeof asms[0]; i++) {
    void *a = p_domain_assembly_open(dom, asms[i]);
    if (!a) continue;
    void *img = p_assembly_get_image(a);
    if (!img) continue;
    void *k = p_class_from_name(img, "UnityEngine", "Touch");
    if (k) {
      g_touch_klass = k; g_touches_state = 1;
      debugPrintf("[input] Input.touches: UnityEngine.Touch resolved from %s -> hook live\n", asms[i]);
      return;
    }
  }
  debugPrintf("[input] Input.touches: could NOT resolve UnityEngine.Touch -- reporting 0 touches\n");
}

/* CACHED arrays -- one per possible touch count, allocated at most once each.
 *
 * The naive version allocated a fresh managed array on EVERY call, and the game calls
 * Input.touches a lot while a finger is down: GuiManager.TouchInput and FindTouch, plus the
 * gesture code (Reporter.isGestureDone x5, getDownPos x3, getDrag x2, isDoubleClickDone x2)
 * -- 15 call sites, several of them per frame. That is a managed allocation per call through
 * the public il2cpp_array_new (class-init check + element-size + GC allocate), which on this
 * port is far more expensive than on Android: with the managed GC disabled the Boehm heap
 * grows instead of collecting, and every heap expansion goes out through our mmap shim; when
 * a collection does run, the stop-the-world uses the semaphore bridge in libc_shim.c rather
 * than real POSIX signals. Either way the cost lands in frames where a finger is down -- which
 * is exactly the reported symptom (smooth until you touch the screen, fine on the phone).
 *
 * So: allocate one array per length 0..NX_MAX_TOUCH, keep each alive with a pinned GC handle,
 * and just refill its elements in place on each call. Steady state is ZERO allocation.
 *
 * Returning the same object to repeated callers is safe here: every call site reads the array
 * immediately (index it, read position/phase) and none retains it across frames, and each
 * refill happens before the read. The pin matters because we hold a raw C pointer to a managed
 * object -- without the handle the collector would be free to reclaim it. */
static void    *g_touch_arr[NX_MAX_TOUCH + 1];
static uint32_t g_touch_arr_pin[NX_MAX_TOUCH + 1];

static void *hk_get_touches(void *mi) {
  (void)mi;
  if (g_touches_state == 0) touches_resolve();
  if (g_touches_state != 1) return NULL;          /* hk_touchCount now reports 0, so the
                                                     game's `i < touchCount` loop never runs
                                                     and this is never indexed. */
  int n = g_hook_count;
  if (n < 0) n = 0;
  if (n > NX_MAX_TOUCH) n = NX_MAX_TOUCH;

  void *arr = g_touch_arr[n];
  if (!arr) {                                     /* first time we see this touch count */
    arr = p_array_new(g_touch_klass, (uintptr_t)n);
    if (!arr) return NULL;
    if (p_gchandle_new) g_touch_arr_pin[n] = p_gchandle_new(arr, 1 /* pinned */);
    g_touch_arr[n] = arr;
    debugPrintf("[input] Input.touches: cached array for %d touch(es) (allocated once)\n", n);
  }

  NxTouch *elem = (NxTouch *)((char *)arr + IL2CPP_ARRAY_DATA_OFF);
  for (int i = 0; i < n; i++) fill_touch(&elem[i], &g_hook_touch[i]);
  return arr;
}

/* Overwrite a method entry with an absolute long jump to `target`:
 *   ldr x16, #8 ; br x16 ; .quad target      (16 bytes) */
static void patch_jump(uintptr_t site, void *target) {
  uint32_t code[4];
  code[0] = 0x58000050u;                 /* ldr x16, #8  (load target from site+8) */
  code[1] = 0xd61f0200u;                 /* br  x16                                */
  memcpy(&code[2], &target, sizeof target);
  so_patch_code((void *)site, code, sizeof code);
}

/* Color Sheep Il2CppDumper RVAs (UnityEngine.Input); runtime = il2cpp base + RVA. */
/* Bad Piggies UnityEngine.Input RVAs [VERIFIED] -- from Il2CppDumper (script.json) on this
 * libil2cpp.so + global-metadata.dat; runtime addr = il2cpp base + RVA. Each was confirmed
 * to land on a real function prologue in the shipping binary. The hk_* bodies below are
 * engine-generic and reused unchanged from the colorsheep_nx reference.
 *
 * These are the reliable path (the same one Color Sheep/Layton use): the game reads
 * Input.touchCount / GetTouch / mousePosition / GetMouseButton* every frame, and these
 * hooks return our Switch touch state directly. (2020.3's nativeInjectEvent may also carry
 * coordinates, but hooking Input is deterministic and does no harm if inject works too.) */
#define RVA_get_touchCount        0x0159DAA8u   /* [VERIFIED] */
#define RVA_get_mousePosition     0x0159D7A0u   /* [VERIFIED] */
#define RVA_GetMouseButton        0x0159D514u   /* [VERIFIED] */
#define RVA_GetMouseButtonDown    0x0159D554u   /* [VERIFIED] */
#define RVA_GetMouseButtonUp      0x0159D594u   /* [VERIFIED] */
#define RVA_GetTouch              0x0159D5D4u   /* [VERIFIED] */
#define RVA_get_touches           0x0159DB10u   /* [VERIFIED from dump.cs: Input.get_touches -> Touch[]] */

void nx_install_input_hooks(uintptr_t il2cpp_base) {
  if (!RVA_get_touchCount) {   /* 0 => not derived; fall back to nativeInjectEvent path */
    debugPrintf("[input] il2cpp Input hooks NOT installed (RVAs=0); using inject path\n");
    return;
  }
  patch_jump(il2cpp_base + RVA_get_touchCount,     (void *)hk_touchCount);
  patch_jump(il2cpp_base + RVA_get_mousePosition,  (void *)hk_mousePosition);
  patch_jump(il2cpp_base + RVA_GetMouseButton,     (void *)hk_getMouseButton);
  patch_jump(il2cpp_base + RVA_GetMouseButtonDown, (void *)hk_getMouseButtonDown);
  patch_jump(il2cpp_base + RVA_GetMouseButtonUp,   (void *)hk_getMouseButtonUp);
  patch_jump(il2cpp_base + RVA_GetTouch,           (void *)hk_getTouch);
  patch_jump(il2cpp_base + RVA_get_touches,       (void *)hk_get_touches);
  debugPrintf("[input] il2cpp Input hooks installed (touchCount/touches/GetTouch/mousePosition/GetMouseButton*)\n");
}

/* Called by android_native_feed_hid with EVERY finger currently down, already mapped to Unity
 * screen space (bottom-left origin, game pixels). Phases are derived by matching HID finger ids
 * against last frame: a new id is Began, a known id is Moved (or Stationary if it didn't move),
 * and an id that disappeared is reported for exactly ONE more frame as Ended at its last
 * position -- Unity's contract, and what lets the game see a tap complete.
 * Fingers keep their slot order, so Input.GetTouch(i) is stable across frames. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n) {
  static HookTouch prev[NX_MAX_TOUCH];
  static int prev_n = 0;
  HookTouch cur[NX_MAX_TOUCH];
  int cn = 0;

  slot_init();
  if (n < 0) n = 0;
  if (n > NX_MAX_TOUCH) n = NX_MAX_TOUCH;

  /* 1) every finger that is down now: Began / Moved / Stationary */
  for (int i = 0; i < n; i++) {
    const HookTouch *was = NULL;
    for (int j = 0; j < prev_n; j++)
      if (prev[j].id == in[i].id && prev[j].phase != 3) { was = &prev[j]; break; }

    /* keep this finger's slot if it already has one, else take the lowest free slot */
    int s = slot_find(in[i].id);
    if (s < 0) s = slot_take(in[i].id);
    if (s < 0) continue;                 /* all slots busy: drop the extra finger */

    cur[cn].id   = in[i].id;
    cur[cn].slot = s;
    cur[cn].x  = in[i].x;
    cur[cn].y  = in[i].y;
    if (was) {
      float dx = in[i].x - was->x, dy = in[i].y - was->y;
      cur[cn].phase = (dx*dx + dy*dy > 0.25f) ? 1 /*Moved*/ : 2 /*Stationary*/;
    } else {
      cur[cn].phase = 0 /*Began*/;
    }
    cn++;
  }

  /* 2) fingers that were down last frame and are now gone: one Ended frame each */
  for (int j = 0; j < prev_n && cn < NX_MAX_TOUCH; j++) {
    if (prev[j].phase == 3) continue;              /* already reported Ended -> drop it */
    int still_down = 0;
    for (int i = 0; i < n; i++)
      if (in[i].id == prev[j].id) { still_down = 1; break; }
    if (!still_down) {
      cur[cn] = prev[j];
      cur[cn].phase = 3 /*Ended*/;
      cn++;
    }
  }

  /* 3) publish to the hooks */
  for (int i = 0; i < cn; i++) g_hook_touch[i] = cur[i];
  g_hook_count = cn;

  /* 3b) a finger that just reported Ended is gone -- free its slot so the next finger
   *     down reuses the lowest index, exactly as Android does. Done AFTER publishing,
   *     so the Ended frame still carries the fingerId the game saw while it was down. */
  for (int i = 0; i < cn; i++)
    if (cur[i].phase == 3 && cur[i].slot >= 0 && cur[i].slot < NX_MAX_TOUCH)
      slot_hid[cur[i].slot] = -1;

  { static int logged = 0;
    if (!logged && cn > 0) { logged = 1;
      debugPrintf("[input] first touch: HID finger_id=%d -> Touch.fingerId=%d (densified)\n",
                  cur[0].id, cur[0].slot); } }

  /* 4) mouse emulation mirrors the primary finger (unchanged behaviour for mouse-driven UI) */
  static int prev_active = 0;
  int active = (n > 0);
  g_hook_btn_down = (active && !prev_active);
  g_hook_btn_up   = (!active && prev_active);
  g_hook_btn      = active;
  if (cn > 0) {
    g_hook_x = cur[0].x; g_hook_y = cur[0].y;
    g_hook_phase = cur[0].phase;
  } else {
    g_hook_phase = 3;
  }
  prev_active = active;

  /* 5) remember this frame */
  for (int i = 0; i < cn; i++) prev[i] = cur[i];
  prev_n = cn;
}

/* Single-touch wrapper (stick-cursor / A-button path, which has only one pointer). */
void nx_input_hook_update(int active, float ux, float uy) {
  /* id 1000: deliberately outside the HID finger-id range (0..15) so switching between the
   * stick cursor and a real finger is seen as a new finger (Began), not a jump of an old one. */
  NxTouchIn t = { 1000, ux, uy };
  nx_input_hook_update_multi(active ? &t : NULL, active ? 1 : 0);
}

/* ---- PlayerPrefs persistence ---------------------------------------------
 * Color Sheep saves via UnityEngine.PlayerPrefs, but Unity's native PlayerPrefs never
 * reaches disk on Switch, so progress lived only in RAM. We replace Set/Get/Delete/Save so
 * the game's PlayerPrefs go through our persistent store (unity_jni.c prefs.kv), loaded on
 * boot and flushed to the SD. RVAs are Color Sheep's UnityEngine.PlayerPrefs methods. */
extern void        nx_prefs_set(char type, const char *key, const char *val);   /* unity_jni.c */
extern const char *nx_prefs_get(const char *key);
extern void        nx_prefs_del(const char *key);
extern void        nx_prefs_flush(void);

static void *(*g_il2cpp_string_new)(const char *);

/* il2cpp System.String (arm64): _stringLength @0x10 (int32), _firstChar (UTF-16) @0x14
 * (confirmed against dump.cs). Malloc'd UTF-8 copy (caller frees). */
static char *il2str_dup(void *s) {
  if (!s) return NULL;
  int32_t len = *(int32_t *)((char *)s + 0x10);
  if (len < 0) len = 0;
  char *out = (char *)malloc((size_t)len * 3 + 1);
  if (!out) return NULL;
  const uint16_t *ch = (const uint16_t *)((char *)s + 0x14);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = ch[i];
    if (c < 0x80)        out[o++] = (char)c;
    else if (c < 0x800){ out[o++] = (char)(0xC0|(c>>6));  out[o++] = (char)(0x80|(c&0x3F)); }
    else               { out[o++] = (char)(0xE0|(c>>12)); out[o++] = (char)(0x80|((c>>6)&0x3F)); out[o++] = (char)(0x80|(c&0x3F)); }
  }
  out[o] = 0;
  return out;
}
static void *mkstr(const char *s) { return g_il2cpp_string_new ? g_il2cpp_string_new(s ? s : "") : (void *)0; }

/* il2cpp static-method ABI: (real args..., MethodInfo*). */
static void hk_pp_SetString(void *key, void *val, void *mi) {
  (void)mi; char *k = il2str_dup(key), *v = il2str_dup(val);
  if (k) nx_prefs_set('S', k, v ? v : "");
  free(k); free(v);
}
static void hk_pp_SetInt(void *key, int32_t val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[16];
  if (k) { snprintf(b, sizeof b, "%d", (int)val); nx_prefs_set('I', k, b); }
  free(k);
}
static void hk_pp_SetFloat(void *key, float val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[32];
  if (k) { snprintf(b, sizeof b, "%.9g", (double)val); nx_prefs_set('F', k, b); }
  free(k);
}
static void *hk_pp_GetString2(void *key, void *def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? mkstr(v) : def;
}
static void *hk_pp_GetString1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return mkstr(v ? v : "");
}
static int32_t hk_pp_GetInt2(void *key, int32_t def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : def;
}
static int32_t hk_pp_GetInt1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : 0;
}
static float hk_pp_GetFloat2(void *key, float def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (float)atof(v) : def;
}
static uint8_t hk_pp_HasKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); int h = (k && nx_prefs_get(k)); free(k); return h ? 1 : 0;
}
static void hk_pp_DeleteKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); if (k) nx_prefs_del(k); free(k);
}
static void hk_pp_Save(void *mi) { (void)mi; nx_prefs_flush(); }

/* Bad Piggies UnityEngine.PlayerPrefs RVAs [VERIFIED] (Il2CppDumper script.json; runtime =
 * il2cpp base + RVA). Routes the game's save through our persistent prefs.kv store, since
 * Unity's native PlayerPrefs does not reach disk on Switch. GetString/GetInt each have 1-arg
 * and 2-arg overloads compiled; GetFloat only the 2-arg. DeleteKey is NOT present in this
 * build's metadata (stripped as unused by the game), so it is simply not hooked. */
#define BP_PP_SetString    0x00D8C40Cu
#define BP_PP_SetInt       0x00D8C17Cu
#define BP_PP_SetFloat     0x00D8C320u
#define BP_PP_GetString2   0x00D8C4A8u   /* GetString(key, default) */
#define BP_PP_GetString1   0x00D8C4F8u   /* GetString(key)          */
#define BP_PP_GetInt2      0x00D8C28Cu   /* GetInt(key, default)    */
#define BP_PP_GetInt1      0x00D8C2DCu   /* GetInt(key)             */
#define BP_PP_GetFloat2    0x00D8C3BCu   /* GetFloat(key, default)  */
#define BP_PP_HasKey       0x00D8C570u
#define BP_PP_Save         0x00D8C5E4u
/* DeleteKey: absent in this build's metadata -> not hooked. */

void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new) {
  g_il2cpp_string_new = (void *(*)(const char *))string_new;
  patch_jump(il2cpp_base + BP_PP_SetString, (void *)hk_pp_SetString);
  patch_jump(il2cpp_base + BP_PP_SetInt,    (void *)hk_pp_SetInt);
  patch_jump(il2cpp_base + BP_PP_SetFloat,  (void *)hk_pp_SetFloat);
  if (g_il2cpp_string_new) {   /* GetString builds a String; skip if unresolved (avoid NRE) */
    patch_jump(il2cpp_base + BP_PP_GetString2, (void *)hk_pp_GetString2);
    patch_jump(il2cpp_base + BP_PP_GetString1, (void *)hk_pp_GetString1);
  }
  patch_jump(il2cpp_base + BP_PP_GetInt2,   (void *)hk_pp_GetInt2);
  patch_jump(il2cpp_base + BP_PP_GetInt1,   (void *)hk_pp_GetInt1);
  patch_jump(il2cpp_base + BP_PP_GetFloat2, (void *)hk_pp_GetFloat2);
  patch_jump(il2cpp_base + BP_PP_HasKey,    (void *)hk_pp_HasKey);
  patch_jump(il2cpp_base + BP_PP_Save,      (void *)hk_pp_Save);
  (void)hk_pp_DeleteKey;   /* DeleteKey absent in this build -> unused */
  debugPrintf("[prefs] il2cpp PlayerPrefs hooks installed (Set/Get/Has/Save; DeleteKey absent)\n");
}

/* True once Input.touches can actually be produced (see touches_resolve). Used by
 * hk_touchCount so we never advertise touches the game would then fail to index. */
int nx_touches_available(void) { return g_touches_state >= 0; }

/* Live touch count, for the frame-spike reporter in main.c (tells us whether a hitching
 * frame had a finger down -- the whole question with tap-correlated stutter). */
int nx_hook_touch_count(void) { return g_hook_count; }
