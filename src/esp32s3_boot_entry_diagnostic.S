/* Private entry witness: no call, stack/window change, SDK or peripheral API.
 * The direct jump leaves the bootloader's pending call8 window for the SDK's
 * original entry instruction. Only scratch a2/a3 are used before that entry.
 */
#if defined(LITCH_ESP32S3) && defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
#ifndef LITCH_BOOT_ENTRY_TOKEN
#error "An exact private trial token is required"
#endif
    .section .iram1,"ax",@progbits
    .align 4
    .global litch_diagnostic_entry
    .type litch_diagnostic_entry,@function
litch_diagnostic_entry:
    movi a2, litch_boot_entry_witness
    movi a3, 0
    s32i a3, a2, 0
    movi a3, LITCH_BOOT_ENTRY_TOKEN
    s32i a3, a2, 4
    movi a3, (~LITCH_BOOT_ENTRY_TOKEN)
    s32i a3, a2, 8
    movi a3, 1
    s32i a3, a2, 12
    movi a3, -2
    s32i a3, a2, 16
    /* Fixed S3 RTC registers, read before application SDK constructors. The
     * second-stage loader has already run and may change brownout config. */
    movi a3, 0x60008038
    l32i a3, a3, 0
    s32i a3, a2, 20
    neg a3, a3
    addi a3, a3, -1
    s32i a3, a2, 24
    movi a3, 0x600080c8
    l32i a3, a3, 0
    s32i a3, a2, 28
    neg a3, a3
    addi a3, a3, -1
    s32i a3, a2, 32
    movi a3, 0x600080e8
    l32i a3, a3, 0
    s32i a3, a2, 36
    neg a3, a3
    addi a3, a3, -1
    s32i a3, a2, 40
    memw
    movi a3, 0x454e5432
    s32i a3, a2, 0
    memw
    j call_start_cpu0
    .size litch_diagnostic_entry, .-litch_diagnostic_entry
#endif
