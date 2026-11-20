#include "modules/board_state_notification.hpp"

#include <ArduinoJson.h>
#include "foxie_embedded/console.hpp"

#include "modules/ble_settings_value_codec.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/settings_registry.hpp"
#include "modules/ui_color_keys.hpp"

namespace modules {

bool TryFormatBoardStateNotification(
    const HomeStateNotificationLine& home,
    const BoardHealthLine& health,
    const SettingsStore& settings,
    BoardStateNotificationLine& outLine, const uint32_t receipt) {
    outLine.Clear();
    if (home.Empty() || health.Empty()) {
        return false;
    }

    // Encode only one small descriptor at a time rather than duplicating the
    // full settings document on the firmware loop's stack.
    StaticJsonDocument<256> item;
    char encoded[256] = {};
    const auto appendItem = [&]() {
        const size_t size = serializeJson(item, encoded, sizeof(encoded));
        return !item.overflowed() && size < sizeof(encoded) &&
               outLine.Append(std::string_view(encoded, size));
    };
    item.set(settings.GetRevision());
    bool ok = outLine.Append("{\"event\":\"board_state\",\"v\":1,\"sr\":") &&
              appendItem() && outLine.Append(",\"home\":") &&
              outLine.Append(home.View()) && outLine.Append(",\"settings\":[");
    size_t count = 0U;
    const auto* descriptors = settings_registry::EnumerateAll(count);
    bool first = true;
    for (size_t i = 0U; ok && descriptors != nullptr && i < count; ++i) {
        const auto& descriptor = descriptors[i];
        if (!ble_settings_value_codec::IsAppVisible(descriptor)) {
            continue;
        }
        item.clear();
        JsonArray entry = item.to<JsonArray>();
        entry.add(descriptor.key);
        entry.add(nullptr);
        ok = ble_settings_value_codec::WriteCurrentValue(settings, descriptor, entry[1]) &&
             (first || outLine.Append(",")) && appendItem();
        first = false;
    }
    item.clear();
    JsonArray ends = item.to<JsonArray>();
    ends.add(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P1_END_PIECE_WHEEL, 170U));
    ends.add(player_end_palette::ReadSettingUInt32(
        settings, ui_color_keys::P2_END_PIECE_WHEEL, 0U));
    ok = ok && outLine.Append("],\"ends\":") && appendItem() &&
         outLine.Append(",\"health\":") && outLine.Append(health.View());
    if (receipt != 0U) {
        item.clear();
        item.set(receipt);
        ok = ok && outLine.Append(",\"q\":") && appendItem();
    }
    ok = ok && outLine.Append("}");
    if (!ok) {
        // Callers must never enqueue a truncated JSON prefix as authority.
        outLine.Clear();
    }
    return ok;
}

void WriteBoardStatePublication(fxe::Console& console,
                               BoardHealthLine& lastHealth,
                               const BoardStatePublication& publication) {
    const bool publishHome = !publication.home.Empty();
    const bool publishHealth = publication.force ||
                               !(lastHealth == publication.health);
    if (!publishHome && !publishHealth) return;
    console.BeginBulkOutput();
    if (publication.force && publishHome) {
        BoardStateNotificationLine initial;
        if (TryFormatBoardStateNotification(publication.home, publication.health,
                                             publication.settings, initial, publication.receipt)) {
            console.PrintRaw(initial.CStr(), initial.Size());
            console.PrintRaw("\r\n", 2U);
        }
    }
    if (publishHealth) {
        console.PrintLn("%s", publication.health.CStr());
        lastHealth = publication.health;
    }
    if (publishHome) {
        console.PrintRaw(publication.home.CStr(), publication.home.Size());
        console.PrintRaw("\r\n", 2U);
    }
    console.EndBulkOutput();
}

} // namespace modules
