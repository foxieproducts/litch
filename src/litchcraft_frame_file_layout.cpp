#include "firmware/litchcraft_frame_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#if defined(ARDUINO) || defined(LITCH_NATIVE)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
#include <esp_littlefs.h>
#endif

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"

namespace firmware {
namespace fixed = modules::litchcraft_frame_store_layout;

LitchcraftFrameFile::Region LitchcraftFrameFile::BodyRegion(const uint8_t resident, const uint8_t part) {
    if (part == 0U) return resident == 0U ? Region::Body0 : Region::Body1;
    return static_cast<Region>(static_cast<uint8_t>(Region::BodyPartBase) +
        resident * (BODY_FILES_PER_BANK - 1U) + part - 1U);
}

bool LitchcraftFrameFile::IsBody(const Region region) {
    const auto value = static_cast<uint8_t>(region);
    return region == Region::Body0 || region == Region::Body1 ||
        (value >= static_cast<uint8_t>(Region::BodyPartBase) &&
         value < static_cast<uint8_t>(Region::BodyPartBase) + 2U * (BODY_FILES_PER_BANK - 1U));
}

uint8_t LitchcraftFrameFile::BodyResident(const Region region) {
    if (region == Region::Body0 || region == Region::Body1) return region == Region::Body0 ? 0U : 1U;
    return (static_cast<uint8_t>(region) - static_cast<uint8_t>(Region::BodyPartBase)) / (BODY_FILES_PER_BANK - 1U);
}

uint8_t LitchcraftFrameFile::BodyPart(const Region region) {
    if (region == Region::Body0 || region == Region::Body1) return 0U;
    return 1U + (static_cast<uint8_t>(region) - static_cast<uint8_t>(Region::BodyPartBase)) % (BODY_FILES_PER_BANK - 1U);
}

bool LitchcraftFrameFile::BuildPath(const Region region, fxe::FixedString<105U>& out, const bool mounted) const {
    const bool body = IsBody(region);
    const uint8_t index = body ? 2U + BodyResident(region) : static_cast<uint8_t>(FileRegion(region));
    if (index >= 4U || m_paths[index] == nullptr) { errno = EINVAL; return false; }
    out.Clear();
    const uint8_t part = body ? BodyPart(region) : 0U;
    // The mount prefix is not part of the logical diagnostic path limit.
    if (std::string_view(m_paths[index]).size() + (body ? 3U : 0U) > 95U) {
        errno = ENAMETOOLONG; return false;
    }
    const bool complete = out.Append(mounted ? "/littlefs" : "") && out.Append(m_paths[index]) &&
        (!body || (out.PushBack('.') && out.PushBack(static_cast<char>('0' + part / 10U)) &&
                   out.PushBack(static_cast<char>('0' + part % 10U))));
    if (!complete) errno = ENAMETOOLONG;
    return complete;
}


size_t LitchcraftFrameFile::ContiguousBytes(const size_t offset, const size_t remaining) {
    const size_t body0 = fixed::Layout::BodyOffset(0U);
    if (offset >= body0 && offset < fixed::FRAME_FILES_CAPACITY) {
        return std::min(remaining, BODY_FILE_BYTES - (offset - body0) % BODY_FILE_BYTES);
    }
    const size_t boundaries[] = {fixed::ROOT0_FILE_BYTES, body0, fixed::FRAME_FILES_CAPACITY,
        fixed::Layout::SummaryOffset(1U, 0U), fixed::ARTWORK_FILE_CAPACITY};
    for (const size_t end : boundaries) if (offset < end) return std::min(remaining, end - offset);
    return 0U;
}

bool LitchcraftFrameFile::ResolvePhysicalSpan(const size_t logicalOffset, const size_t size, PhysicalSpan& out) {
    if (logicalOffset > fixed::ARTWORK_FILE_CAPACITY || size > fixed::ARTWORK_FILE_CAPACITY - logicalOffset) return false;
    const size_t body0 = fixed::Layout::BodyOffset(0U);
    if (logicalOffset >= body0 && logicalOffset < fixed::FRAME_FILES_CAPACITY) {
        const uint8_t resident = logicalOffset >= fixed::Layout::BodyOffset(1U) ? 1U : 0U;
        const size_t at = logicalOffset - fixed::Layout::BodyOffset(resident);
        const uint8_t part = at / BODY_FILE_BYTES;
        const size_t inPart = at % BODY_FILE_BYTES;
        if (size > BODY_FILE_BYTES - inPart) return false;
        out = {BodyRegion(resident, part), inPart};
        return true;
    }
    if (logicalOffset >= fixed::FRAME_FILES_CAPACITY) {
        const bool second = logicalOffset >= fixed::Layout::SummaryOffset(1U, 0U);
        const size_t begin = fixed::Layout::SummaryOffset(second ? 1U : 0U, 0U);
        if (size > fixed::SUMMARY_RESIDENT_BYTES - (logicalOffset - begin)) return false;
        out = {second ? Region::Summary1 : Region::Summary0,
            (second ? fixed::ROOT1_FILE_BYTES : fixed::ROOT0_FILE_BYTES) + logicalOffset - begin};
        return true;
    }
    const bool second = logicalOffset >= fixed::ROOT0_FILE_BYTES;
    const size_t begin = second ? fixed::ROOT0_FILE_BYTES : 0U;
    const size_t end = second ? body0 : fixed::ROOT0_FILE_BYTES;
    if (size > end - logicalOffset) return false;
    out = {second ? Region::Root1 : Region::Root0, logicalOffset - begin};
    return true;
}

bool LitchcraftFrameFile::PrepareBodyPart(const uint8_t resident, const uint8_t part) {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    static_assert(BODY_FILES_PER_BANK <= 16U, "replacement preparation mask must cover every body part");
    if (resident > 1U || part >= BODY_FILES_PER_BANK) { errno = EINVAL; return false; }
    const uint16_t bit = uint16_t(uint16_t{1U} << part);
    if ((m_pendingBodyPreparation[resident] & bit) == 0U) return true;
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    if (!esp_littlefs_mounted("spiffs")) { errno = ENODEV; return false; }
    constexpr bool mounted = true;
#else
    constexpr bool mounted = false;
#endif
    fxe::FixedString<105U> path;
    if (!BuildPath(BodyRegion(resident, part), path, mounted)) return false;
    const auto elapsed = fxe::ElapsedTime::StartNow();
    BeginAccess();
    CloseRead();
    int error = (m_writeFd < 0 || CloseWrite()) ? 0 : errno;
    const int fd = error == 0 ? open(path.CStr(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (error == 0 && fd < 0) error = errno;
    if (fd >= 0) {
        if (fsync(fd) != 0) error = errno;
        struct stat info{};
        // The pinned VFS can hide an implicit truncation-sync failure. Prove
        // persisted zero EOF before this part receives any replacement bytes.
        if (error == 0) {
            if ((fstat)(fd, &info) != 0) error = errno != 0 ? errno : EIO;
            else if (info.st_size != 0) error = EIO;
        }
        if (close(fd) != 0 && error == 0) error = errno;
    }
    EndAccess();
    RecordAccess(AccessOperation::Write, fixed::Layout::BodyOffset(resident) + part * BODY_FILE_BYTES,
                 0U, elapsed.ElapsedMs());
    if (error != 0) { errno = error; return false; }
    m_pendingBodyPreparation[resident] &= uint16_t(~bit);
    return true;
#else
    (void)resident; (void)part; return false;
#endif
}

bool LitchcraftFrameFile::PrepareBank(const uint8_t resident) {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
    if (resident > 1U) { errno = EINVAL; return false; }
    // Both inactive roots are already durably invalid. Preserve the same
    // per-part durability proof, but defer untouched files until first use.
    m_pendingBodyPreparation[resident] = uint16_t((uint32_t{1U} << BODY_FILES_PER_BANK) - 1U);
    if (!PrepareBodyPart(resident, 0U)) return false;
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    constexpr bool mounted = true;
#else
    constexpr bool mounted = false;
#endif
    // The obsolete monolith is unreachable after root invalidation and a
    // checked part zero. The other resident remains untouched.
    fxe::FixedString<96U> oldPath;
    if (m_paths[2U + resident] == nullptr || !oldPath.Append(mounted ? "/littlefs" : "") || !oldPath.Append(m_paths[2U + resident])) {
        errno = EINVAL; return false;
    }
    BeginAccess();
    const bool removed = unlink(oldPath.CStr()) == 0 || errno == ENOENT;
    const int removeError = removed ? 0 : errno;
    EndAccess();
    errno = removeError;
    return removed;
#else
    (void)resident; return false;
#endif
}

bool LitchcraftFrameFile::Remove() {
#if defined(ARDUINO) || defined(LITCH_NATIVE)
#if defined(ARDUINO) && !defined(LITCH_NATIVE)
    if (!esp_littlefs_mounted("spiffs")) return false;
    constexpr bool mounted = true;
#else
    constexpr bool mounted = false;
#endif
    BeginAccess();
    CloseRead();
    if (!CloseWrite()) { const int error = errno; EndAccess(); errno = error; return false; }
    bool complete = true;
    int firstError = 0;
    const auto remove = [&](const char* path) {
        if (unlink(path) != 0 && errno != ENOENT) {
            if (firstError == 0) firstError = errno;
            complete = false;
        }
    };
    fxe::FixedString<105U> path;
    for (uint8_t resident = 0U; resident < 2U; ++resident) {
        for (uint8_t part = 0U; part < BODY_FILES_PER_BANK; ++part) {
            if (BuildPath(BodyRegion(resident, part), path, mounted)) remove(path.CStr());
            else { complete = false; if (firstError == 0) firstError = errno; }
        }
        path.Clear();
        if (m_paths[2U + resident] != nullptr && path.Append(mounted ? "/littlefs" : "") &&
            path.Append(m_paths[2U + resident])) remove(path.CStr());
        else { complete = false; if (firstError == 0) firstError = EINVAL; }
    }
    // Keep the recognizable old root if body cleanup fails or is interrupted,
    // so the one-time development cleanup remains retryable after reboot.
    if (complete) {
        for (uint8_t root = 0U; root < 2U; ++root) {
            if (BuildPath(root == 0U ? Region::Root0 : Region::Root1, path, mounted)) remove(path.CStr());
            else { complete = false; if (firstError == 0) firstError = errno; }
        }
    }
    EndAccess();
    errno = firstError;
    return complete;
#else
    return false;
#endif
}

static_assert(LitchcraftFrameFile::BODY_FILE_BYTES == 17552U);
static_assert(LitchcraftFrameFile::BODY_FILES_PER_BANK * LitchcraftFrameFile::BODY_FILE_BYTES == fixed::BODY_BANK_BYTES);
} // namespace firmware
