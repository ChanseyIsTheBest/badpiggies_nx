/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height */
#include "unity_input_hook.h"

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 720, g_h = 1280;   /* fbstub45 PORTRAIT (stable) */
extern int screen_width, screen_height;   /* the render resolution main.c resolved (config/auto) */

void android_native_update_mode(void){
  /* The NWindow buffer MUST match the resolution Unity renders/reports (screen_width/height),
   * or the game renders at one size into a buffer of another -> zoom/crop. main.c sets those
   * from config.txt or the 1440x2560 default; mirror them here. */
  if (screen_width > 0 && screen_height > 0)                   { g_w = (u32)screen_width; g_h = (u32)screen_height; }
  else if (appletGetOperationMode() == AppletOperationMode_Console) { g_w = 1080; g_h = 1920; }
  else                                                         { g_w = 720;  g_h = 1280; }
}
u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented. */
/* LANDSCAPE, NO ROTATION. Layton renders landscape (confirmed on hardware), so the buffer
 * (bw x bh, 16:9) maps 1:1 to the landscape panel with the identity transform -- no compositor
 * rotation. (The port originally assumed portrait and rotated 90deg, which was the whole
 * "zoomed/tiny" bug.) */
/* Remember the last geometry so we can re-assert it (see nx_window_reassert). */
static u32 g_geom_bw = 1920, g_geom_bh = 1080;

static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  g_geom_bw = bw; g_geom_bh = bh;
  nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0u);   /* identity: landscape buffer -> landscape panel */
  static int nlog = 0;
  if (nlog < 6) {
    nlog++;
    u32 aw = 0, ah = 0;
    nwindowGetDimensions(w, &aw, &ah);
    debugPrintf("[gfx] set_geom: render %ux%u, nwindow reports %ux%u, transform=0 (landscape)\n",
                bw, bh, aw, ah);
  }
}

/* switch-mesa creates its swapchain inside eglCreateWindowSurface and may reset the NWindow's
 * crop/dimensions to its own defaults; re-assert our crop (and identity transform) afterwards
 * so only the rendered content is presented. Cheap: two property writes. */
void nx_window_reassert(void) {
  NWindow *w = nwindowGetDefault();
  nwindowSetCrop(w, 0, 0, g_geom_bw, g_geom_bh);
  nwindowSetTransform(w, 0u);
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  if (width > 0 && height > 0) nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
/* Unity 6's InitializeUILooper() does `if (ALooper_forThread()==NULL) { log
 * "Couldn't retrieve native ALooper for UI thread"; return; }` -- and leaves the
 * global NdkLooper singleton NULL. A later NdkLooper::CreateHandler() then calls
 * WaitForCreation() on that null pointer -> Data Abort (ldrb [null+0xe8]). On real
 * Android the UI thread already owns a Java-created looper; we have none, so return
 * a create-on-demand looper (like ALooper_pollOnce already does) so the NdkLooper
 * is constructed (its ctor sets the [+0xe8] "created" flag WaitForCreation reads). */
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 1); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID polling -> Unity input.
 * Unity ingests input through the Java UnityPlayer (touch -> nativeInjectEvent /
 * key path). The exact native event struct is engine-internal: recover the
 * registered "injectEvent"/"nativePointer*" method from libunity's JNI_OnLoad
 * (PORTING_PLAN.md S4) and fill in feed_one_touch(). Until then this reads HID
 * but doesn't yet hand it to the engine.
 * ========================================================================== */
/* HID -> Unity input. The Switch touchscreen passes straight through (all fingers) into
 * il2cpp UnityEngine.Input via the hooks in unity_input_hook.c.
 *
 * On top of that there is an optional VIRTUAL CURSOR (ported from the Laytonbmr port):
 * the left stick moves an on-screen dot, A presses it as a touch. It is drawn by the GL
 * overlay in imports.c, in the SAME bottom-left game-pixel space the touch is injected in,
 * so the dot lands exactly where A taps.
 *
 *   +  (Plus)  -> show the cursor
 *   -  (Minus) -> hide the cursor
 *
 * Default: ON when docked, OFF in handheld -- a docked Switch has no touch panel, so the
 * cursor is the only pointer there; in handheld the finger is better and a dot would just
 * clutter. Pressing +/- once overrides that default for the rest of the session.
 *
 * The cursor tap is fed as just another finger, with a reserved id that cannot collide with
 * a real HID finger_id, so it coexists with real touches and gets a dense fingerId from the
 * slot allocator like everything else. */

#define NX_CURSOR_TOUCH_ID 1000            /* reserved: real HID finger_ids are small (0..15) */

static PadState g_pad;
static HidTouchScreenState g_touch;
static float g_last_tx = 360, g_last_ty = 640;    /* last touch (game space), reused on release */

static float g_cursor_x = 960.0f, g_cursor_y = 540.0f;  /* game px, bottom-left origin */
static int   g_cursor_enabled  = 0;
static int   g_cursor_user_set = 0;               /* has the player pressed +/- yet? */

