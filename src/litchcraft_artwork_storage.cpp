#include "firmware/litchcraft_artwork_storage.hpp"

#include <algorithm>
#include <array>

#ifdef ARDUINO
#include <LittleFS.h>
#include <esp_littlefs.h>
#endif

namespace firmware {
namespace {

// Keep readback scratch and individual storage requests bounded on the
// storage worker stack; hardware arbitration belongs to the access owner.
constexpr size_t VERIFY_CHUNK_BYTES = 512U;

#ifdef ARDUINO
constexpr const char* Path(const LitchcraftArtworkStorage::Artifact artifact) {
    switch (artifact) {
        case LitchcraftArtworkStorage::Artifact::Current:
            return "/litchcraft_artwork.bin";
        case LitchcraftArtworkStorage::Artifact::LastGood:
            return "/litchcraft_artwork.bak";
        case LitchcraftArtworkStorage::Artifact::Pending:
            return "/litchcraft_artwork.tmp";
    }
    return nullptr;
}

LitchcraftArtworkStorage::Status FsStat(
    void*, const LitchcraftArtworkStorage::Artifact artifact,
    size_t& outSize) {
    outSize = 0U;
    const char* const path = Path(artifact);
    if (path == nullptr || !esp_littlefs_mounted("spiffs")) {
        return LitchcraftArtworkStorage::Status::IoFailure;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        return LittleFS.exists(path)
            ? LitchcraftArtworkStorage::Status::IoFailure
            : LitchcraftArtworkStorage::Status::Missing;
    }
    outSize = file.size();
    file.close();
    return LitchcraftArtworkStorage::Status::Present;
}

bool FsRead(void*, const LitchcraftArtworkStorage::Artifact artifact,
            const size_t offset, uint8_t* const out, const size_t size) {
    const char* const path = Path(artifact);
    if (path == nullptr || (out == nullptr && size != 0U)) return false;
    File file = LittleFS.open(path, "r");
    if (!file || offset > file.size() || size > file.size() - offset ||
        !file.seek(offset, SeekSet)) {
        if (file) file.close();
        return false;
    }
    const bool complete = size == 0U || file.read(out, size) == size;
    file.close();
    return complete;
}

bool FsReplace(void*, const LitchcraftArtworkStorage::Artifact artifact,
               const uint8_t* const bytes, const size_t size) {
    const char* const path = Path(artifact);
    if (path == nullptr || bytes == nullptr || size == 0U) return false;
    File file = LittleFS.open(path, "w");
    if (!file) return false;
    const bool complete = file.write(bytes, size) == size;
    file.flush();
    const size_t written = file.size();
    file.close();
    return complete && written == size;
}

bool FsReplaceChunk(void*, const LitchcraftArtworkStorage::Artifact artifact,
                    const size_t offset, const uint8_t* const bytes,
                    const size_t size, const size_t totalSize) {
    const char* const path = Path(artifact);
    if (path == nullptr || bytes == nullptr || size == 0U ||
        size > LitchcraftArtworkStorage::WRITE_CHUNK_BYTES ||
        offset > totalSize || size > totalSize - offset) {
        return false;
    }

    File file = LittleFS.open(path, offset == 0U ? "w" : "a");
    if (!file || file.size() != offset) {
        if (file) file.close();
        return false;
    }
    const bool written = file.write(bytes, size) == size;
    file.flush();
    const bool correctSize = file.size() == offset + size;
    file.close();
    if (!written || !correctSize) return false;

    std::array<uint8_t, LitchcraftArtworkStorage::WRITE_CHUNK_BYTES> readback{};
    File readFile = LittleFS.open(path, "r");
    if (!readFile || readFile.size() != offset + size ||
        !readFile.seek(offset, SeekSet)) {
        if (readFile) readFile.close();
        return false;
    }
    const bool read = readFile.read(readback.data(), size) == size;
    readFile.close();
    return read && std::equal(
        readback.begin(), readback.begin() + size, bytes);
}

bool FsRemove(void*, const LitchcraftArtworkStorage::Artifact artifact) {
    const char* const path = Path(artifact);
    return path != nullptr && (!LittleFS.exists(path) || LittleFS.remove(path));
}

bool FsRename(void*, const LitchcraftArtworkStorage::Artifact from,
              const LitchcraftArtworkStorage::Artifact to) {
    const char* const fromPath = Path(from);
    const char* const toPath = Path(to);
    return fromPath != nullptr && toPath != nullptr &&
           LittleFS.exists(fromPath) && !LittleFS.exists(toPath) &&
           LittleFS.rename(fromPath, toPath);
}
#endif

} // namespace

LitchcraftArtworkStorage::LitchcraftArtworkStorage() {
#ifdef ARDUINO
    m_ops = {nullptr, &FsStat, &FsRead, &FsReplace, &FsRemove, &FsRename,
             &FsReplaceChunk};
#endif
}

bool LitchcraftArtworkStorage::IsConfigured() const {
    return m_ops.stat != nullptr && m_ops.read != nullptr &&
           m_ops.replace != nullptr && m_ops.remove != nullptr &&
           m_ops.rename != nullptr;
}

bool LitchcraftArtworkStorage::SetAccessOps(const AccessOps ops) {
    if ((ops.begin == nullptr) != (ops.end == nullptr)) return false;
    m_accessOps = ops;
    return true;
}

bool LitchcraftArtworkStorage::SetCurrentKnowledge(
    const CurrentKnowledge knowledge) {
    if (knowledge == CurrentKnowledge::Unknown) {
        m_currentKnowledge = knowledge;
        return true;
    }
    if (!IsConfigured()) return false;
    m_currentKnowledge = knowledge;
    return true;
}

LitchcraftArtworkStorage::Status LitchcraftArtworkStorage::StatArtifact(
    const Artifact artifact, size_t& outSize) const {
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    const Status status = m_ops.stat(m_ops.context, artifact, outSize);
    if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
    return status;
}

bool LitchcraftArtworkStorage::ReadArtifact(
    const Artifact artifact, const size_t offset, uint8_t* const out,
    const size_t size) const {
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    const bool read = m_ops.read(m_ops.context, artifact, offset, out, size);
    if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
    return read;
}

bool LitchcraftArtworkStorage::ReplaceArtifact(
    const Artifact artifact, const uint8_t* const bytes, const size_t size) {
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    const bool replaced = m_ops.replace(m_ops.context, artifact, bytes, size);
    if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
    return replaced;
}

bool LitchcraftArtworkStorage::ReplaceAndVerifyArtifact(
    const Artifact artifact, const uint8_t* const bytes, const size_t size) {
    if (m_ops.replaceChunk == nullptr) {
        return ReplaceArtifact(artifact, bytes, size) &&
               Verify(artifact, bytes, size);
    }
    for (size_t offset = 0U; offset < size;
         offset += WRITE_CHUNK_BYTES) {
        const size_t length = std::min(WRITE_CHUNK_BYTES, size - offset);
        if (m_accessOps.begin != nullptr) {
            m_accessOps.begin(m_accessOps.context);
        }
        const bool replaced = m_ops.replaceChunk(
            m_ops.context, artifact, offset, bytes + offset, length, size);
        if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
        if (!replaced) return false;
    }
    size_t storedSize = 0U;
    return StatArtifact(artifact, storedSize) == Status::Present &&
           storedSize == size;
}

bool LitchcraftArtworkStorage::RemoveArtifact(const Artifact artifact) {
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    const bool removed = m_ops.remove(m_ops.context, artifact);
    if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
    return removed;
}

bool LitchcraftArtworkStorage::RenameArtifact(const Artifact from,
                                              const Artifact to) {
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    const bool renamed = m_ops.rename(m_ops.context, from, to);
    if (m_accessOps.end != nullptr) m_accessOps.end(m_accessOps.context);
    return renamed;
}

LitchcraftArtworkStorage::Status LitchcraftArtworkStorage::Read(
    const Artifact artifact, uint8_t* const out, const size_t capacity,
    size_t& outSize) const {
    outSize = 0U;
    if (!IsConfigured() || (out == nullptr && capacity != 0U)) {
        return Status::IoFailure;
    }
    size_t size = 0U;
    const Status status = StatArtifact(artifact, size);
    if (status != Status::Present) return status;
    outSize = size;
    if (size > SLOT_CAPACITY || size > capacity) return Status::Present;
    return ReadArtifact(artifact, 0U, out, size)
        ? Status::Present : Status::IoFailure;
}

bool LitchcraftArtworkStorage::Verify(
    const Artifact artifact, const uint8_t* const bytes,
    const size_t size) const {
    size_t storedSize = 0U;
    if (StatArtifact(artifact, storedSize) != Status::Present ||
        storedSize != size) {
        return false;
    }
    std::array<uint8_t, VERIFY_CHUNK_BYTES> chunk{};
    for (size_t offset = 0U; offset < size; offset += chunk.size()) {
        const size_t length = std::min(chunk.size(), size - offset);
        if (!ReadArtifact(artifact, offset, chunk.data(), length) ||
            !std::equal(chunk.begin(), chunk.begin() + length, bytes + offset)) {
            return false;
        }
    }
    return true;
}

bool LitchcraftArtworkStorage::Save(const uint8_t* const bytes,
                                    const size_t size) {
    if (!IsConfigured() || m_currentKnowledge == CurrentKnowledge::Unknown ||
        bytes == nullptr || size == 0U ||
        size > SLOT_CAPACITY ||
        !RemoveArtifact(Artifact::Pending) ||
        !ReplaceAndVerifyArtifact(Artifact::Pending, bytes, size)) {
        return false;
    }
    size_t currentSize = 0U;
    const Status current =
        StatArtifact(Artifact::Current, currentSize);
    if (current == Status::IoFailure) {
        return false;
    }
    if (m_currentKnowledge == CurrentKnowledge::Validated) {
        if (current != Status::Present) {
            m_currentKnowledge = CurrentKnowledge::Unknown;
            return false;
        }
        if (!RemoveArtifact(Artifact::LastGood)) return false;
        m_currentKnowledge = CurrentKnowledge::Unknown;
        if (!RenameArtifact(Artifact::Current, Artifact::LastGood)) {
            size_t ignored = 0U;
            const Status currentAfter = StatArtifact(Artifact::Current, ignored);
            if (currentAfter == Status::Present) {
                m_currentKnowledge = CurrentKnowledge::Validated;
            } else if (currentAfter == Status::Missing &&
                       StatArtifact(Artifact::LastGood, ignored) ==
                           Status::Present) {
                m_currentKnowledge = CurrentKnowledge::InvalidOrMissing;
            }
            return false;
        }
        m_currentKnowledge = CurrentKnowledge::InvalidOrMissing;
    } else if (current == Status::Present &&
               !RemoveArtifact(Artifact::Current)) {
        return false;
    }
    if (!RenameArtifact(Artifact::Pending, Artifact::Current)) {
        if (Verify(Artifact::Current, bytes, size)) {
            m_currentKnowledge = CurrentKnowledge::Validated;
            // A backend error may follow a completed rename. Exact readback
            // establishes durability even when that operation's return did not.
            return true;
        }
        return false;
    }
    const bool verified = Verify(Artifact::Current, bytes, size);
    m_currentKnowledge = verified ? CurrentKnowledge::Validated
                                  : CurrentKnowledge::InvalidOrMissing;
    return verified;
}

} // namespace firmware
