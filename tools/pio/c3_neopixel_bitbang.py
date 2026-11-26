"""Keep NeoPixelBus's timing-critical C3 GPIO loop in instruction RAM."""

Import("env")


def keep_bitbang_in_iram(build_env, node):
    # With LTO the upstream IRAM_ATTR routine gets inlined into flash code.
    # Compile this one source as a normal object so its section is preserved.
    return build_env.Object(node, CCFLAGS=[*build_env["CCFLAGS"], "-fno-lto"])


env.AddBuildMiddleware(
    keep_bitbang_in_iram, "*/NeoPixelBus/src/internal/methods/NeoEspBitBangMethod.cpp"
)
