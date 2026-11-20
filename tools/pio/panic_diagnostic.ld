/* Pinned ESP32-S3 diagnostic only. Preserve the standard RTC objects exactly.
 * p1x_deep_wake.cpp owns raw ULP words 2016..2040 (0x50001f80 onward).
 * No SDK linker file or RTC memory-region size is changed. */
SECTIONS
{
  .rtc.panic_diagnostic 0x50001c00 (NOLOAD) :
  {
    _panic_diagnostic_start = ABSOLUTE(.);
    KEEP(*(.rtc.panic_diagnostic))
    _panic_diagnostic_end = ABSOLUTE(.);
  }
  .rtc.boot_entry_witness 0x50001cd0 (NOLOAD) :
  {
    _boot_entry_witness_start = ABSOLUTE(.);
    KEEP(*(.rtc.boot_entry_witness))
    _boot_entry_witness_end = ABSOLUTE(.);
  }
  .rtc.panic_stack_probe 0x50001d00 (NOLOAD) :
  {
    _panic_stack_probe_start = ABSOLUTE(.);
    KEEP(*(.rtc.panic_stack_probe))
    _panic_stack_probe_end = ABSOLUTE(.);
  }
}
INSERT AFTER .rtc.force_slow;
ASSERT(_rtc_noinit_end <= _panic_diagnostic_start, "panic capsule overlaps normal RTC")
ASSERT(_rtc_force_slow_end <= _panic_diagnostic_start, "panic capsule overlaps forced RTC")
ASSERT(_panic_diagnostic_start >= ORIGIN(rtc_slow_seg), "panic capsule below RTC region")
ASSERT(_panic_diagnostic_end <= 0x50001f80, "panic capsule overlaps P1-X ULP")
ASSERT(_panic_diagnostic_end <= _boot_entry_witness_start, "entry witness overlaps panic capsule")
ASSERT(_boot_entry_witness_end - _boot_entry_witness_start == 44, "entry witness layout changed")
ASSERT(_boot_entry_witness_end <= 0x50001f80, "entry witness overlaps P1-X ULP")
ASSERT(_boot_entry_witness_end <= _panic_stack_probe_start, "stack probe overlaps entry witness")
ASSERT(_panic_stack_probe_end - _panic_stack_probe_start == 256, "stack probe layout changed")
ASSERT(_panic_stack_probe_end <= 0x50001f80, "stack probe overlaps P1-X ULP")
ASSERT(0x50002000 <= ORIGIN(rtc_slow_seg) + LENGTH(rtc_slow_seg), "pinned RTC region changed")
