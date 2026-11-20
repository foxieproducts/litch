#pragma once

#include <cstddef>
#include <cstdint>

namespace firmware {

// Fixed auxiliary transaction used by settings_io. Callers own flash/RMT
// exclusion; this type owns only the current -> last-good -> pending promotion.
class LitchcraftArtworkStorage final {
  public:
    static constexpr size_t SLOT_CAPACITY = 20U * 1024U;
    static constexpr size_t WRITE_CHUNK_BYTES = 512U;
    enum class Artifact : uint8_t { Current, LastGood, Pending };
    enum class Status : uint8_t { Missing, Present, IoFailure };
    enum class CurrentKnowledge : uint8_t {
        Unknown,
        Validated,
        InvalidOrMissing,
    };
    struct Ops final {
        void* context = nullptr;
        Status (*stat)(void*, Artifact, size_t&) = nullptr;
        bool (*read)(void*, Artifact, size_t, uint8_t*, size_t) = nullptr;
        bool (*replace)(void*, Artifact, const uint8_t*, size_t) = nullptr;
        bool (*remove)(void*, Artifact) = nullptr;
        bool (*rename)(void*, Artifact, Artifact) = nullptr;
        // Optional physical-backend seam. Each call replaces one ordered chunk
        // and proves its readback before returning; offset zero truncates.
        bool (*replaceChunk)(void*, Artifact, size_t, const uint8_t*, size_t,
                             size_t) = nullptr;
    };
    struct AccessOps final {
        void* context = nullptr;
        void (*begin)(void*) = nullptr;
        void (*end)(void*) = nullptr;
    };

    LitchcraftArtworkStorage();
    explicit LitchcraftArtworkStorage(Ops ops) : m_ops(ops) {}

    bool IsConfigured() const;
    bool SetAccessOps(AccessOps ops);
    bool SetCurrentKnowledge(CurrentKnowledge knowledge);
    Status Read(Artifact artifact, uint8_t* out, size_t capacity,
                size_t& outSize) const;
    bool Save(const uint8_t* bytes, size_t size);

  private:
    bool Verify(Artifact artifact, const uint8_t* bytes, size_t size) const;
    Status StatArtifact(Artifact artifact, size_t& outSize) const;
    bool ReadArtifact(Artifact artifact, size_t offset, uint8_t* out,
                      size_t size) const;
    bool ReplaceArtifact(Artifact artifact, const uint8_t* bytes, size_t size);
    bool ReplaceAndVerifyArtifact(Artifact artifact, const uint8_t* bytes,
                                  size_t size);
    bool RemoveArtifact(Artifact artifact);
    bool RenameArtifact(Artifact from, Artifact to);
    Ops m_ops{};
    AccessOps m_accessOps{};
    CurrentKnowledge m_currentKnowledge = CurrentKnowledge::Unknown;
};

} // namespace firmware
