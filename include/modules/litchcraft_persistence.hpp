#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_animation_codec.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/settings_store.hpp"

namespace modules {

inline constexpr const char* LITCHCRAFT_ART_KEY = "LITCHCRAFT_ART_V1";
struct LitchcraftRecord {
    LitchcraftSession::Canvas canvas{};
    LitchcraftQuickPalette quickPalette = LITCHCRAFT_DEFAULT_QUICK_PALETTE;
    bool active = false;
};
enum class LitchcraftRecordStatus : uint8_t { Missing, Valid, Invalid, FutureVersion };
using LitchcraftEncodedCanvas = std::array<char, 1025U>;
using LitchcraftEncodedQuickPalette = std::array<char, 33U>;
LitchcraftRecordStatus ReadLitchcraftRecord(const JsonDocument& document, LitchcraftRecord& out);
bool ScrubLitchcraftRecord(JsonDocument& document, fxe::SettingsRemovedKeys* removed = nullptr);
// The document links this caller-owned buffer, which must remain alive and
// unchanged until serialization. Reusing it avoids JSON-pool growth per save.
bool WriteLitchcraftRecord(JsonDocument& document, const LitchcraftRecord& record,
                          LitchcraftEncodedCanvas& storage,
                          LitchcraftEncodedQuickPalette& paletteStorage);

class LitchcraftPersistence final {
  public:
    static constexpr uint32_t CANVAS_IDLE_MS = 5000U;
    static constexpr uint32_t MODE_IDLE_MS = 100U;
    static constexpr size_t ADVANCED_SLOT_BYTES = 20U * 1024U;
    static constexpr size_t ADVANCED_BODY_OFFSET = 16U;
    static constexpr size_t ADVANCED_ENVELOPE_TRAILER_BYTES = 4U;
    static constexpr size_t ADVANCED_MAX_ENVELOPE_BYTES =
        ADVANCED_BODY_OFFSET + litchcraft_animation::MAX_ENCODED_BYTES +
        ADVANCED_ENVELOPE_TRAILER_BYTES;
    static constexpr size_t ADVANCED_ACTIVATION_BODY_COUNT = 3U;
    static constexpr size_t ADVANCED_ACTIVATION_RAM_LIMIT_BYTES = 36U * 1024U;
    enum class SaveOutcome : uint8_t { Pending, Saved, Failed };
    enum class FlushStatus : uint8_t { Idle, Pending, Saved, Failed };
    enum class AdvancedSlot : uint8_t { Current, LastGood };
    enum class AdvancedReadOutcome : uint8_t { Missing, Present, Failed };
    enum class AdvancedRecordStatus : uint8_t {
        BackendUnavailable,
        Missing,
        Valid,
        Recovered,
        Invalid,
        FutureVersion,
    };
    struct AdvancedDiagnostics {
        uint32_t revision = 0U;
        uint32_t submittedRevision = 0U;
        uint32_t savedRevision = 0U;
        uint32_t submittedReceipt = 0U;
        uint32_t savedReceipt = 0U;
        uint32_t submittedCrc = 0U;
        uint32_t savedCrc = 0U;
        uint8_t frames = 0U;
        uint8_t submittedFrames = 0U;
        uint8_t savedFrames = 0U;
    };
    AdvancedDiagnostics GetAdvancedDiagnostics() const {
        return {m_advancedGeneration, m_submittedAdvancedGeneration,
                m_savedAdvancedGeneration, m_lastAdvancedReceipt, m_savedAdvancedReceipt,
                m_submittedEnvelopeCrc, m_savedEnvelopeCrc,
                uint8_t(HasAdvancedDocument() ? AdvancedDocument().Value().frameCount : 0U),
                m_submittedEnvelopeFrames, m_savedEnvelopeFrames};
    }
    struct AdvancedEnvelope final {
        std::array<uint8_t, ADVANCED_MAX_ENVELOPE_BYTES> bytes{};
        size_t size = 0U;
    };
    struct AdvancedStorage final {
        litchcraft_animation::ValidatedDocument document{};
        litchcraft_animation::Document staging{};
        AdvancedEnvelope envelope{};
        bool active = false;
    };
    struct Ops {
        void* context = nullptr;
        fxe::SettingsStore::AsyncSaveAdmission (*enqueue)(void*, const JsonDocument&, uint32_t&) = nullptr;
        // Saved certifies the frozen submission was promoted and read back
        // byte-for-byte by the storage worker. Runtime polling must not do I/O.
        SaveOutcome (*poll)(void*, uint32_t) = nullptr;
        void (*release)(void*, uint32_t) = nullptr;
        // Boot/recovery only; never called from the cooperative Update path.
        AdvancedReadOutcome (*readAdvanced)(void*, AdvancedSlot, uint8_t*, size_t,
                                            size_t&) = nullptr;
        fxe::SettingsStore::AsyncSaveAdmission (*enqueueAdvanced)(
            void*, const uint8_t*, size_t, uint32_t&) = nullptr;
        // Firmware freezes the document before this nonblocking admission;
        // encoding and storage are completed by the persistence worker.
        fxe::SettingsStore::AsyncSaveAdmission (*enqueueAdvancedDocument)(
            void*, const litchcraft_animation::Document&, bool,
            AdvancedEnvelope&, uint32_t&) = nullptr;
    };
    LitchcraftPersistence(SettingsStore& settings, LitchcraftSession& session)
        : m_settings(settings), m_session(session) {}
    void Configure(Ops ops);
    // On Store-owned boards, this owner only drains lifecycle settings saves.
    // Artwork and mode durability belong to the frame Store.
    void ConfigureStoreOnly(Ops ops);
    // The firmware owns this fixed working set before advertising advanced
    // support. Admission, edits and persistence never allocate document bodies.
    void ConfigureAdvancedStorage(AdvancedStorage& storage);
    AdvancedRecordStatus RestoreAdvanced();
    bool SetAdvancedDocument(
        const litchcraft_animation::ValidatedDocument& document, bool active);
    bool HasAdvancedDocument() const;
    uint32_t AdvancedGeneration() const { return m_advancedGeneration; }
    const litchcraft_animation::ValidatedDocument& AdvancedDocument() const;
    bool IsAdvancedConfigured() const;
    static bool EncodeAdvancedEnvelope(
        const litchcraft_animation::Document& document, bool active,
        AdvancedEnvelope& envelope);
    AdvancedStorage* AcquireAdvancedTransportLease();
    AdvancedStorage* AdvancedTransportStorage();
    void ReleaseAdvancedTransportLease();
    bool AdoptAdvancedTransportDocument(size_t size, uint32_t crc, bool active);
    // Call after settings recovery and session.Configure. Restored mode arms
    // a first-Hall-observation baseline only when boot occupancy is unknown.
    LitchcraftRecordStatus Restore(uint64_t occupancy = 0U,
                                  bool bootOccupancyKnown = false);
    void Update();
    void RequestFlush();
    FlushStatus FlushState() const { return m_flushState; }
    bool IsDirty() const;
    bool HasFailure() const { return m_failed; }
    bool IsSaving() const { return m_receipt != 0U || m_flushRequested; }
    bool IsConfigured() const { return m_ops.enqueue && m_ops.poll && m_ops.release; }

