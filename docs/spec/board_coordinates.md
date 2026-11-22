# Board coordinate mapping

1. Square index mapping is stable: 0=a8 and 63=h1, unless flip-sides mapping is explicitly enabled.
2. Flip-sides mapping affects both sensor reads and LED output consistently (logical index maps to physical index by `63-index`).

These rules govern the [runtime invariant checks](../../include/runtime/invariant_checks.hpp). The [board controls guide](../controls_and_menu.md) describes the player-visible behavior.
