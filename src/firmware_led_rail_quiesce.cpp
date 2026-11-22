#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"

namespace firmware_internal {

bool QuiesceLedRail(FirmwareSystem& sys) {
    if (!sys.powerRail.Is5vEnableRequested()) return true;
    if (sys.ledAnimation.IsStarted()) {
        if (!sys.ledAnimation.Quiesce()) return false;
    } else if (sys.powerRail.Is5vEnabled() && !ClearLedOutput(sys)) return false;
    if (!sys.powerRail.Is5vEnabled()) return true;
    // This owner closes the borrowed output guard and routes both data pins
    // low before lowering 5V_EN, so later rendering cannot reopen the route.
    sys.powerRail.Disable5v();
    return true;
}

bool EnableLedRail(FirmwareSystem& sys) {
    if (sys.powerRail.Is5vEnableRequested()) return true;
    // A borrowed power guard and GPIO routing change only after the output
    // owner has finished its current transfer and cancelled pending demand.
    if (sys.ledAnimation.IsStarted() && !sys.ledAnimation.Quiesce()) return false;
    sys.powerRail.Enable5v();
    if (!sys.powerRail.Is5vEnabled()) return false;
    sys.recoveryFrameInitialized = false;
    sys.Leds().RestoreOutputPinRouting();
    sys.ledAnimation.Resume();
    return true;
}

} // namespace firmware_internal

#endif
