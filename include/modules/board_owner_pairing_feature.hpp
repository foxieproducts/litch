#pragma once

namespace modules {

// S3 explicitly enables the accepted runtime. Preview builds additionally
// permit the private local-USB developer consent adapter.
#if (defined(LITCH_OWNER_PAIRING) && LITCH_OWNER_PAIRING) || \
    (defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW)
inline constexpr bool OWNER_PAIRING_ENABLED = true;
#else
inline constexpr bool OWNER_PAIRING_ENABLED = false;
#endif

#if defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW
inline constexpr bool OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED = true;
#else
inline constexpr bool OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED = false;
#endif

} // namespace modules