  private:
    friend class LitchcraftSession;
    enum class SubmittedKind : uint8_t { None, Static, Advanced };
    AdvancedRecordStatus ReadAdvancedSlot(AdvancedSlot slot);
    static void RefreshAdvancedEnvelopeChecksum(AdvancedEnvelope& envelope);
    fxe::SettingsStore::AsyncSaveAdmission QueueAdvanced();
    void RestoreDurableDocument();
    SettingsStore& m_settings;
    LitchcraftSession& m_session;
    Ops m_ops{};
    LitchcraftRecord m_durable{};
    LitchcraftEncodedCanvas m_encoded{};
    LitchcraftEncodedQuickPalette m_encodedPalette{};
    AdvancedStorage* m_advanced = nullptr;
    uint32_t m_savedCanvasGeneration = 0U;
    uint32_t m_savedModeGeneration = 0U;
    uint32_t m_savedPaletteGeneration = 0U;
    uint32_t m_submittedCanvasGeneration = 0U;
    uint32_t m_submittedModeGeneration = 0U;
    uint32_t m_submittedPaletteGeneration = 0U;
    uint32_t m_receipt = 0U;
    uint32_t m_failedCanvasGeneration = 0U;
    uint32_t m_failedModeGeneration = 0U;
    uint32_t m_failedPaletteGeneration = 0U;
    uint32_t m_advancedGeneration = 0U;
    uint32_t m_savedAdvancedGeneration = 0U;
    uint32_t m_submittedAdvancedGeneration = 0U;
    uint32_t m_failedAdvancedGeneration = 0U;
    uint32_t m_advancedChangedAtMs = 0U;
    uint32_t m_lastAdvancedReceipt = 0U;
    uint32_t m_savedAdvancedReceipt = 0U;
    uint32_t m_submittedEnvelopeCrc = 0U;
    uint32_t m_savedEnvelopeCrc = 0U;
    uint8_t m_submittedEnvelopeFrames = 0U;
    uint8_t m_savedEnvelopeFrames = 0U;
    SubmittedKind m_submittedKind = SubmittedKind::None;
    bool m_failed = false;
    bool m_restoreBlocked = false;
    bool m_advancedRestoreBlocked = false;
    bool m_flushRequested = false;
    bool m_submittedForFlush = false;
    bool m_advancedTransportLeased = false;
    bool m_storeOnly = false;
    FlushStatus m_flushState = FlushStatus::Idle;
};

static_assert(LitchcraftPersistence::ADVANCED_ACTIVATION_BODY_COUNT == 3U,
              "advanced activation owns live, incoming, and outgoing bodies");
static_assert(sizeof(LitchcraftPersistence::AdvancedStorage) <=
                  LitchcraftPersistence::ADVANCED_ACTIVATION_RAM_LIMIT_BYTES,
              "live, staging and envelope fit their 36KiB fixed storage budget");
static_assert(sizeof(LitchcraftSession) + sizeof(LitchcraftPersistence) +
                  sizeof(LitchcraftPersistence::AdvancedStorage) +
                  sizeof(litchcraft_animation::ComposedFrame) <=
                  64U * 1024U,
              "session, persistence, artwork bodies and render scratch fit 64KiB");
static_assert(sizeof(LitchcraftPersistence::AdvancedEnvelope) <= 20U * 1024U,
              "one advanced codec envelope must fit its 20KiB bound");
static_assert(LitchcraftPersistence::ADVANCED_SLOT_BYTES * 3U <= 60U * 1024U,
              "current, last-good, and pending slots must fit 60KiB storage");

} // namespace modules
