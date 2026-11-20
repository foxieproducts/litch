"""Track the opt-in RTC linker fragment as a real firmware input."""

Import("env")  # type: ignore[name-defined]
env.Depends(  # type: ignore[name-defined]
    "$BUILD_DIR/${PROGNAME}.elf", "$PROJECT_DIR/tools/pio/panic_diagnostic.ld"
)
