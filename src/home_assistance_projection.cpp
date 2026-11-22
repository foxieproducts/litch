#include "modules/home_assistance_projection.hpp"

namespace modules {
namespace {

bool IsValidPresentation(const HomeAssistancePresentation presentation) {
    return presentation >= HomeAssistancePresentation::Hidden &&
           presentation <= HomeAssistancePresentation::LegalGuide;
}

bool IsValidProjection(const HomeAssistanceProjection& assistance,
                       const GameVariant variant) {
    if (!IsValidPresentation(assistance.presentation)) {
        return false;
    }

    const bool requestedState =
        assistance.presentation == HomeAssistancePresentation::Pending ||
        assistance.presentation == HomeAssistancePresentation::Thinking ||
        assistance.presentation == HomeAssistancePresentation::HintReady ||
        assistance.presentation ==
            HomeAssistancePresentation::HintRefreshing ||
        assistance.presentation ==
            HomeAssistancePresentation::DuckCompound ||
        assistance.presentation ==
            HomeAssistancePresentation::DuckRelocation;
    if ((requestedState && !assistance.hintRequested) ||
        (assistance.presentation == HomeAssistancePresentation::Off &&
         assistance.hintRequested)) {
        return false;
    }

    const bool primaryMove =
        assistance.presentation == HomeAssistancePresentation::HintReady ||
        assistance.presentation ==
            HomeAssistancePresentation::HintRefreshing ||
        assistance.presentation ==
            HomeAssistancePresentation::DuckCompound;
    if (primaryMove &&
        (assistance.fromSquare >= 64U || assistance.toSquare >= 64U ||
         assistance.fromSquare == assistance.toSquare)) {
        return false;
    }
    if (assistance.presentation ==
            HomeAssistancePresentation::HintRefreshing &&
        variant == GameVariant::Duck) {
        return false;
    }

    const bool legalDuckMove =
        assistance.presentation == HomeAssistancePresentation::LegalGuide &&
        (assistance.duckFromSquare != runtime::CHESS_INTERACTION_INVALID_SQUARE ||
         assistance.duckToSquare != runtime::CHESS_INTERACTION_INVALID_SQUARE);
    const bool duckMove = legalDuckMove ||
        assistance.presentation ==
            HomeAssistancePresentation::DuckCompound ||
        assistance.presentation ==
            HomeAssistancePresentation::DuckRelocation;
    if (duckMove &&
        (variant != GameVariant::Duck ||
         assistance.duckFromSquare >= 64U ||
         assistance.duckToSquare >= 64U ||
         assistance.duckFromSquare == assistance.duckToSquare)) {
        return false;
    }
    if (legalDuckMove &&
        (!assistance.hintRequested || assistance.fromSquare >= 64U ||
         assistance.toSquare >= 64U)) {
        return false;
    }
    if (assistance.presentation ==
            HomeAssistancePresentation::LegalGuide &&
        (assistance.legalSourceSquare >= 64U ||
         assistance.legalDestinationMask == 0ULL ||
         (assistance.legalDestinationMask &
          (1ULL << assistance.legalSourceSquare)) != 0ULL ||
         ((assistance.fromSquare < 64U || assistance.toSquare < 64U) &&
          (!assistance.hintRequested ||
           assistance.fromSquare != assistance.legalSourceSquare ||
           assistance.toSquare >= 64U ||
           (assistance.legalDestinationMask & (1ULL << assistance.toSquare)) == 0ULL)))) {
        return false;
    }
    return true;
}

void FormatSquare(const uint8_t square, char (&out)[3]) {
    out[0] = '-';
    out[1] = '-';
    out[2] = '\0';
    if (square >= 64U) {
        return;
    }
    out[0] = static_cast<char>('a' + (square % 8U));
    out[1] = static_cast<char>('8' - (square / 8U));
}

} // namespace

bool TryFormatHomeAssistanceProjection(
    const HomeAssistanceProjection& assistance,
    const GameVariant variant,
    HomeAssistanceExtra& outExtra) {
    outExtra.Clear();
    if (!assistance.valid) {
        return true;
    }
    if (!IsValidProjection(assistance, variant)) {
        return false;
    }

    const uint8_t flags =
        static_cast<uint8_t>(assistance.hintRequested ? 1U : 0U) |
        static_cast<uint8_t>(assistance.manualAffordanceUseful ? 2U : 0U) |
        static_cast<uint8_t>(assistance.coachingForced ? 4U : 0U);
    char from[3] = {};
    char to[3] = {};
    char duckFrom[3] = {};
    char duckTo[3] = {};
    char legalSource[3] = {};
    FormatSquare(assistance.fromSquare, from);
    FormatSquare(assistance.toSquare, to);
    FormatSquare(assistance.duckFromSquare, duckFrom);
    FormatSquare(assistance.duckToSquare, duckTo);
    FormatSquare(assistance.legalSourceSquare, legalSource);
    switch (assistance.presentation) {
        case HomeAssistancePresentation::Hidden:
            return outExtra.Format(
                ",\"as\":\"%ux\"", static_cast<unsigned>(flags));
        case HomeAssistancePresentation::Off:
            return outExtra.Format(
                ",\"as\":\"%uo\"", static_cast<unsigned>(flags));
        case HomeAssistancePresentation::Pending:
            return outExtra.Format(
                ",\"as\":\"%up\"", static_cast<unsigned>(flags));
        case HomeAssistancePresentation::Thinking:
            return outExtra.Format(
                ",\"as\":\"%ut\"", static_cast<unsigned>(flags));
        case HomeAssistancePresentation::HintReady:
            return outExtra.Format(
                ",\"as\":\"%uh%s%s\"", static_cast<unsigned>(flags),
                from, to);
        case HomeAssistancePresentation::HintRefreshing:
            return outExtra.Format(
                ",\"as\":\"%ur%s%s\"", static_cast<unsigned>(flags),
                from, to);
        case HomeAssistancePresentation::DuckCompound:
            return outExtra.Format(
                ",\"as\":\"%ub%s%s%s%s\"",
                static_cast<unsigned>(flags), from, to,
                duckFrom, duckTo);
        case HomeAssistancePresentation::DuckRelocation:
            return outExtra.Format(
                ",\"as\":\"%ud%s%s\"", static_cast<unsigned>(flags),
                duckFrom, duckTo);
        case HomeAssistancePresentation::LegalGuide:
            if (assistance.toSquare < 64U) {
                return outExtra.Format(
                    ",\"as\":\"%ul%s%016llX%s\"",
                    static_cast<unsigned>(flags), legalSource,
                    static_cast<unsigned long long>(assistance.legalDestinationMask), to);
            }
            return outExtra.Format(
                ",\"as\":\"%ul%s%016llX\"",
                static_cast<unsigned>(flags), legalSource,
                static_cast<unsigned long long>(
                    assistance.legalDestinationMask));
    }
    return false;
}

} // namespace modules
