/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

// File-only logger (DEBUG_LOG builds only): open once + flush per line so the tail survives a
// crash, mutex-serialised across engine threads. Drops the high-frequency dlsym/dlopen/JNI spam.
#if DEBUG_LOG
static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
}
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  static FILE *f = NULL;
  va_list list;
  if (log_is_noisy(text)) return 0;
  mutexLock(&g_log_lock);
  if (!f) f = fopen(LOG_NAME, "a");
  if (f) { va_start(list, text); vfprintf(f, text, list); va_end(list); fflush(f); }
  mutexUnlock(&g_log_lock);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