/* Consumed by the GL overlay in imports.c. Same space the touch is injected in. */
int   nx_cursor_show = 0;
float nx_cursor_x = 960.0f, nx_cursor_y = 540.0f;

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
}

void android_native_feed_hid(void){
  padUpdate(&g_pad);
  const float PANEL_W = 1280.0f, PANEL_H = 720.0f;

  /* Touchscreen -- ALL fingers, not just the first.
   *
   * The Switch touch panel ALWAYS reports in a fixed 1280x720 top-left space, no matter
   * what resolution we render at. Unity wants game pixels with a BOTTOM-LEFT origin. So we
   * scale panel -> (screen_width, screen_height) and flip Y. Because both factors are
   * derived from screen_width/screen_height, this tracks the forced 1920x1080 render
   * resolution automatically (x1.5 on each axis) -- and would track any other value too.
   *
   * Guard the degenerate case: if the resolution has not been resolved yet, fall back to
   * the panel space 1:1 rather than collapsing every touch onto (0,0). */
  const float SW = (screen_width  > 0) ? (float)screen_width  : PANEL_W;
  const float SH = (screen_height > 0) ? (float)screen_height : PANEL_H;
  const float sx = SW / PANEL_W, sy = SH / PANEL_H;

  NxTouchIn tin[NX_MAX_TOUCH];
  int tn = 0;
  int n = hidGetTouchScreenStates(&g_touch, 1);
  if (n > 0 && g_touch.count > 0){
    for (u32 i = 0; i < g_touch.count && tn < NX_MAX_TOUCH; i++){
      float px = (float)g_touch.touches[i].x, py = (float)g_touch.touches[i].y;
      float gx = px * sx;              /* panel px -> game px            */
      float gy = SH - py * sy;         /* flip Y: top-left -> bottom-left */
      /* Clamp: a touch right on the panel edge can map a hair outside the render
       * target, and a game that indexes a grid by pixel does not enjoy that. */
      if (gx < 0.0f) gx = 0.0f;  if (gx > SW - 1.0f) gx = SW - 1.0f;
      if (gy < 0.0f) gy = 0.0f;  if (gy > SH - 1.0f) gy = SH - 1.0f;
      tin[tn].id = (int)g_touch.touches[i].finger_id;   /* raw HID id; densified in the hook */
      tin[tn].x  = gx;
      tin[tn].y  = gy;
      tn++;
    }
    g_last_tx = tin[0].x; g_last_ty = tin[0].y;
  }

  /* ---- virtual cursor: left stick moves, A taps, +/- shows/hides ---- */
  const u64 pressed = padGetButtonsDown(&g_pad);       /* edge-triggered: fires once per press */
  if (pressed & HidNpadButton_Plus) {
    g_cursor_enabled = 1; g_cursor_user_set = 1;
    debugPrintf("[input] virtual cursor ON  (+): left stick moves, A taps\n");
  }
  if (pressed & HidNpadButton_Minus) {
    g_cursor_enabled = 0; g_cursor_user_set = 1;
    debugPrintf("[input] virtual cursor OFF (-)\n");
  }
  if (!g_cursor_user_set)                              /* until the player says otherwise */
    g_cursor_enabled = (appletGetOperationMode() == AppletOperationMode_Console);

  if (g_cursor_enabled) {
    HidAnalogStickState ls = padGetStickPos(&g_pad, 0);   /* 0 = left stick */
    const int dx = ls.x, dy = ls.y;
    if (dx > 3200 || dx < -3200 || dy > 3200 || dy < -3200) {   /* ~10% deadzone kills drift */
      const float step = 34.0f * (SW / 1920.0f);              /* same feel at any resolution */
      g_cursor_x += ((float)dx / 32767.0f) * step;
      g_cursor_y += ((float)dy / 32767.0f) * step;            /* stick up -> +Y (Unity up) */
      if (g_cursor_x < 0.0f) g_cursor_x = 0.0f; else if (g_cursor_x > SW - 1.0f) g_cursor_x = SW - 1.0f;
      if (g_cursor_y < 0.0f) g_cursor_y = 0.0f; else if (g_cursor_y > SH - 1.0f) g_cursor_y = SH - 1.0f;
    }
    /* A held == finger down at the cursor. Fed as one more finger, so it works alongside a
     * real touch and picks up Began/Moved/Ended from the same tracker. */
    if ((padGetButtons(&g_pad) & HidNpadButton_A) && tn < NX_MAX_TOUCH) {
      tin[tn].id = NX_CURSOR_TOUCH_ID;
      tin[tn].x  = g_cursor_x;
      tin[tn].y  = g_cursor_y;
      tn++;
      g_last_tx = g_cursor_x; g_last_ty = g_cursor_y;
    }
  }
  nx_cursor_show = g_cursor_enabled;
  nx_cursor_x    = g_cursor_x;
  nx_cursor_y    = g_cursor_y;

  /* tn == 0 -> no fingers down: update_multi emits the Ended frame for any that just lifted. */
  nx_input_hook_update_multi(tin, tn);
}
