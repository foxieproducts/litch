#include "firmware/litchcraft_frame_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#if defined(ARDUINO) || defined(LITCH_NATIVE)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"

#if defined(ARDUINO) && !defined(LITCH_NATIVE)
#include <esp_littlefs.h>
#endif

namespace firmware {

LitchcraftFrameFile::~LitchcraftFrameFile() {
    CloseCachedHandles();
}

void LitchcraftFrameFile::CloseCachedHandles() {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    if (m_readFile == nullptr && m_writeFd < 0) return;
#else
    return;
#endif
    BeginAccess();
    CloseRead();
    CloseWrite();
    EndAccess();
}

bool LitchcraftFrameFile::SetAccessOps(const AccessOps ops) {
    if ((ops.begin == nullptr) != (ops.end == nullptr)) return false;
    m_accessOps = ops;
    return true;
}

modules::litchcraft_frame_store::FrameStore::Ops
LitchcraftFrameFile::StoreOps() {
    return {this, &Read, &Write, &Sync, &PrepareReplacementBank};
}

bool LitchcraftFrameFile::PrepareReplacementBank(void* const context,
                                                const uint8_t resident) {
    return context != nullptr &&
        static_cast<LitchcraftFrameFile*>(context)->PrepareBank(resident);
}

void LitchcraftFrameFile::BeginAccess() {
    const auto elapsed = fxe::ElapsedTime::StartNow();
    if (m_accessOps.begin != nullptr) m_accessOps.begin(m_accessOps.context);
    m_accessTiming.maximumLeaseWaitMs = std::max(
        m_accessTiming.maximumLeaseWaitMs, elapsed.ElapsedMs());
}

void LitchcraftFrameFile::EndAccess(const AccessOperation operation,
                                  const size_t actualBytes, const uint32_t elapsedMs) {
    if (m_accessOps.end != nullptr) {
        m_accessOps.end(m_accessOps.context, operation, actualBytes, elapsedMs);
    }
}

void LitchcraftFrameFile::RecordAccess(const AccessOperation operation,
                                       const size_t offset, const size_t size,
                                       const uint32_t elapsedMs) {
    ++m_accessTiming.accesses;
    if (m_accessTiming.maximumOperation == AccessOperation::None ||
        elapsedMs > m_accessTiming.maximumMs) {
        m_accessTiming.maximumMs = elapsedMs;
        m_accessTiming.maximumOperation = operation;
        m_accessTiming.maximumBytes = static_cast<uint32_t>(size);
        m_accessTiming.maximumOffset = static_cast<uint32_t>(offset);
    }
}

bool LitchcraftFrameFile::Read(void* const context, const size_t offset,
                               uint8_t* const out, const size_t size) {
    return context != nullptr &&
        static_cast<LitchcraftFrameFile*>(context)->ReadAt(offset, out, size);
}

void LitchcraftFrameFile::SetEditTimingPhase(
    const modules::litchcraft_frame_store::FrameStore::TimingPhase phase) {
    using Phase = modules::litchcraft_frame_store::FrameStore::TimingPhase;
    switch (phase) {
        case Phase::EditBegin: m_editTimingPhase = 0U; break;
        case Phase::BodyPrepare: m_editTimingPhase = 1U; break;
        case Phase::RootPublish: m_editTimingPhase = 2U; break;
        case Phase::BodyWrite: break;
    }
}

void LitchcraftFrameFile::RecordSync(
    const Region region, const size_t fileBytes, const size_t cursor,
    const uint32_t elapsedMs) {
    auto& sync = m_accessTiming.sync;
    auto& phase = sync.phases[m_editTimingPhase];
    phase.totalMs += elapsedMs;
    phase.maximumMs = std::max(phase.maximumMs, elapsedMs);
    ++phase.calls;
    if (sync.maximumPhase == UINT8_MAX || elapsedMs > sync.maximumMs) {
        sync.maximumMs = elapsedMs;
        sync.maximumFileBytes = static_cast<uint32_t>(fileBytes);
        sync.maximumCursor = static_cast<uint32_t>(cursor);
        sync.maximumRegion = region;
        sync.maximumFileRegion = FileRegion(region);
        sync.maximumPhase = m_editTimingPhase;
    }
}

bool LitchcraftFrameFile::Write(void* const context, const size_t offset,
                                const uint8_t* const bytes, const size_t size) {
    return context != nullptr &&
        static_cast<LitchcraftFrameFile*>(context)->WriteAt(offset, bytes, size);
}

bool LitchcraftFrameFile::Sync(void* const context) {
    return context != nullptr &&
        static_cast<LitchcraftFrameFile*>(context)->SyncWrites();
}

bool LitchcraftFrameFile::ReadAt(size_t offset, uint8_t* out, size_t size) {
    namespace fixed = modules::litchcraft_frame_store_layout;
    if ((out == nullptr && size != 0U) || offset > fixed::ARTWORK_FILE_CAPACITY ||
        size > fixed::ARTWORK_FILE_CAPACITY - offset) return false;
    while (size != 0U) {
        const size_t count = ContiguousBytes(offset, size);
        if (count == 0U) return false;
#if defined(ARDUINO) || defined(LITCH_NATIVE)
        PhysicalSpan span{};
        fxe::FixedString<105U> path;
        if (!ResolvePhysicalSpan(offset, count, span)) return false;
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
        if (!esp_littlefs_mounted("spiffs")) { errno = ENODEV; return false; }
        constexpr bool mounted = true;
#else
        constexpr bool mounted = false;
#endif
        if (!BuildPath(span.region, path, mounted)) return false;
        const auto elapsed = fxe::ElapsedTime::StartNow();
        BeginAccess();
        std::fill_n(out, count, 0xffU);
        const bool body = IsBody(span.region);
        const bool retained = body || span.region == Region::Summary0 ||
            span.region == Region::Summary1 ||
            (span.region == Region::Root0 && span.offset >=
                modules::litchcraft_frame_store_layout::Layout::StageOffset());
        std::FILE* transient = nullptr;
        bool metadataFailed = false;
        if (retained) {
            if (m_readFile != nullptr && m_readRegion != span.region) CloseRead();
            if (m_readFile == nullptr) {
                errno = 0;
#if defined(LITCH_NATIVE)
                ++m_readOpens;
#endif
                m_readFile = (fopen)(path.CStr(), "rb");
                if (m_readFile != nullptr) {
#if defined(LITCH_NATIVE)
                    if (body) ++m_bodyReadOpens;
#endif
                    if ((fseek)(m_readFile, 0L, SEEK_END) != 0) {
                        CloseRead();
                        metadataFailed = true;
                    } else {
                        const long bytes = (ftell)(m_readFile);
                        if (bytes < 0L) {
                            CloseRead();
                            metadataFailed = true;
                        }
                        else {
                            m_readRegion = span.region;
                            m_readSize = static_cast<size_t>(bytes);
                        }
                    }
                } else if (errno != ENOENT) {
                    metadataFailed = true;
                }
            }
        } else {
            errno = 0;
#if defined(LITCH_NATIVE)
            ++m_readOpens;
#endif
            transient = (fopen)(path.CStr(), "rb");
            if (transient == nullptr && errno != ENOENT) metadataFailed = true;
        }
        std::FILE* const file = retained ? m_readFile : transient;
        bool complete = !metadataFailed;
        if (file != nullptr && !retained) {
            complete = (fseek)(file, 0L, SEEK_END) == 0;
        }
        const long transientSize = !retained && complete && file != nullptr
            ? (ftell)(file) : 0L;
        if (!retained && file != nullptr && transientSize < 0L) complete = false;
        const size_t fileSize = retained ? m_readSize :
            (transientSize > 0L ? static_cast<size_t>(transientSize) : 0U);
        const size_t available = file != nullptr && span.offset < fileSize
            ? std::min(count, fileSize - span.offset) : 0U;
        size_t received = 0U;
        if (complete && available != 0U) {
            complete = (fseek)(file, static_cast<long>(span.offset), SEEK_SET) == 0;
            if (complete) {
                received = (fread)(out, 1U, available, file);
                complete = received == available;
            }
        }
        if (transient != nullptr) (fclose)(transient);
        if (!complete && retained) CloseRead();
        EndAccess(AccessOperation::Read, received, elapsed.ElapsedMs());
        RecordAccess(AccessOperation::Read, offset, count, elapsed.ElapsedMs());
        if (!complete) return false;
#else
        (void)offset; (void)out; (void)count;
        return false;
#endif
        offset += count;
        out += count;
        size -= count;
    }
    return true;
}

bool LitchcraftFrameFile::WriteAt(size_t offset, const uint8_t* bytes, size_t size) {
    namespace fixed = modules::litchcraft_frame_store_layout;
    if ((bytes == nullptr && size != 0U) || offset > fixed::ARTWORK_FILE_CAPACITY ||
        size > fixed::ARTWORK_FILE_CAPACITY - offset) return false;
    while (size != 0U) {
        const size_t count = ContiguousBytes(offset, size);
        if (count == 0U) return false;
#if defined(ARDUINO) || defined(LITCH_NATIVE)
        PhysicalSpan span{};
        fxe::FixedString<105U> resolvedPath;
        if ((bytes == nullptr && count != 0U) || !ResolvePhysicalSpan(offset, count, span)) {
            errno = EINVAL; return false;
        }
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
        if (!esp_littlefs_mounted("spiffs")) { errno = ENODEV; return false; }
        if (!BuildPath(span.region, resolvedPath, true)) return false;
#elif defined(LITCH_NATIVE)
        if ((span.region == Region::Root0 || span.region == Region::Root1) &&
            m_blockRootWritesForTest.load(std::memory_order_acquire)) return false;
        if (!BuildPath(span.region, resolvedPath)) return false;
#endif
        if (IsBody(span.region) &&
            !PrepareBodyPart(BodyResident(span.region), BodyPart(span.region))) return false;
        const char* const path = resolvedPath.CStr();
        static constexpr std::array<uint8_t, ACCESS_BYTES> ERASED = [] {
            std::array<uint8_t, ACCESS_BYTES> value{};
            for (size_t index = 0U; index < value.size(); ++index) value[index] = 0xffU;
            return value;
        }();
        const auto elapsed = fxe::ElapsedTime::StartNow();
        BeginAccess();
        if (FileRegion(m_readRegion) == FileRegion(span.region)) CloseRead();
        bool complete = true;
        if (m_writeFd >= 0 && (!m_writeRegionValid || m_writeRegion != span.region)) {
            complete = CloseWrite();
        }
        if (complete && m_writeFd < 0) {
            m_writeFd = open(path, O_RDWR | O_CREAT, 0666);
            struct stat info{};
            complete = m_writeFd >= 0 && (fstat)(m_writeFd, &info) == 0 && info.st_size >= 0;
            if (complete) {
                m_writeRegion = span.region;
                m_writeRegionValid = true;
                m_writeCursor = 0U;
                m_writeSize = static_cast<size_t>(info.st_size);
            }
        }
        size_t sourceOffset = 0U;
        while (complete && sourceOffset < count) {
            const size_t target = span.offset + sourceOffset;
            const bool fill = m_writeSize < target;
            const size_t at = fill ? m_writeSize : target;
            const size_t bytesWritten = fill
                ? std::min(target - m_writeSize, ERASED.size())
                : std::min(ACCESS_BYTES, count - sourceOffset);
            complete = m_writeCursor == at ||
                lseek(m_writeFd, static_cast<off_t>(at), SEEK_SET) == static_cast<off_t>(at);
            if (complete) {
                const ssize_t written = write(
                    m_writeFd, fill ? ERASED.data() : bytes + sourceOffset, bytesWritten);
                complete = written == static_cast<ssize_t>(bytesWritten);
                if (!complete && written >= 0) errno = EIO;
            }
            if (complete) {
                m_writeCursor = at + bytesWritten;
                m_writeSize = std::max(m_writeSize, m_writeCursor);
                if (!fill) sourceOffset += bytesWritten;
            }
        }
        const int operationError = complete ? 0 : (errno != 0 ? errno : EIO);
        if (!complete) CloseWrite();
        m_writeError = operationError;
        EndAccess();
        RecordAccess(AccessOperation::Write, offset, count, elapsed.ElapsedMs());
        errno = operationError;
        if (!complete) return false;
#else
        (void)offset; (void)bytes; (void)count;
        return false;
#endif
        offset += count;
        bytes += count;
        size -= count;
    }
    return true;
}

bool LitchcraftFrameFile::SyncWrites() {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    if (m_writeFd < 0) { errno = m_writeError; return m_writeError == 0; }
    const Region region = m_writeRegion;
    const size_t fileBytes = m_writeSize;
    const size_t cursor = m_writeCursor;
    const auto elapsed = fxe::ElapsedTime::StartNow();
    BeginAccess();
    const bool complete = CloseWrite();
    const int operationError = errno;
    EndAccess();
    const uint32_t elapsedMs = elapsed.ElapsedMs();
    RecordAccess(AccessOperation::Sync, 0U, 0U, elapsedMs);
    RecordSync(region, fileBytes, cursor, elapsedMs);
    errno = operationError;
    return complete;
#else
    return false;
#endif
}

void LitchcraftFrameFile::CloseRead() {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    if (m_readFile != nullptr) (fclose)(m_readFile);
    m_readFile = nullptr;
    m_readSize = 0U;
#endif
}

bool LitchcraftFrameFile::CloseWrite() {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    if (m_writeFd < 0) { errno = m_writeError; return m_writeError == 0; }
    int operationError = fsync(m_writeFd) == 0 ? 0 : errno;
    if (close(m_writeFd) != 0 && operationError == 0) operationError = errno;
    m_writeFd = -1;
    m_writeRegionValid = false;
    m_writeCursor = 0U;
    m_writeSize = 0U;
    m_writeError = operationError;
    errno = operationError;
    return operationError == 0;
#else
    return false;
#endif
}

bool LitchcraftFrameFile::Benchmark(const uint32_t bytes,
                                    BenchmarkResult& out, uint8_t* const page,
                                    const size_t pageCapacity) {
    out = {};
    if (bytes == 0U ||
        bytes > modules::litchcraft_frame_store::BODY_BANK_BYTES) return false;
    // The caller owns the full page for this synchronous diagnostic.
    if (page == nullptr || pageCapacity < ACCESS_BYTES || !Remove()) return false;
    for (size_t index = 0U; index < ACCESS_BYTES; ++index) {
        page[index] = static_cast<uint8_t>(index * 37U + 11U);
    }
    ResetAccessTiming();
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    auto elapsed = fxe::ElapsedTime::StartNow();
#endif
    const size_t logicalBase =
        modules::litchcraft_frame_store::FrameStore::BodyOffset(1U);
    for (uint32_t offset = 0U; offset < bytes; offset += ACCESS_BYTES) {
        const size_t count = std::min<size_t>(ACCESS_BYTES, bytes - offset);
        if (!WriteAt(logicalBase + offset, page, count)) {
            (void)Remove();
            return false;
        }
    }
    if (!SyncWrites()) {
        (void)Remove();
        return false;
    }
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    out.writeMs = elapsed.ElapsedMs();
    elapsed.Reset();
#endif
    for (uint32_t offset = 0U; offset < bytes; offset += ACCESS_BYTES) {
        const size_t count = std::min<size_t>(ACCESS_BYTES, bytes - offset);
        if (!ReadAt(logicalBase + offset, page, count)) {
            (void)Remove();
            return false;
        }
        for (size_t index = 0U; index < count; ++index) {
            if (page[index] != static_cast<uint8_t>(index * 37U + 11U)) {
                (void)Remove();
                return false;
            }
        }
    }
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    out.readMs = elapsed.ElapsedMs();
#endif
    out.passed = Remove();
    out.bytes = bytes;
    out.maximumAccessMs = m_accessTiming.maximumMs;
    out.accesses = m_accessTiming.accesses;
    return out.passed;
}

} // namespace firmware
