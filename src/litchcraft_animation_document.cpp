#include "modules/litchcraft_animation.hpp"

#include <algorithm>
#include <utility>

namespace modules::litchcraft_animation {

bool DocumentsEqual(const Document& left, const Document& right) {
    if (left.frameCount != right.frameCount || left.topology != right.topology ||
        left.playbackOnEntry != right.playbackOnEntry ||
        left.sparkleSeed != right.sparkleSeed) return false;
    const auto sameColor = [](const Rgb a, const Rgb b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };
    for (size_t i = 0U; i < left.favorites.size(); ++i) {
        if (!sameColor(left.favorites[i], right.favorites[i])) return false;
    }
    for (size_t frame = 0U; frame < left.frameCount; ++frame) {
        const auto& a = left.frames[frame];
        const auto& b = right.frames[frame];
        if (a.durationMs != b.durationMs) return false;
        for (size_t i = 0U; i < a.emitters.size(); ++i) {
            if (!sameColor(a.emitters[i].color, b.emitters[i].color) ||
                a.emitters[i].effect != b.emitters[i].effect ||
                a.emitters[i].durationMs != b.emitters[i].durationMs) return false;
        }
    }
    return true;
}

bool IsValidDocument(const Document& document) {
    if (!IsValidDocumentHeader(SCHEMA, document.frameCount, document.topology,
                               document.playbackOnEntry ? 1U : 0U,
                               document.sparkleSeed)) {
        return false;
    }
    for (uint8_t frameIndex = 0U; frameIndex < document.frameCount; ++frameIndex) {
        const Frame& frame = document.frames[frameIndex];
        if (!IsValidFrameDuration(frame.durationMs) ||
            std::any_of(frame.emitters.begin(), frame.emitters.end(),
                        [](const Emitter& emitter) {
                            return !IsValidEmitter(emitter);
                        })) {
            return false;
        }
    }
    return true;
}

bool ValidatedDocument::TryAdopt(const Document& candidate,
                                 ValidatedDocument& out) {
    if (!IsValidDocument(candidate)) {
        return false;
    }
    out.m_document = candidate;
    out.m_adopted = true;
    return true;
}

bool ValidatedDocument::TryAdoptBySwap(Document& candidate,
                                       ValidatedDocument& out) {
    if (!IsValidDocument(candidate)) {
        return false;
    }
    using std::swap;
    swap(out.m_document.frameCount, candidate.frameCount);
    swap(out.m_document.topology, candidate.topology);
    swap(out.m_document.playbackOnEntry, candidate.playbackOnEntry);
    swap(out.m_document.sparkleSeed, candidate.sparkleSeed);
    for (size_t index = 0U; index < candidate.favorites.size(); ++index) {
        swap(out.m_document.favorites[index], candidate.favorites[index]);
    }
    for (size_t frameIndex = 0U; frameIndex < candidate.frames.size();
         ++frameIndex) {
        Frame& currentFrame = out.m_document.frames[frameIndex];
        Frame& candidateFrame = candidate.frames[frameIndex];
        swap(currentFrame.durationMs, candidateFrame.durationMs);
        for (size_t emitterIndex = 0U;
             emitterIndex < candidateFrame.emitters.size(); ++emitterIndex) {
            Emitter& current = currentFrame.emitters[emitterIndex];
            Emitter& incoming = candidateFrame.emitters[emitterIndex];
            swap(current.color, incoming.color);
            swap(current.effect, incoming.effect);
            swap(current.durationMs, incoming.durationMs);
        }
    }
    out.m_adopted = true;
    return true;
}

} // namespace modules::litchcraft_animation
