#include "bounded_hint_engine.hpp"

#include <cstdio>
#include <string>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/settings_store.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t DUCK = 24U;

void Checkpoint(const runtime::VirtualLitchSession& board, const char* input,
                const char* next, const unsigned directAccesses) {
    const auto& state = board.State();
    const auto color = board.SemanticFrame().board[DUCK][1];
    const auto text = board.DisplayText();
    std::printf("[SETTINGS_DUCK_RECOVERY] t=%u input=%s occupancy=%016llx "
                "variant=%u phase=%u required=%u correction=%016llx "
                "duck_led=%u/%u/%u edge=%u text=\"%.*s\" next=%s storage_accesses=%u\n",
                state.now_ms, input,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned>(board.GameVariant()), state.interaction_phase,
                state.required_action,
                static_cast<unsigned long long>(state.correction_mask),
                color.r, color.g, color.b, static_cast<unsigned>(board.EdgeCueIntent().kind),
                static_cast<int>(text.size()), text.empty() ? "" : text.data(),
                next, directAccesses);
}
} // namespace

int main() {
    fxe::test_support::TestResults results("SETTINGS_DUCK_RECOVERY_VIRTUAL_LITCH_JOURNEY");
    results.Start();
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config, "build/settings_duck_recovery.json");
    auto& board = fixture.session;
    auto& settings = board.SettingsForPersistenceTest();
    results.Check(board.Reset() == VIRTUAL_LITCH_OK &&
                      board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
                      board.Advance(6200U) == VIRTUAL_LITCH_OK,
                  "production opening accepted and setup acknowledgement expires");
    modules::ControlMutationOutcome outcome;
    results.Check(board.MutateSettingJson(100U, "BRIGHTNESS", "7", outcome) == VIRTUAL_LITCH_OK &&
                      settings.SaveNowBlocking(),
                  "fixture saves older brightness7 before controlled RAM pressure");
    results.Check(board.MutateSettingJson(101U, "BRIGHTNESS", "8", outcome) == VIRTUAL_LITCH_OK,
                  "latest brightness8 accepted in live RAM");
    unsigned directAccesses = 0U;
    settings.SetBeforeDirectStorageAccessCallback([&]() { ++directAccesses; return true; });
    // Controlled equivalent fault: valid retained data larger than old 3072-byte
    // scratch plus dead allocator space, within the actual 8192-byte file bound.
    settings.Doc()["CONTROLLED_RETAINED"] = std::string(3600U, 'r');
    const size_t freePool = settings.Doc().capacity() - settings.Doc().memoryUsage();
    settings.Doc()["CONTROLLED_DEAD_POOL"] = std::string(freePool - 128U, 'x');
    settings.Doc().remove("CONTROLLED_DEAD_POOL");
    results.Check(!settings.Doc().overflowed() && measureJson(settings.Doc()) < 8192U,
                  "controlled document is valid before physical Duck input");
    results.Check(board.ObserveHallMask(OPENING | (UINT64_C(1) << DUCK)) == VIRTUAL_LITCH_OK &&
                      board.Advance(200U) == VIRTUAL_LITCH_OK &&
                      board.GameVariant() == modules::GameVariant::Duck &&
                      board.DuckSquare() == DUCK && directAccesses == 0U &&
                      settings.ReadValue("BRIGHTNESS", 0U) == 8U,
                  "physical Duck activates without storage work or preference rollback");
    const auto color = board.SemanticFrame().board[DUCK][1];
    results.Check(color.r == 255U && color.g == 255U && color.b == 0U,
                  "active Duck center LED has the production yellow cue");
    Checkpoint(board, "add-Duck", "remove-Duck-before-first-move", directAccesses);
    results.Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
                      board.Advance(200U) == VIRTUAL_LITCH_OK &&
                      board.GameVariant() == modules::GameVariant::Undecided &&
                      board.State().correction_mask == 0ULL,
                  "removal before play clears Duck mode and correction");
    Checkpoint(board, "remove-Duck", "New-Game", directAccesses);
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = 102U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    results.Check(board.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Accepted &&
                      board.Advance(1U) == VIRTUAL_LITCH_OK &&
                      board.TryTakeControlCompletion(outcome) &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      board.GameVariant() == modules::GameVariant::Undecided &&
                      settings.ReadValue("BRIGHTNESS", 0U) == 8U && directAccesses == 0U,
                  "acknowledged New Game retains latest preference without flash access");
    results.Check(board.Advance(6200U) == VIRTUAL_LITCH_OK,
                  "New Game acknowledgement expires through timed production rendering");
    Checkpoint(board, "New-Game-ack-expired", "move-first-piece-or-add-Duck", directAccesses);
    return results.Finish();
}
