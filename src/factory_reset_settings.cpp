#include "firmware/factory_reset_settings.hpp"

#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_persistence.hpp"
#include "modules/board_storage_recovery.hpp"

namespace firmware {
namespace {
template <bool OwnerPairing>
bool PrepareReset(JsonDocument& document) {
    StaticJsonDocument<1024U> safe;
    if (!modules::BuildBoardStorageRecoverySnapshot(document, safe) || !document.set(safe)) return false;
    // This is not a saved phone. It is durable authority to finish the user's
    // reset before pairing becomes available, even after another power cut.
    if constexpr (OwnerPairing) {
        if (!modules::WriteBoardOwner(document, modules::BoardOwnerRecord::Clearing)) return false;
    }
    return true;
}
}

bool ResetFactorySettings(modules::SettingsStore& settings, bool ownerPairing) {
    return settings.ResetFileForRecovery(ownerPairing ? &PrepareReset<true> : &PrepareReset<false>);
}
}
