// bp_tm_trampoline.s -- frame-correct entry into TimeManager::Update's body.
//
// 2020.3's TimeManager::Update sets up its stack frame in the ENTRY prologue:
//     stp d9, d8, [sp, #-0x30]!
//     str x20, [sp, #0x10]
//     stp x19, x30, [sp, #0x20]
// and the BODY (entry+0x3c) relies on that frame; its epilogue restores
// d9/d8/x20/x19/x30 and returns. So we cannot call the body directly (its epilogue
// would pop a frame that was never pushed). This trampoline rebuilds the exact
// prologue frame, then branches to the body. The body's own epilogue then tears the
// frame down and returns to OUR caller (bp_time_update_hook / the clock thread),
// because we saved that return address in x30's slot.
//
// void bp_tm_call_body(void *tm /* x0 */, double newTime /* d0 */);
//   x0 (tm) and d0 (newTime) pass straight through to the body, which reads
//   `mov v8.16b, v0.16b` (newTime) and `mov x19, x0` (this).
//
// g_tm_body_target is a uint64_t set by bp_patches.c to (unity_base + body RVA).
    .text
    .align 2
    .global bp_tm_call_body
    .type   bp_tm_call_body, %function
bp_tm_call_body:
    stp     d9, d8, [sp, #-0x30]!      // replicate TimeManager::Update entry frame
    str     x20, [sp, #0x10]
    stp     x19, x30, [sp, #0x20]      // save OUR return address in x30's slot
    adrp    x16, g_tm_body_target
    add     x16, x16, :lo12:g_tm_body_target
    ldr     x16, [x16]                 // x16 = unity_base + body RVA
    br      x16                        // -> body; its epilogue restores frame + ret to us
    .size   bp_tm_call_body, .-bp_tm_call_body
