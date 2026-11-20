#include <cstdio>
#include <cstring>

#include "modules/litchcraft_history.hpp"

namespace {
using modules::LitchcraftHistory;
using modules::litchcraft_animation::Document;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void FullDocumentAndFragmentedRoundTrips() {
    LitchcraftHistory history;
    Document before{};
    before.frameCount = modules::litchcraft_animation::MAX_FRAME_COUNT;
    // Exercise the representation bound, including padding. No decoder or
    // renderer consumes these intentionally arbitrary object bytes.
    for (unsigned step = 0U; step < 12U; ++step) {
        Document after{};
        std::memcpy(&after, &before, sizeof(after));
        auto* bytes = reinterpret_cast<unsigned char*>(&after);
        for (size_t offset = 0U; offset < sizeof(after); ++offset) {
            if (step % 2U == 0U || offset % 2U == 0U) bytes[offset] ^= 0x5aU;
        }
        Check(history.Record(before, after, 2U, 7U, false),
              "newest maximum-size operation fits the fixed arena");
        Check(history.BytesUsed() <= 12U * 1024U,
              "dense and worst fragmented operations remain within 12 KiB");
        Document restored{};
        std::memcpy(&restored, &after, sizeof(restored));
        uint8_t selected = 7U;
        Check(history.Undo(restored, selected) && selected == 2U &&
                  std::memcmp(&restored, &before, sizeof(before)) == 0,
              "eviction preserves every byte and selected frame for Undo");
        Check(history.Redo(restored, selected) && selected == 7U &&
                  std::memcmp(&restored, &after, sizeof(after)) == 0,
              "Redo restores every byte and selected frame");
        std::memcpy(&before, &after, sizeof(before));
    }
}

void TypicalSparseEditsRetainFiftySteps() {
    LitchcraftHistory history;
    Document document{};
    document.frameCount = modules::litchcraft_animation::MAX_FRAME_COUNT;
    const Document initial = document;
    for (uint8_t step = 1U; step <= 50U; ++step) {
        Document before = document;
        auto& emitter = document.frames[step % 8U].emitters[step * 5U];
        emitter.color = {step, static_cast<uint8_t>(step + 1U),
                         static_cast<uint8_t>(step + 2U)};
        Check(history.Record(before, document, 0U, 0U, false),
              "typical sparse operation records");
    }
    Check(history.UndoCount() == 50U, "all 50 typical edits survive");
    uint8_t selected = 0U;
    for (uint8_t step = 0U; step < 50U; ++step) {
        Check(history.Undo(document, selected), "each typical edit is undoable");
    }
    Check(std::memcmp(&document, &initial, sizeof(document)) == 0,
          "50 Undo operations restore the complete original document");
}
} // namespace

int main() {
    FullDocumentAndFragmentedRoundTrips();
    TypicalSparseEditsRetainFiftySteps();
    const size_t denseBytes = sizeof(Document) + 3U * ((sizeof(Document) + 254U) / 255U);
    std::printf("[HISTORY_BOUND] document=%zu dense_run=%zu arena=%zu\n",
                sizeof(Document), denseBytes, sizeof(LitchcraftHistory));
    return g_failures == 0 ? 0 : 1;
}
