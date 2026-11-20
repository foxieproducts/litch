#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/ble_device_capabilities.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_palette.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/game_clock.hpp"
#include "virtual_litch_session.hpp"

namespace {
int g_failures = 0;
using Art = modules::LitchcraftSession;
void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}
Art::Packet Request(Art::Op op, uint32_t session, uint16_t id, std::initializer_list<uint8_t> payload = {}) {
    Art::Packet p;
    p.bytes[0] = Art::MAGIC;
    p.bytes[1] = uint8_t(op);
    for (int i = 0; i < 4; ++i) {
        p.bytes[2 + i] = session >> (i * 8);
    }
    p.bytes[6] = id;
    p.bytes[7] = id >> 8;
    p.size = 8;
    for (auto value : payload) {
        p.bytes[p.size++] = value;
    }
    return p;
}
void Send(Art& art, Art::Op op, uint16_t id, std::initializer_list<uint8_t> payload = {}) {
    const auto p = Request(op, op == Art::Op::Enter ? 0U : art.SessionId(), id, payload);
    art.HandlePacket(p.bytes.data(), p.size, true, 0U);
}
void Drain(Art& art) {
    Art::Packet p;
    unsigned count = 0;
    while (art.PeekPacket(p) && count++ < 1000) {
        art.AcceptPacket();
    }
}
bool HasStatus(Art& art, const uint16_t request, const Art::Status status) {
    Art::Packet p;
    bool found = false;
    while (art.PeekPacket(p)) {
        found = found || (p.bytes[1] == uint8_t(Art::Event::State) &&
                          (uint16_t(p.bytes[10]) |
                           uint16_t(p.bytes[11]) << 8U) == request &&
                          p.bytes[12] == uint8_t(status));
        art.AcceptPacket();
    }
    return found;
}
bool Color(const modules::Rgb c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}
void CompactModeReturnsCanonicalStatusAndAck() {
    Art art;
    art.Configure(3U, 1153U);
    art.BeginConnection();
    auto enter = Request(Art::Op::Enter, 0U, 1U);
    Check(art.HandleCompactMode(enter.bytes.data(), enter.size, false, 0U) == Art::Status::Busy &&
              HasStatus(art, 1U, Art::Status::Busy) && !art.IsActive(),
          "Ordered mode helper returns and acknowledges canonical eligibility rejection");
    enter = Request(Art::Op::Enter, 0U, 2U);
    Check(art.HandleCompactMode(enter.bytes.data(), enter.size, true, 1U) == Art::Status::BoardNotEmpty &&
              HasStatus(art, 2U, Art::Status::BoardNotEmpty) && !art.IsActive(),
          "Ordered mode helper preserves occupied-board refusal");
    enter = Request(Art::Op::Enter, 0U, 3U);
    Check(art.HandleCompactMode(enter.bytes.data(), enter.size, true, 0U) == Art::Status::Ok &&
              HasStatus(art, 3U, Art::Status::Ok) && art.IsActive(),
          "Ordered Enter admits inactive mode and retains exact compact ACK");
    const uint32_t session = art.SessionId(), revision = art.Revision();
    Check(art.HandleCompactMode(enter.bytes.data(), enter.size, true, 0U) == Art::Status::Ok &&
              HasStatus(art, 3U, Art::Status::Ok) && art.Revision() == revision && art.SessionId() == session,
          "canonical compact receipt replay remains observational");
    auto exit = Request(Art::Op::Exit, session, 3U);
    Check(art.HandleCompactMode(exit.bytes.data(), exit.size, true, 0U) == Art::Status::Malformed &&
              HasStatus(art, 3U, Art::Status::Malformed) && art.IsActive(),
          "changed compact mode request ID cannot mutate authority");
    exit = Request(Art::Op::Exit, session + 1U, 4U);
    Check(art.HandleCompactMode(exit.bytes.data(), exit.size, true, 0U) == Art::Status::StaleSession &&
              HasStatus(art, 4U, Art::Status::StaleSession) && art.IsActive(),
          "Ordered mode retains canonical stale-session check");
    exit = Request(Art::Op::Exit, session, 5U);
    Check(art.HandleCompactMode(exit.bytes.data(), exit.size, true, 0U) == Art::Status::Ok &&
              HasStatus(art, 5U, Art::Status::Ok) && !art.IsActive(),
          "Ordered Exit returns the actual mutation result and compact ACK");
    auto invalid = Request(Art::Op::Tool, session, 6U);
    Check(art.HandleCompactMode(invalid.bytes.data(), invalid.size, true, 0U) == Art::Status::Malformed &&
              HasStatus(art, 6U, Art::Status::Malformed) && !art.IsActive(),
          "mode-only adapter rejects non-mode compact commands");
    std::puts("[COMPACT_ORDERED_MODE] eligibility=Busy occupied=BoardNotEmpty replay=once stale=fenced ACK=preserved");
}

void ModeReceiptsSurvivePublicationResync() {
    for (const auto mode : {Art::Op::Exit, Art::Op::Enter}) {
        Art art;
        art.Configure(3U, 953U);
        art.BeginConnection();
        Send(art, Art::Op::Enter, 1U);
        Check(HasStatus(art, 1U, Art::Status::Ok), "mode resync journey starts with acknowledged Enter");
        uint16_t request = 2U;
        // The existing adversarial journey produces real bounded queue overflow.
        for (uint16_t index = 0U; index < 110U; ++index)
            Send(art, Art::Op::Tool, request++, {0U, 15U, uint8_t(index), 3U, 5U});
        Art::Packet packet;
        while (art.PeekPacket(packet) && packet.bytes[1U] == uint8_t(Art::Event::State))
            art.AcceptPacket();
        Check(art.PeekPacket(packet) && packet.bytes[1U] == uint8_t(Art::Event::ResyncRequired),
              "actual overflow leaves canvas resync required before mode request");

        const auto replies = [&](uint16_t id, Art::Status status, bool active, bool expectResync) {
            unsigned matching = 0U, count = 0U;
            bool firstMatches = false, resync = false;
            while (art.PeekPacket(packet) && count < 1000U) {
                const auto event = static_cast<Art::Event>(packet.bytes[1U]);
                const bool matchingState = event == Art::Event::State && packet.size == 20U &&
                    (uint16_t(packet.bytes[10U]) | uint16_t(packet.bytes[11U]) << 8U) == id;
                if (count == 0U) firstMatches = matchingState;
                if (matchingState) {
                    ++matching;
                    Check(packet.bytes[12U] == uint8_t(status) &&
                              ((packet.bytes[13U] & 1U) != 0U) == active,
                          "resync control ACK carries truthful result and actual mode");
                }
                Check(event == Art::Event::State || event == Art::Event::ResyncRequired,
                      "incremental artwork stays embargoed while control receipts pass");
                resync = resync || event == Art::Event::ResyncRequired;
                art.AcceptPacket();
                ++count;
            }
            Check(firstMatches && matching != 0U, "current compact control ACK precedes pending canvas resync");
            Check(!expectResync || resync, "control ACK does not erase required canvas recovery");
            std::printf("[MODE_RESYNC_RECEIPT] request=%u active=%u status=%u matching=%u resync=%u packets=%u\n",
                        id, art.IsActive(), uint8_t(status), matching, resync, count);
            return matching;
        };
        if (mode == Art::Op::Enter) art.Exit(); // Production board-local departure.
        const auto modeRequest = request++;
        Send(art, mode, modeRequest);
        const bool active = mode == Art::Op::Enter;
        Check(art.IsActive() == active, "mode transition remains immediate despite artwork resync");
        replies(modeRequest, Art::Status::Ok, active, true);

        if (mode == Art::Op::Exit) {
            const auto occupied = Request(Art::Op::Enter, 0U, request++);
            art.HandlePacket(occupied.bytes.data(), occupied.size, true, 1ULL);
            replies(request - 1U, Art::Status::BoardNotEmpty, false, false);
            const auto busy = Request(Art::Op::Enter, 0U, request++);
            art.HandlePacket(busy.bytes.data(), busy.size, false, 0ULL);
            replies(request - 1U, Art::Status::Busy, false, false);
            const auto enter = request++;
            Send(art, Art::Op::Enter, enter);
            replies(enter, Art::Status::Ok, true, false);
            const auto revision = art.Revision();
            Send(art, Art::Op::Enter, enter); // Exact cached request retry.
            replies(enter, Art::Status::Ok, true, false);
            Send(art, Art::Op::Enter, request++); // New idempotent request.
            replies(request - 1U, Art::Status::Ok, true, false);
            Check(art.Revision() == revision, "cached and idempotent Enter preserve authoritative mode revision");

            Send(art, Art::Op::Tool, request++, {0U, 15U, 112U, 22U, 33U});
            replies(request - 1U, Art::Status::Ok, true, false);
            Send(art, Art::Op::Paint, request++, {1U, 0U, 3U, 0U});
            replies(request - 1U, Art::Status::Ok, true, false);
            Check(Color(art.Artwork()[0U], 112U, 22U, 33U),
                  "accepted artwork mutation still needs snapshot rather than incremental publication");
            art.HandlePacket(nullptr, 0U, true, 0ULL);
            Check(!art.PeekPacket(packet), "uncorrelated malformed State response remains embargoed");

            for (unsigned index = 0U; index < 96U; ++index)
                Send(art, Art::Op::Enter, request++);
            const auto full = request++;
            Send(art, Art::Op::Enter, full);
            const auto acceptedRevision = art.Revision();
            Send(art, Art::Op::Enter, full);
            Check(replies(full, Art::Status::Ok, true, true) == 2U && art.Revision() == acceptedRevision,
                  "full receipt queue retains incoming mode ACK and exact retry without repeating effect");
        }
        const auto inspect = request++;
        Send(art, Art::Op::Inspect, inspect);
        Check(HasStatus(art, inspect, Art::Status::Ok), "existing Inspect restores coherent publication baseline");
        const auto next = request++;
        Send(art, art.IsActive() ? Art::Op::Exit : Art::Op::Enter, next);
        Check(HasStatus(art, next, Art::Status::Ok), "next mode request recovers through normal acknowledgement path");
    }
}
void ReplacementPresenceRetainsSparseEdgesAndConnectionOwnership() {
    Art art;
    art.Configure(3U, 953U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    uint16_t request = 2U;
    const auto original = art.Artwork();
    const auto revision = art.Revision();
    Send(art, Art::Op::ReplaceBegin, request++);
    Drain(art);
    const auto chunk = [&](const uint16_t index, const uint8_t red) {
        const uint16_t id = request++;
        Send(art, Art::Op::ReplaceChunk, id,
             {static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8U),
              red, 20U, 30U});
        return id;
    };
    Check(HasStatus(art, chunk(255U, 77U), Art::Status::Ok) &&
              HasStatus(art, chunk(0U, 66U), Art::Status::Ok),
          "replacement tracks both edge indices in a sparse out-of-order upload");
    Check(HasStatus(art, chunk(255U, 77U), Art::Status::Ok),
          "identical repeated replacement RGB is accepted without losing presence");
    Check(HasStatus(art, chunk(255U, 78U), Art::Status::Malformed),
          "conflicting repeated replacement RGB remains rejected");
    const uint16_t incomplete = request++;
    Send(art, Art::Op::ReplaceCommit, incomplete);
    Check(HasStatus(art, incomplete, Art::Status::Incomplete) &&
              art.Revision() == revision &&
              Color(art.Artwork()[0U], original[0U].r, original[0U].g, original[0U].b),
          "sparse presence cannot commit or change the visible canvas");
    // A superseding connection must clear both the transaction and all bits.
    art.BeginConnection();
    request = 1U;
    Send(art, Art::Op::ReplaceBegin, request++);
    Drain(art);
    for (uint16_t index = 1U; index < 256U; ++index) {
        Check(HasStatus(art, chunk(index, static_cast<uint8_t>(index)),
                        Art::Status::Ok),
              "new connection admits every replacement slot through the production ingress");
    }
    const uint16_t missingZero = request++;
    Send(art, Art::Op::ReplaceCommit, missingZero);
    Check(HasStatus(art, missingZero, Art::Status::Incomplete) &&
              art.Revision() == revision,
          "old connection's slot zero cannot complete the new replacement");
    Check(HasStatus(art, chunk(0U, 0U), Art::Status::Ok),
          "last missing slot completes all 256 current-connection presence bits");
    const uint16_t commit = request++;
    Send(art, Art::Op::ReplaceCommit, commit);
    Check(HasStatus(art, commit, Art::Status::Ok) &&
              art.Revision() == revision + 1U &&
              Color(art.Artwork()[0U], 0U, 20U, 30U) &&
              Color(art.Artwork()[255U], 255U, 20U, 30U),
          "complete replacement publishes all slots as one coherent revision");
    const uint16_t undo = request++;
    Send(art, Art::Op::Undo, undo);
    Check(HasStatus(art, undo, Art::Status::Ok) &&
              Color(art.Artwork()[255U], original[255U].r,
                    original[255U].g, original[255U].b),
          "replacement remains one undoable canvas operation");
    const uint16_t redo = request++;
    Send(art, Art::Op::Redo, redo);
    Check(art.RedoCount() == 0U &&
              Color(art.Artwork()[255U], 255U, 20U, 30U),
          "replacement redo restores the exact last slot");
    Art::Packet recovery;
    Check(art.PeekPacket(recovery) && recovery.size == 20U &&
              recovery.bytes[1U] == static_cast<uint8_t>(Art::Event::State) &&
              (uint16_t(recovery.bytes[10U]) | uint16_t(recovery.bytes[11U]) << 8U) == redo &&
              recovery.bytes[12U] == static_cast<uint8_t>(Art::Status::Ok),
          "dense replacement redo keeps its matching receipt ahead of canvas repair");
    art.AcceptPacket();
    Check(art.PeekPacket(recovery) &&
              recovery.bytes[1] == static_cast<uint8_t>(Art::Event::ResyncRequired),
          "dense replacement redo requests coherent repair when its cell echoes overflow");
    const uint16_t snapshot = request++;
    Send(art, Art::Op::Snapshot, snapshot);
    Check(HasStatus(art, snapshot, Art::Status::Ok) &&
              Color(art.Artwork()[255U], 255U, 20U, 30U),
          "snapshot repair resumes the connection without losing replacement redo");
}

void PhysicalHistoryDuringSnapshot() {
    Art art;
    art.Configure(3U, 950U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Send(art, Art::Op::Paint, 2U, {1U, 0U, 3U, 0U});
    Send(art, Art::Op::Paint, 3U, {2U, 0U, 3U, 1U});
    Send(art, Art::Op::Paint, 4U, {3U, 0U, 3U, 2U});
    Drain(art);
    auto mirror = art.Artwork();
    uint32_t mirrorRevision = art.Revision();
    Send(art, Art::Op::Snapshot, 5U);
    Art::Packet packet;
    for (unsigned i = 0U; i < 12U; ++i) {
        Check(art.PeekPacket(packet), "snapshot starts before repeated physical Undo");
        art.AcceptPacket();
    }
    art.PeekPacket(packet);
    const auto nextChunk = packet;
    Send(art, Art::Op::Snapshot, 5U);
    Check(art.PeekPacket(packet) && packet.bytes[1] == uint8_t(Art::Event::State),
          "identical in-flight Snapshot retry replays its frozen acknowledgement");
    art.AcceptPacket();
    Check(art.PeekPacket(packet) && packet.size == nextChunk.size &&
              packet.bytes == nextChunk.bytes,
          "identical in-flight Snapshot retry preserves the unsent cursor");
    for (unsigned i = 0U; i < 3U; ++i) {
        art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Press,
                           modules::UiOwner::P1});
        art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Release,
                           modules::UiOwner::P1});
    }
    unsigned cells = 0U;
    unsigned states = 0U;
    bool snapshotEnded = false;
    while (art.PeekPacket(packet)) {
        const auto event = static_cast<Art::Event>(packet.bytes[1]);
        Check(event != Art::Event::ResyncRequired,
              "repeated physical Undo during a snapshot never requests another snapshot");
        if (event == Art::Event::SnapshotEnd) {
            snapshotEnded = true;
        } else if (event == Art::Event::Cell) {
            Check(snapshotEnded, "history deltas follow the frozen snapshot end");
            uint32_t revision = 0U;
            for (uint8_t byte = 0U; byte < 4U; ++byte) {
                revision |= uint32_t(packet.bytes[6U + byte]) << (8U * byte);
            }
            Check(revision == ++mirrorRevision, "history Cell revisions remain contiguous");
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                if ((packet.bytes[13] & (1U << slot)) != 0U) {
                    mirror[packet.bytes[12] * 4U + slot] = {
                        packet.bytes[14], packet.bytes[15], packet.bytes[16]};
                }
            }
            ++cells;
        } else if (event == Art::Event::State) {
            ++states;
        }
        art.AcceptPacket();
    }
    Check(snapshotEnded && cells == 3U && states == 3U,
          "three single-square Undo presses cost six notifications after one coherent snapshot");
    Check(std::equal(mirror.begin(), mirror.end(), art.Artwork().begin(),
                     [](modules::Rgb a, modules::Rgb b) {
                         return Color(a, b.r, b.g, b.b);
                     }),
          "existing Cell-only client reconstructs the exact board artwork after repeated Undo");
}
void DetailedHistoryAndColorPick() {
    Art art;
    art.Configure(3U, 951U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    uint16_t request = 2U;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        Send(art, Art::Op::Tool, request++, {0U, uint8_t(1U << slot), uint8_t(10U + slot), 20U, 30U});
        Send(art, Art::Op::Paint, request++, {uint8_t(slot + 1U), 0U, 3U, 9U});
    }
    const auto original = art.Artwork();
    Send(art, Art::Op::Tool, request++, {0U, 15U, 255U, 116U, 48U});
    const auto originalHistory = art.UndoCount();
    Drain(art);
    art.ObserveOccupancy(UINT64_C(1) << 9U);
    Art::Packet pickState;
    bool sawPicking = false;
    while (art.PeekPacket(pickState)) {
        if (pickState.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            sawPicking = pickState.size == 13U && pickState.bytes[12] == 1U;
        }
        art.AcceptPacket();
    }
    Check(sawPicking,
          "physical color-pick progress is published to the connected app");
    fxe::ManualMillis::AdvanceMs(Art::PICK_ARM_MS + Art::PICK_PROGRESS_MS);
    art.Update();
    bool sawPicked = false;
    while (art.PeekPacket(pickState)) {
        if (pickState.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            sawPicked = pickState.size == 13U && pickState.bytes[12] == 2U;
        }
        art.AcceptPacket();
    }
    Check(std::equal(original.begin(), original.end(), art.Artwork().begin(),
                     [](modules::Rgb a, modules::Rgb b) { return Color(a, b.r, b.g, b.b); }) &&
              art.UndoCount() == originalHistory && Color(art.Color(), 10U, 20U, 30U),
          "held color pick restores all four distinct pre-touch emitters without adding history");
    Check(sawPicked,
          "completed physical color pick is published to the connected app");
    Send(art, Art::Op::Inspect, request++);
    bool inspectRepublishedPick = false;
    bool inspectKeptLegacyState = false;
    while (art.PeekPacket(pickState)) {
        if (pickState.bytes[1] == uint8_t(Art::Event::State)) {
            inspectKeptLegacyState = pickState.bytes[13] <= 3U;
        } else if (pickState.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            inspectRepublishedPick = pickState.bytes[12] == 2U;
        }
        art.AcceptPacket();
    }
    Check(inspectKeptLegacyState && inspectRepublishedPick,
          "Inspect preserves legacy State parsing and republishes current color-pick status");
    Send(art, Art::Op::Snapshot, request++);
    bool snapshotEnded = false;
    bool snapshotRepublishedPick = false;
    while (art.PeekPacket(pickState)) {
        if (pickState.bytes[1] == uint8_t(Art::Event::SnapshotEnd)) {
            snapshotEnded = true;
        } else if (snapshotEnded &&
                   pickState.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            snapshotRepublishedPick = pickState.bytes[12] == 2U;
        }
        art.AcceptPacket();
    }
    Check(snapshotEnded && snapshotRepublishedPick,
          "Snapshot republishes current color-pick status after its session-binding end");
    art.ObserveOccupancy(0U);
    bool sawPickCleared = false;
    while (art.PeekPacket(pickState)) {
        if (pickState.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            sawPickCleared = pickState.size == 13U && pickState.bytes[12] == 0U;
        }
        art.AcceptPacket();
    }
    Check(sawPickCleared,
          "physical color-pick status clears when the held piece is lifted");
    Drain(art);
    Send(art, Art::Op::Clear, request++);
    Art::Packet packet;
    unsigned cells = 0U;
    while (art.PeekPacket(packet)) {
        Check(packet.bytes[1] != uint8_t(Art::Event::ResyncRequired), "Clear uses existing bounded Cell deltas");
        if (packet.bytes[1] == uint8_t(Art::Event::Cell)) {
            Check(packet.bytes[13] == 15U, "Clear groups all four erased emitters into one Cell");
            ++cells;
        }
        art.AcceptPacket();
    }
    Check(cells == 1U, "clearing a detailed single square costs one Cell");
    Send(art, Art::Op::Undo, request++);
    cells = 0U;
    while (art.PeekPacket(packet)) {
        Check(packet.bytes[1] != uint8_t(Art::Event::ResyncRequired), "detailed Undo uses compatible per-emitter Cell deltas");
        if (packet.bytes[1] == uint8_t(Art::Event::Cell)) {
            ++cells;
        }
        art.AcceptPacket();
    }
    Check(cells == 4U, "undoing a detailed Clear transmits exactly the four distinct restored colors");
}

void ColorPickLiftTimingAndSourceBoundaries() {
    for (const uint32_t held : {999U, 1000U, 1001U, 1500U, 1999U, 2000U}) {
        // Cross uint32 wrap while the magnet is held, without an Update at lift.
        fxe::ManualMillis::SetNowMs(UINT32_MAX - 1200U);
        Art art;
        art.Configure(3U, 956U);
        art.BeginConnection();
        Send(art, Art::Op::Enter, 1U);
        Send(art, Art::Op::Tool, 2U, {0U, 2U, 25U, 75U, 125U});
        Send(art, Art::Op::Paint, 3U, {1U, 0U, 3U, 9U});
        Send(art, Art::Op::Tool, 4U, {0U, 2U, 255U, 116U, 48U});
        const auto historyBefore = art.UndoCount();
        Drain(art);
        art.ObserveOccupancy(UINT64_C(1) << 9U);
        fxe::ManualMillis::AdvanceMs(held);
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        art.Compose(board, status);
        const auto shownAtLift = board[9U].top;
        if (held == 1500U) {
            Check(Color(shownAtLift, 140U, 96U, 86U),
                  "half-progress semantic RGB retains existing alpha rounding");
        }
        const bool coalescedDestination = held == 1500U;
        art.ObserveOccupancy(coalescedDestination ? UINT64_C(1) << 10U : 0U);
        const bool picking = held > Art::PICK_ARM_MS;
        Check(Color(art.Color(), shownAtLift.r, shownAtLift.g, shownAtLift.b) &&
                  !art.IsErasing() && art.Target() == 2U,
              "lift freezes the displayed color at dwell/blend/end boundaries across millis wrap");
        Check(Color(art.Artwork()[37U], picking ? 25U : 255U,
                    picking ? 75U : 116U, picking ? 125U : 48U) &&
                  art.UndoCount() == historyBefore + (picking ? 0U : 1U) +
                      (coalescedDestination ? 1U : 0U),
              "only a qualified lift restores pre-touch artwork without phantom history");
        art.ObserveOccupancy(UINT64_C(1) << 10U);
        Check(Color(art.Artwork()[41U], shownAtLift.r, shownAtLift.g, shownAtLift.b) &&
                  Color(art.Artwork()[40U], 0U, 0U, 0U),
              "next placement uses the frozen color and preserves the emitter mask");
    }

    fxe::ManualMillis::SetNowMs(100U);
    Art art;
    art.Configure(3U, 957U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    art.ObserveOccupancy(UINT64_C(1) << 9U);
    fxe::ManualMillis::AdvanceMs(1500U);
    Send(art, Art::Op::Tool, 2U, {0U, 4U, 31U, 63U, 95U});
    art.ObserveOccupancy(0U);
    Check(Color(art.Color(), 31U, 63U, 95U) && art.Target() == 4U,
          "an app tool change cancels the old pick before a later Hall lift");
}

void StaticDeltaHistoryRetainsTypicalWorkload() {
    Art art;
    art.Configure(3U, 955U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    const auto blank = art.Artwork();
    uint16_t request = 2U;
    for (uint8_t step = 0U; step < 50U; ++step) {
        Send(art, Art::Op::Tool, request++,
             {0U, 15U, static_cast<uint8_t>(step + 1U),
              static_cast<uint8_t>(100U + step),
              static_cast<uint8_t>(200U - step)});
        Send(art, Art::Op::Paint, request++,
             {static_cast<uint8_t>(step + 1U), 0U, 3U, step});
        Drain(art);
    }
    Check(art.UndoCount() >= 50U,
          "static delta history retains at least 50 typical one-square strokes");
    for (uint8_t step = 0U; step < 50U; ++step) {
        Send(art, Art::Op::Undo, request++);
        Drain(art);
    }
    Check(std::equal(blank.begin(), blank.end(), art.Artwork().begin(),
                     [](modules::Rgb a, modules::Rgb b) {
                         return Color(a, b.r, b.g, b.b);
                     }),
          "50 static Undo steps restore the exact pre-workload canvas");
}
void LiveStrokePublishesTransientHistoryCount() {
    Art art;
    art.Configure(3U, 1030U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Send(art, Art::Op::Tool, 2U, {0U, 1U, 112U, 22U, 33U});
    Drain(art);
    uint16_t request = 3U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        Send(art, Art::Op::Paint, request++, {uint8_t(square + 1U), 0U, 3U, square});
        Drain(art);
    }
    Check(art.UndoCount() == 64U, "64 completed real strokes fill the bounded static history");
    Send(art, Art::Op::Tool, request++, {0U, 1U, 22U, 33U, 44U});
    Drain(art);
    const auto open = request++;
    Send(art, Art::Op::Paint, open, {65U, 0U, 1U, 0U});
    const auto capture = [&](Art::Event event, uint16_t id, uint8_t history, const char* label) {
        Art::Packet packet;
        bool found = false;
        while (art.PeekPacket(packet)) {
            if (packet.size == 20U && packet.bytes[1U] == uint8_t(event) &&
                (uint16_t(packet.bytes[10U]) | uint16_t(packet.bytes[11U]) << 8U) == id) {
                found = packet.bytes[12U] == uint8_t(Art::Status::Ok) &&
                    packet.bytes[15U] == history && packet.bytes[16U] == 0U;
                std::printf("[NATIVE_HISTORY_%s]", label);
                for (uint8_t byte = 0U; byte < packet.size; ++byte) std::printf("%02x", packet.bytes[byte]);
                std::puts("");
            }
            art.AcceptPacket();
        }
        return found;
    };
    Check(art.UndoCount() == 65U && capture(Art::Event::State, open, 65U, "STATE_65"),
          "open 65th real stroke emits compact State with transient Undo count 65");
    const auto snapshot = request++;
    Send(art, Art::Op::Snapshot, snapshot);
    Check(capture(Art::Event::SnapshotBegin, snapshot, 65U, "SNAPSHOT_65"),
          "snapshot authority preserves the same valid live-stroke history count");
    const auto finish = request++;
    Send(art, Art::Op::Paint, finish, {65U, 0U, 2U});
    Check(art.UndoCount() == 64U && capture(Art::Event::State, finish, 64U, "SETTLED_64"),
          "ending the real stroke commits within the 64-operation history bound");
}
void UnitJourney() {
    Art occupied;
    occupied.Configure(3U, 122U);
    occupied.BeginConnection();
    const auto occupiedEnter = Request(Art::Op::Enter, 0U, 1U);
    occupied.HandlePacket(occupiedEnter.bytes.data(), occupiedEnter.size, true,
                          UINT64_C(1) << 4U);
    Art::Packet occupiedReply;
    Check(!occupied.IsActive() && occupied.PeekPacket(occupiedReply) &&
              occupiedReply.bytes[12] == uint8_t(Art::Status::BoardNotEmpty),
          "entry with any resting piece is rejected with a distinct additive status");

    Art art;
    art.Configure(3U, 123U);
    art.BeginConnection();
    auto enter = Request(Art::Op::Enter, 0U, 1U);
    art.HandlePacket(enter.bytes.data(), enter.size, true, 0U);
    Check(art.IsActive() && !art.IsWaitingForLift(),
          "entry starts only from a freshly observed empty board");
    Check(Color(art.Artwork()[16], 0, 0, 0), "baseline is not stored artwork");
    const uint32_t revision = art.Revision();
    art.ObserveOccupancy(UINT64_C(1) << 4U);
    Check(art.Revision() == revision + 1U,
          "the first piece after empty admission paints immediately");
    Drain(art);
    art.ObserveButton({modules::UserButton::Press,
                       fxe::Button::Event::Press, modules::UiOwner::P1});
    Art::Packet cancelledPick;
    bool sawCancelledPick = false;
    while (art.PeekPacket(cancelledPick)) {
        if (cancelledPick.bytes[1] == uint8_t(Art::Event::ColorPick)) {
            sawCancelledPick = cancelledPick.bytes[12] == 0U;
        }
        art.AcceptPacket();
    }
    Check(!sawCancelledPick,
          "unused center cannot cancel an in-progress color pick");
    art.ObserveButton({modules::UserButton::Press,
                       fxe::Button::Event::Release, modules::UiOwner::P1});
    art.ObserveOccupancy(0U);
    art.ObserveOccupancy((UINT64_C(1) << 4U) | (UINT64_C(1) << 5U));
    Check(Color(art.Artwork()[16], 255, 116, 48) && Color(art.Artwork()[20], 255, 116, 48), "multi-sensor footprint deterministically paints every touched square");
    Send(art, Art::Op::Tool, 2, {0, 15, 20, 30, 40});
    Check(Color(art.Artwork()[16], 20, 30, 40) && Color(art.Artwork()[20], 20, 30, 40), "color change recolors a resting active brush once");
    Send(art, Art::Op::Paint, 3, {1, 0, 3, 4});
    Send(art, Art::Op::Tool, 4, {1, 15, 20, 30, 40});
    Check(Color(art.Artwork()[16], 0, 0, 0), "eraser changes active brush footprint to explicit off");
    art.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(349U);
    art.Update();
    art.ObserveOccupancy(UINT64_C(1) << 6U);
    art.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(700U);
    art.Update();
    const auto before = art.UndoCount();
    Send(art, Art::Op::Undo, 5);
    Check(art.UndoCount() + 1U == before, "physical release grouping is one undo operation");
    Send(art, Art::Op::Redo, 6);
    Check(art.UndoCount() == before, "physical redo restores same shared history");
    const auto duplicate = Request(Art::Op::Clear, art.SessionId(), 7U);
    art.HandlePacket(duplicate.bytes.data(), duplicate.size, true, 0U);
    const auto clearRevision = art.Revision();
    art.HandlePacket(duplicate.bytes.data(), duplicate.size, true, 0U);
    Check(art.Revision() == clearRevision, "duplicate Clear exactly once");
    auto stale = Request(Art::Op::Paint, art.SessionId() + 1U, 8U, {2, 0, 3, 12});
    art.HandlePacket(stale.bytes.data(), stale.size, true, 0U);
    Check(art.Revision() == clearRevision, "stale session cannot paint");
    Send(art, Art::Op::Paint, 8, {2, 0, 3, 64});
    Check(art.Revision() == clearRevision, "invalid square rejects whole packet");
    Send(art, Art::Op::Tool, 9, {0, 15, 7, 11, 19});
    Send(art, Art::Op::Paint, 10, {3, 0, 1, 0, 7, 56, 63});
    Send(art, Art::Op::Paint, 11, {3, 0, 2, 9});
    const auto history = art.UndoCount();
    Send(art, Art::Op::Undo, 12);
    Check(Color(art.Artwork()[0], 0, 0, 0) && art.UndoCount() + 1 == history, "many app packets form one stroke");
    Send(art, Art::Op::Redo, 13);
    Drain(art);
    Send(art, Art::Op::Snapshot, 14);
    const auto frozen = art.Revision();
    Send(art, Art::Op::Paint, 15, {4, 0, 3, 1});
    Art::Packet packet;
    unsigned chunks = 0;
    bool ended = false;
    uint32_t endRevision = 0;
    while (art.PeekPacket(packet)) {
        if (packet.bytes[1] == uint8_t(Art::Event::SnapshotChunk)) {
            ++chunks;
        }
        if (packet.bytes[1] == uint8_t(Art::Event::SnapshotEnd)) {
            ended = true;
            for (int i = 0; i < 4; ++i) {
                endRevision |= uint32_t(packet.bytes[6 + i]) << (i * 8);
            }
        }
        art.AcceptPacket();
    }
    Check(ended && chunks == 128U && endRevision == frozen, "snapshot is coherent while newer app mutations queue behind it");
    Send(art, Art::Op::ReplaceBegin, 16);
    Send(art, Art::Op::ReplaceChunk, 17, {0, 0, 1, 2, 3});
    Send(art, Art::Op::ReplaceCommit, 18);
    Check(Color(art.Artwork()[0], 7, 11, 19), "incomplete replacement never shows partial image");
    art.EndConnection();
    art.ObserveOccupancy(UINT64_C(1) << 2U);
    Check(Color(art.Artwork()[8], 7, 11, 19), "disconnected magnetic painting remains authoritative");
    art.BeginConnection();
    Send(art, Art::Op::Tool, 1, {0, 15, 100, 90, 80});
    Check(Color(art.Artwork()[8], 100, 90, 80), "reconnected generation may restart request IDs");
    Send(art, Art::Op::Exit, 2);
    Check(!art.IsActive(), "explicit exit releases board mode");
    const auto oldSession = art.SessionId();
    Send(art, Art::Op::Enter, 3);
    Check(art.SessionId() != oldSession && Color(art.Artwork()[8], 100, 90, 80), "new mode session invalidates old packets and retains art");
}

void PhysicalModeTransitionNotifications() {
    Art art;
    art.Configure(3U, 124U);
    art.BeginConnection();
    art.ObserveEntryButton({modules::UserButton::P1X,
                            fxe::Button::Event::Press, modules::UiOwner::P1}, 0U, true);
    art.ObserveEntryButton({modules::UserButton::P1O,
                            fxe::Button::Event::Press, modules::UiOwner::P1}, 30U, true);
    art.ObserveEntryButton({modules::UserButton::P1X,
                            fxe::Button::Event::Release, modules::UiOwner::P1}, 1500U, true);

    Art::Packet packet;
    Check(art.PeekPacket(packet) &&
              packet.bytes[1] == uint8_t(Art::Event::State) &&
              packet.bytes[10] == 0U && packet.bytes[11] == 0U &&
              (packet.bytes[13] & 1U) != 0U,
          "physical entry publishes unsolicited active State to the connected app");
    art.AcceptPacket();
    Check(art.PeekPacket(packet) &&
              packet.bytes[1] == uint8_t(Art::Event::ResyncRequired) &&
              packet.bytes[10] == 0U && packet.bytes[11] == 0U,
          "physical entry follows State with the existing canvas resync cue");
    art.AcceptPacket();

    art.ObserveButton({modules::UserButton::P1X,
                       fxe::Button::Event::Press, modules::UiOwner::P1}, 2000U);
    art.ObserveButton({modules::UserButton::P1O,
                       fxe::Button::Event::Press, modules::UiOwner::P1}, 2020U);
    Check(art.IsActive() && art.IsExitHoldActive(),
          "physical exit chord starts confirmation without exiting");
    art.ObserveButton({modules::UserButton::P1X,
                       fxe::Button::Event::Release, modules::UiOwner::P1}, 3500U);
    Check(art.PeekPacket(packet) &&
              packet.bytes[1] == uint8_t(Art::Event::State) &&
              packet.bytes[10] == 0U && packet.bytes[11] == 0U &&
              (packet.bytes[13] & 1U) == 0U,
          "completed physical exit publishes inactive State only on release");
}

void EntryChordUsesCapturedEventTime() {
    Art art;
    art.Configure(3U, 125U);
    fxe::ManualMillis::SetNowMs(1000U);
    Check(art.ObserveEntryButton(
              {modules::UserButton::P1X, fxe::Button::Event::Press,
               modules::UiOwner::P1},
              100U, true) == Art::ModeChordResult::Pending,
          "first batched edge starts entry arbitration at capture time");
    Check(art.ObserveEntryButton(
              {modules::UserButton::P1O, fxe::Button::Event::Press,
               modules::UiOwner::P1},
              100U + Art::MODE_CHORD_PRIORITY_MS + 1U, true) ==
                  Art::ModeChordResult::Replay &&
              !art.IsActive(),
          "batched edges beyond the priority window cannot form a false processing-time chord");
    modules::ButtonEvent replay;
    uint32_t replayTimeMs = 0U;
    Check(art.TakeEntryButtonForReplay(replay, replayTimeMs) &&
              replay.button == modules::UserButton::P1X &&
              replayTimeMs == 100U,
          "replay preserves the first edge capture timestamp");

    fxe::ManualMillis::SetNowMs(2000U);
    Check(art.ObserveEntryButton(
              {modules::UserButton::P1X, fxe::Button::Event::Press,
               modules::UiOwner::P1},
              500U, true) == Art::ModeChordResult::Pending &&
              art.ObserveEntryButton(
                  {modules::UserButton::P1O, fxe::Button::Event::Press,
                   modules::UiOwner::P1},
                  500U + Art::MODE_CHORD_PRIORITY_MS, true) ==
                      Art::ModeChordResult::Pending &&
              art.ObserveEntryButton(
                  {modules::UserButton::P1X, fxe::Button::Event::Release,
                   modules::UiOwner::P1},
                  2000U, true) == Art::ModeChordResult::Entered &&
              art.IsActive(),
          "exact priority-window captured edges enter only on release after the full hold");
}

void ActiveModeChordUsesCapturedEventTime() {
    Art late;
    late.Configure(3U, 126U);
    Send(late, Art::Op::Enter, 1U);
    Drain(late);
    fxe::ManualMillis::SetNowMs(3000U);
    late.ObserveButton(
        {modules::UserButton::P1X, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        100U);
    late.ObserveButton(
        {modules::UserButton::P1O, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        100U + Art::MODE_CHORD_PRIORITY_MS + 1U);
    Check(late.IsActive(),
          "active batched edges beyond the priority window cannot form a false exit chord");

    Art boundary;
    boundary.Configure(3U, 127U);
    Send(boundary, Art::Op::Enter, 1U);
    Drain(boundary);
    fxe::ManualMillis::SetNowMs(4000U);
    boundary.ObserveButton(
        {modules::UserButton::P1X, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        500U);
    boundary.ObserveButton(
        {modules::UserButton::P1O, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        500U + Art::MODE_CHORD_PRIORITY_MS);
    Check(boundary.IsActive() && boundary.IsExitHoldActive(),
          "active exact-priority-window captured edges form an exit hold without exiting");
    boundary.ObserveButton(
        {modules::UserButton::P1X, fxe::Button::Event::Release,
         modules::UiOwner::P1},
        2000U);
    Check(!boundary.IsActive(),
          "active captured-time exit commits on release after the full hold");
}

void PhysicalQuickPaletteJourney() {
    fxe::ManualMillis::SetNowMs(20000U);
    Art art;
    art.Configure(3U, 125U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    const auto originalArtwork = art.Artwork();
    const auto originalFavorites = art.RecentColors();

    art.ObserveButton({modules::UserButton::Left,
                       fxe::Button::Event::Press, modules::UiOwner::P1});
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    art.Compose(board, status);
    Check(art.IsQuickPaletteVisible() &&
              Color(art.Color(), 255U, 26U, 0U),
          "Left opens the physical palette and selects the prior color in the 32-step wheel");
    Check(Color(board[59].left, 255U, 26U, 0U) &&
              !Color(board[56].left, 0U, 0U, 0U),
          "the selected wheel color pulses inside an eight-swatch moving window");
    Check(std::equal(originalFavorites.begin(), originalFavorites.end(),
                     art.RecentColors().begin(),
                     [](modules::Rgb a, modules::Rgb b) {
                         return Color(a, b.r, b.g, b.b);
                     }),
          "opening and stepping the wheel preserve the saved and previous colors");
    Check(std::equal(originalArtwork.begin(), originalArtwork.end(), art.Artwork().begin(),
                     [](modules::Rgb a, modules::Rgb b) {
                         return Color(a, b.r, b.g, b.b);
                     }),
          "opening the palette never paints its temporary row into the artwork");

    art.ObserveOccupancy(UINT64_C(1) << 59U);
    const auto draggedColor = art.Color();
    Check(Color(draggedColor, 255U, 26U, 0U),
          "placing the brush on a visible swatch selects it immediately");
    art.ObserveOccupancy(UINT64_C(1) << 60U);
    Check(Color(art.Color(), 255U, 59U, 0U) &&
              std::equal(originalArtwork.begin(), originalArtwork.end(), art.Artwork().begin(),
                         [](modules::Rgb a, modules::Rgb b) {
                             return Color(a, b.r, b.g, b.b);
                         }),
          "dragging across the palette previews each color without drawing");

    const auto beforeFineAdjust = art.Color();
    art.ObserveButton({modules::UserButton::Up,
                       fxe::Button::Event::Press, modules::UiOwner::P1});
    Check(art.IsQuickPaletteVisible() &&
              !Color(art.Color(), beforeFineAdjust.r, beforeFineAdjust.g,
                     beforeFineAdjust.b) &&
              std::equal(originalFavorites.begin(), originalFavorites.end(),
                         art.RecentColors().begin(),
                         [](modules::Rgb a, modules::Rgb b) {
                             return Color(a, b.r, b.g, b.b);
                         }),
          "Up fine-adjusts only the active color while the wheel and favorites stay intact");

    art.ObserveOccupancy(0U);
    Check(!art.IsQuickPaletteVisible() &&
              !Color(art.Color(), art.RecentColors()[0].r,
                     art.RecentColors()[0].g, art.RecentColors()[0].b),
          "lifting the magnet immediately commits the sampled swatch as the active color");
    constexpr uint8_t paintedSquare = 20U;
    art.ObserveOccupancy(UINT64_C(1) << paintedSquare);
    Check(Color(art.Artwork()[paintedSquare * 4U], art.Color().r,
                art.Color().g, art.Color().b),
          "the next square paints with the sampled swatch without waiting for palette timeout");
    art.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(fxe::Millis() + Art::QUICK_PALETTE_FADE_MS);
    art.Compose(board, status);
    Check(!art.IsQuickPaletteVisible() && Color(board[60].left, 0U, 0U, 0U),
          "lifting restores the underlying artwork instead of retaining a modal palette");

    art.ObserveOccupancy(0U);
    art.ObserveButton({modules::UserButton::Right,
                       fxe::Button::Event::Press, modules::UiOwner::P2});
    art.Compose(board, status);
    Check(art.IsQuickPaletteVisible() &&
              (!Color(board[0].left, 0U, 0U, 0U) ||
               !Color(board[7].left, 0U, 0U, 0U)),
          "the same quick palette appears beside the P2 control bank");
    const auto beforeFullTurn = art.Color();
    for (uint8_t step = 0U; step < 32U; ++step) {
        art.ObserveButton({modules::UserButton::Right,
                           fxe::Button::Event::Press, modules::UiOwner::P2});
    }
    Check(Color(art.Color(), beforeFullTurn.r, beforeFullTurn.g,
                beforeFullTurn.b) &&
              std::equal(originalFavorites.begin(), originalFavorites.end(),
                         art.RecentColors().begin(),
                         [](modules::Rgb a, modules::Rgb b) {
                             return Color(a, b.r, b.g, b.b);
                         }),
          "32 Right steps wrap continuously to the same active color without changing favorites");
}

void FavoritePaletteTransportJourney() {
    Art art;
    art.Configure(3U, 126U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    const auto original = art.RecentColors();

    Send(art, Art::Op::PaletteChunk, 2U,
         {0U, 3U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U});
    Check(std::equal(original.begin(), original.end(), art.RecentColors().begin(),
                     [](modules::Rgb a, modules::Rgb b) {
                         return Color(a, b.r, b.g, b.b);
                     }),
          "an incomplete app palette remains invisible to physical controls");
    Send(art, Art::Op::PaletteChunk, 3U,
         {6U, 2U, 19U, 20U, 21U, 22U, 23U, 24U});
    Check(HasStatus(art, 3U, Art::Status::Incomplete),
          "an out-of-order palette chunk fails without partial authority");

    Send(art, Art::Op::PaletteChunk, 4U,
         {0U, 3U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U});
    Send(art, Art::Op::PaletteChunk, 5U,
         {3U, 3U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U});
    Send(art, Art::Op::PaletteChunk, 6U,
         {6U, 2U, 19U, 20U, 21U, 22U, 23U, 24U});
    Check(Color(art.RecentColors()[0], 1U, 2U, 3U) &&
              Color(art.RecentColors()[7], 22U, 23U, 24U) &&
              art.PaletteGeneration() == 1U,
          "three minimum-MTU-safe chunks atomically replace all eight board favorites");

    Send(art, Art::Op::PaletteChunk, 7U,
         {0U, 3U, 31U, 32U, 33U, 34U, 35U, 36U, 37U, 38U, 39U});
    art.EndConnection();
    art.BeginConnection();
    Send(art, Art::Op::PaletteChunk, 1U,
         {3U, 3U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U});
    Check(HasStatus(art, 1U, Art::Status::Incomplete) &&
              Color(art.RecentColors()[0], 1U, 2U, 3U),
          "reconnect retires an interrupted palette transaction without changing favorites");
}

void ExactDocumentColorJourney() {
    Check(modules::litchcraft_physical_presentation::CANVAS_SCALE256 == 128U,
          "Litchcraft canvas uses the owner-evaluation anti-bloom scale");
    Art art;
    art.Configure(3U, 456U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);

    Send(art, Art::Op::Tool, 2U, {0U, 15U, 255U, 52U, 82U});
    Send(art, Art::Op::Paint, 3U, {1U, 0U, 3U, 0U});
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    art.Compose(board, status);
    Check(Color(art.Artwork()[0], 255U, 52U, 82U),
          "stored red artwork keeps exact document RGB");
    Check(Color(board[0].left, 255U, 52U, 82U),
          "semantic red presentation keeps exact document RGB");

    Send(art, Art::Op::Tool, 4U, {0U, 15U, 174U, 83U, 255U});
    Send(art, Art::Op::Paint, 5U, {2U, 0U, 3U, 1U});
    art.Compose(board, status);
    Check(Color(art.Artwork()[4], 174U, 83U, 255U),
          "stored violet artwork keeps exact document RGB");
    Check(Color(board[1].left, 174U, 83U, 255U),
          "semantic violet presentation keeps exact document RGB");
    const std::array<modules::Rgb, 11U> inputs{{
        {255U, 116U, 48U}, {255U, 52U, 82U}, {255U, 207U, 45U},
        {55U, 215U, 105U}, {34U, 195U, 226U}, {63U, 106U, 255U},
        {174U, 83U, 255U}, {255U, 255U, 255U}, {40U, 40U, 40U},
        {255U, 0U, 0U}, {0U, 0U, 0U}}};
    for (uint8_t i = 0U; i < inputs.size(); ++i) {
        const auto input = inputs[i];
        Send(art, Art::Op::Tool, 6U + i * 2U, {0U, 15U, input.r, input.g, input.b});
        Send(art, Art::Op::Paint, 7U + i * 2U, {uint8_t(i + 3U), 0U, 3U, i});
        art.Compose(board, status);
        Check(Color(board[i].left, input.r, input.g, input.b),
              "all semantic palette and neutral vectors keep exact document RGB");
        Check(Color(art.Artwork()[i * 4U], input.r, input.g, input.b),
              "palette document RGB remains unchanged");
    }
}
void AdversarialProtocol() {
    Art art;
    art.Configure(2U, 876U);
    art.BeginConnection();
    const auto query = Request(Art::Op::Snapshot, 0U, 1U);
    art.HandlePacket(query.bytes.data(), query.size, false, 0U);
    Check(!art.IsActive(), "session-zero snapshot inspects without taking mode ownership");
    Drain(art);
    Art observer;
    observer.Configure(3U, 877U);
    observer.BeginConnection();
    const auto inspect = Request(Art::Op::Inspect, 0U, 1U);
    observer.HandlePacket(inspect.bytes.data(), inspect.size, false, 0U);
    Art::Packet inspected;
    Check(observer.PeekPacket(inspected) &&
              inspected.bytes[1] == uint8_t(Art::Event::State) &&
              inspected.bytes[10] == 1U && inspected.bytes[11] == 0U,
          "Inspect returns one state packet without starting an artwork snapshot");
    observer.AcceptPacket();
    Check(observer.PeekPacket(inspected) &&
              inspected.bytes[1] == uint8_t(Art::Event::ColorPick) &&
              inspected.bytes[12] == 0U && !observer.IsActive(),
          "Inspect republishes idle gesture state without streaming canvas or acquiring ownership");
    observer.AcceptPacket();
    Check(!observer.PeekPacket(inspected),
          "Inspect publishes only State plus the bounded gesture phase");

    Art bulkArt;
    bulkArt.Configure(3U, 878U);
    bulkArt.BeginConnection();
    Send(bulkArt, Art::Op::Enter, 1U);
    Drain(bulkArt);
    const auto bulkSnapshot = Request(
        Art::Op::Snapshot, bulkArt.SessionId(), 2U, {1U});
    bulkArt.HandlePacket(
        bulkSnapshot.bytes.data(), bulkSnapshot.size, true, 0U);
    Art::BulkPacket bulkPacket;
    unsigned bulkPackets = 0U;
    bool bulkEnded = false;
    bool bulkGesture = false;
    while (bulkArt.PeekPacket(bulkPacket, 244U)) {
        ++bulkPackets;
        Check(bulkPacket.size <= 244U,
              "bulk Snapshot respects negotiated ATT payload");
        bulkEnded = bulkEnded ||
                    bulkPacket.bytes[1] == uint8_t(Art::Event::SnapshotEnd);
        bulkGesture = bulkGesture ||
                      bulkPacket.bytes[1] == uint8_t(Art::Event::ColorPick);
        bulkArt.AcceptPacket();
    }
    Check(bulkEnded && bulkGesture && bulkPackets <= 9U,
          "negotiated bulk Snapshot carries a complete canvas in eight notifications plus gesture state");
    Send(art, Art::Op::Enter, 2);
    Drain(art);
    Send(art, Art::Op::ReplaceBegin, 3);
    const auto before = art.Revision();
    uint16_t request = 4U;
    for (uint16_t offset = 0; offset < 256U; offset += 3U) {
        auto packet = Request(Art::Op::ReplaceChunk, art.SessionId(), request++);
        packet.bytes[8] = offset;
        packet.bytes[9] = offset >> 8U;
        packet.size = 10U;
        for (uint16_t i = offset; i < offset + 3U && i < 256U; ++i) {
            packet.bytes[packet.size++] = uint8_t(i);
            packet.bytes[packet.size++] = uint8_t(255U - i);
            packet.bytes[packet.size++] = uint8_t(i % 13U);
        }
        art.HandlePacket(packet.bytes.data(), packet.size, true, 0U);
        Check(art.Revision() == before && Color(art.Artwork()[0], 0, 0, 0), "all restore chunks remain invisible until complete commit");
        Drain(art);
    }
    Send(art, Art::Op::ReplaceCommit, request++);
    Check(art.Revision() == before + 1U && Color(art.Artwork()[255], 255, 0, 8), "complete replacement becomes one coherent revision");
    const auto undo = art.UndoCount();
    Send(art, Art::Op::Undo, request++);
    Check(art.UndoCount() + 1U == undo && Color(art.Artwork()[255], 0, 0, 0), "complete replacement is exactly one undo operation");
    Send(art, Art::Op::Redo, request++);
    Drain(art);
    const auto snapshot = Request(Art::Op::Snapshot, 0U, request++);
    art.HandlePacket(snapshot.bytes.data(), snapshot.size, true, 0U);
    Art::Packet held, again;
    Check(art.PeekPacket(held) && art.PeekPacket(again) &&
              held.bytes == again.bytes,
          "congested transport retains unsent head exactly");
    art.AcceptPacket();
    Drain(art);
    const auto stale = Request(Art::Op::Clear, art.SessionId(), 3U);
    const auto retained = art.Revision();
    art.HandlePacket(stale.bytes.data(), stale.size, true, 0U);
    Check(art.Revision() == retained, "delayed request beyond receipt cache cannot replay old mutation");
    Send(art, Art::Op::ReplaceBegin, request++);
    Send(art, Art::Op::ReplaceChunk, request++, {0, 0, 9, 8, 7});
    fxe::ManualMillis::SetNowMs(12000U);
    art.Update();
    Send(art, Art::Op::ReplaceCommit, request++);
    Check(art.Revision() == retained, "timed-out incomplete replacement preserves last coherent art");
    Drain(art);
    for (uint16_t i = 0; i < 110U; ++i) {
        Send(art, Art::Op::Tool, request++, {0, 15, uint8_t(i), 3, 5});
    }
    Art::Packet overflow;
    unsigned retainedReceipts = 0U;
    while (art.PeekPacket(overflow) && overflow.bytes[1U] == uint8_t(Art::Event::State)) {
        Check(overflow.size == 20U &&
                  (uint16_t(overflow.bytes[10U]) | uint16_t(overflow.bytes[11U]) << 8U) != 0U &&
                  overflow.bytes[12U] == uint8_t(Art::Status::Ok),
              "overflow preserves only correlated compact control receipts ahead of repair");
        ++retainedReceipts;
        art.AcceptPacket();
    }
    Check(retainedReceipts != 0U, "post-overflow controls keep their bounded authoritative receipts");
    Check(art.PeekPacket(overflow) &&
              overflow.bytes[1] == uint8_t(Art::Event::ResyncRequired),
          "bounded outbound overflow explicitly invalidates stream");
    art.AcceptPacket();
    Check(!art.PeekPacket(overflow), "overflow stops incremental publication until resync");
    Send(art, Art::Op::Inspect, request++);
    Check(art.PeekPacket(overflow) &&
              overflow.bytes[1] == uint8_t(Art::Event::State),
          "inspection establishes a fresh notification baseline after overflow");
    Drain(art);
    Send(art, Art::Op::Snapshot, request++);
    Drain(art);
    art.ResynchronizeOccupancy(UINT64_C(1) << 19U);
    const auto healthy = art.Revision();
    Send(art, Art::Op::Tool, request++, {0, 15, 1, 2, 3});
    Check(art.IsWaitingForLift() && Color(art.Artwork()[76], 76, 179, 11), "fresh recovery snapshot baselines uncertain resting brush without stamping");
    art.ObserveOccupancy(0U);
    art.ObserveOccupancy(UINT64_C(1) << 19U);
    Check(art.Revision() > healthy && Color(art.Artwork()[76], 1, 2, 3), "lift/reapply restores magnetic painting after health resync");
    const auto strokeCount = art.UndoCount();
    art.ObserveOccupancy(UINT64_C(1) << 19U);
    Check(art.UndoCount() == strokeCount, "stable recovered brush stays one stroke");
    const auto beforeEraserRevision = art.Revision();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1});
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Repeat, modules::UiOwner::P1});
    Check(!art.IsErasing() && art.Revision() == beforeEraserRevision,
          "O press and held repeat wait for the established hold threshold before toggling Erase");
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 1000U);
    art.Update();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release, modules::UiOwner::P1});
    const auto eraserRevision = art.Revision();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Repeat, modules::UiOwner::P1});
    Check(art.IsErasing() && art.Revision() == eraserRevision,
          "long O toggles Erase once; release and stale repeat cannot restamp it");
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1});
    const auto undoRevision = art.Revision();
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Repeat, modules::UiOwner::P1});
    Check(art.Revision() == undoRevision, "held X repeat cannot consume multiple undo operations");
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1});
    // Cross-owner face buttons cannot form the mode chord.
    fxe::ManualMillis::SetNowMs(13000U);
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1});
    art.ObserveButton({modules::UserButton::P2O, fxe::Button::Event::Press, modules::UiOwner::P2});
    Check(art.IsActive(), "cross-owner X+O cannot exit Litchcraft");
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1});
    art.ObserveButton({modules::UserButton::P2O, fxe::Button::Event::Release, modules::UiOwner::P2});
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Press,
                       modules::UiOwner::P1}, 14000U);
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                       modules::UiOwner::P1}, 14020U);
    Check(art.IsActive() && art.IsExitHoldActive(),
          "same-owner X+O starts exit confirmation inside the priority window");
    art.ObserveButton({modules::UserButton::P1X, fxe::Button::Event::Release,
                       modules::UiOwner::P1}, 15500U);
    Check(!art.IsActive(),
          "same-owner X+O exits only on release after the complete hold");
}

void BulkArtworkProtocol() {
    Art art;
    art.Configure(3U, 912U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    const uint32_t before = art.Revision();
    constexpr uint16_t request = 2U;
    uint16_t offset = 0U;
    unsigned frames = 0U;
    while (offset < Art::ELEMENT_COUNT) {
        const uint8_t count = static_cast<uint8_t>(
            std::min<size_t>(77U, Art::ELEMENT_COUNT - offset));
        std::vector<uint8_t> frame(12U + size_t(count) * 3U);
        frame[0] = Art::MAGIC;
        frame[1] = Art::BULK_REPLACE;
        const uint32_t session = art.SessionId();
        for (int i = 0; i < 4; ++i) frame[2U + i] = session >> (i * 8);
        frame[6] = request;
        frame[7] = request >> 8U;
        frame[8] = offset;
        frame[9] = offset >> 8U;
        frame[10] = offset + count == Art::ELEMENT_COUNT ? 1U : 0U;
        frame[11] = count;
        for (uint16_t i = 0U; i < count; ++i) {
            frame[12U + i * 3U] = static_cast<uint8_t>(offset + i);
            frame[13U + i * 3U] = static_cast<uint8_t>(255U - offset - i);
            frame[14U + i * 3U] = static_cast<uint8_t>((offset + i) % 13U);
        }
        art.HandleBulkPacket(frame.data(), frame.size(), true, 0U);
        ++frames;
        offset = static_cast<uint16_t>(offset + count);
        if (offset < Art::ELEMENT_COUNT) {
            Check(art.Revision() == before &&
                      Color(art.Artwork()[0], 0, 0, 0),
                  "high-MTU artwork frames remain invisible until final frame");
        }
    }
    Check(frames == 4U && art.Revision() == before + 1U &&
              Color(art.Artwork()[255], 255, 0, 8),
          "four high-MTU frames atomically replace the complete canvas");
    const auto revision = art.Revision();
    // A retry can begin with the same committed request after an Ack timeout.
    std::vector<uint8_t> retry(15U, 0U);
    retry[0] = Art::MAGIC;
    retry[1] = Art::BULK_REPLACE;
    const uint32_t session = art.SessionId();
    for (int i = 0; i < 4; ++i) retry[2U + i] = session >> (i * 8);
    retry[6] = request;
    retry[8] = 0U;
    retry[10] = 0U;
    retry[11] = 1U;
    art.HandleBulkPacket(retry.data(), retry.size(), true, 0U);
    Check(art.Revision() == revision,
          "bulk retry after a lost Ack cannot apply artwork twice");
}
void WireFixtureJourney() {
    Art art;
    art.Configure(3U, 0x12340000U);
    art.BeginConnection();
    const bool capture = std::getenv("LITCHCRAFT_WIRE_CAPTURE") != nullptr;
    auto drain = [&]() {
        Art::Packet packet;
        while (art.PeekPacket(packet)) {
            if (capture) {
                std::printf("[LITCHCRAFT_WIRE] ");
                for (uint8_t i = 0U; i < packet.size; ++i) {
                    std::printf("%02x", packet.bytes[i]);
                }
                std::printf("\n");
            }
            art.AcceptPacket();
        }
    };
    Send(art, Art::Op::Enter, 1U);
    drain();
    Send(art, Art::Op::Tool, 2U, {0, 15, 0x12, 0x34, 0x56});
    drain();
    Send(art, Art::Op::Paint, 3U, {1, 0, 3, 0});
    drain();
    Send(art, Art::Op::Paint, 4U, {2, 0, 3, 63});
    drain();
    Send(art, Art::Op::Snapshot, 5U);
    drain();
    Check(Color(art.Artwork()[0], 0x12, 0x34, 0x56) &&
              Color(art.Artwork()[255], 0x12, 0x34, 0x56),
          "cross-language wire fixture retains two asymmetric corner strokes");
}
void LocalEntryOwnsConcurrentAppAdmissionJourney() {
    for (const bool cancel : {true, false}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession session(config, engine);
        session.ObserveBootHallMask(0U);
        session.Advance(3000U);
        auto& art = session.Litchcraft();
        art.BeginConnection();
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::X, true);
        session.Advance(30U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::O, true);
        session.Advance(30U);
        session.Advance(cancel ? 300U : Art::ENTRY_HOLD_MS);
        Check(art.IsEntryHoldActive() && !art.IsActive(),
              "local entry remains pending until release even after full rainbow");
        const auto pendingFrame = session.SemanticFrame();
        const auto enter = Request(Art::Op::Enter, 0U, 1U);
        session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
        Check(!art.IsActive() && art.IsEntryHoldActive() &&
                  HasStatus(art, 1U, Art::Status::EntryPending) &&
                  session.SemanticFrame().status_hash == pendingFrame.status_hash,
              "concurrent app Enter cannot acquire or replace local hold presentation");
        const auto inspect = Request(Art::Op::Inspect, 0U, 2U);
        session.SendLitchcraftPacket(inspect.bytes.data(), inspect.size);
        Check(HasStatus(art, 2U, Art::Status::Ok) && !art.IsActive() &&
                  session.SemanticFrame().status_hash == pendingFrame.status_hash,
              "app inspection observes pending local entry without changing its rainbow");
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::X, false);
        session.Advance(30U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::O, false);
        session.Advance(30U);
        Check(art.IsActive() == !cancel && !art.IsEntryHoldActive(),
              "only local release commits or cancels its mode choice");
        const auto generation = art.ModeGeneration();
        const auto revision = art.Revision();
        const auto after = Request(Art::Op::Enter, 0U, 3U);
        session.SendLitchcraftPacket(after.bytes.data(), after.size);
        Check(art.IsActive() && HasStatus(art, 3U, Art::Status::Ok) &&
                  (cancel || (art.ModeGeneration() == generation &&
                              art.Revision() == revision)),
              "post-release app admission is idempotent for the board-owned session");
        std::printf("[LOCAL_ENTRY_APP_RACE_JOURNEY] input=raw-X+O hold=%s "
                    "app=Enter/Inspect result=EntryPending/Ok mode=pending "
                    "status=rainbow-uninterrupted release=%s "
                    "next=app-Enter result=Ok/idempotent\n",
                    cancel ? "short" : "complete", cancel ? "cancel" : "commit");
    }
}

void AppOccupiedEntryFeedbackJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    constexpr uint64_t PIECE = UINT64_C(1) << 52U;
    session.ObserveBootHallMask(PIECE);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(!art.IsActive() && session.EdgeCueIntent().kind ==
              modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "app occupied Enter keeps Setup and shows the board's amber warning");
    const auto cueStart = session.EdgeCueIntent().startMs;
    const auto firstFrame = session.SemanticFrame();
    session.Advance(250U);
    Check(session.SemanticFrame().board_hash != firstFrame.board_hash,
          "app occupied Enter animates the blocking piece");
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(session.EdgeCueIntent().startMs == cueStart,
          "replayed app Enter receipt cannot restart occupied feedback");
    session.Advance(751U);
    Check(!session.EdgeCueIntent().IsActive() && !art.IsActive(),
          "app occupied warning expires without a state change or new input");
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(!session.EdgeCueIntent().IsActive(),
          "replayed app Enter cannot resurrect an expired warning");
    session.ObserveHallMask(0U);
    session.Advance(100U);
    const auto retry = Request(Art::Op::Enter, 0U, 2U);
    session.SendLitchcraftPacket(retry.bytes.data(), retry.size);
    Check(art.IsActive() && !session.EdgeCueIntent().IsActive(),
          "a new Enter after removing the piece succeeds without a warning");
    std::puts("[APP_OCCUPIED_ENTRY_JOURNEY] input=Enter occupancy=e2 "
              "authoritative=Setup result=BoardNotEmpty pieces=pulse "
              "status=amber expiry=1000ms replay=silent next=remove-piece; "
              "input=new-Enter occupancy=empty result=Ok mode=Litchcraft");
}

void StartedClockAdmission() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    modules::ControlMutationOutcome outcome;
    session.MutateSettingJson(1U, "TIMER", "1", outcome);
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    session.ObserveHallMask(opening);
    session.Advance(2500U);
    session.ObserveHallMask(opening & ~(UINT64_C(1) << 52U));
    session.Advance(100U);
    session.ObserveHallMask(opening);
    session.Advance(250U);
    Check(session.UndoHistoryMoveCount() == 0U && session.GameClock()->IsRunning(), "clock admission trigger is a returned pickup with no chess history yet");
    const auto request = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(request.bytes.data(), request.size);
    Check(!session.Litchcraft().IsActive() && session.GameClock()->IsRunning(), "painting entry cannot stop an already started clock even with initial FEN and no history");
}

void Topologies() {
    for (uint8_t rev = 1U; rev <= 3U; ++rev) {
        Art art;
        art.Configure(rev, 10U);
        art.BeginConnection();
        Send(art, Art::Op::Enter, 1);
        uint16_t id = 2;
        for (uint8_t square = 0; square < 64; ++square) {
            Send(art, Art::Op::Tool, id++, {0, 15, uint8_t(square + 1), 0, 0});
            Send(art, Art::Op::Paint, id++, {1, 0, 3, square});
            Drain(art);
        }
        modules::BoardLedFrame board;
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status;
        art.Compose(board, status);
        Check(Color(art.Artwork()[0], 1, 0, 0) &&
                  Color(board[0].left, 1, 0, 0),
              "already saturated artwork remains unchanged in presentation");
        size_t previewCount = 0U;
        for (const auto color : status) {
            if (color.r != 0U || color.g != 0U || color.b != 0U) {
                ++previewCount;
            }
        }
        Check(previewCount == (rev == 1U ? 0U : 8U),
              "selected color previews on two LEDs around every physical corner");
        for (bool rotate : {false, true}) {
            const auto frame = hardware::MapSemanticLedFrame({static_cast<hardware::BoardRevision>(rev), rotate, false, 0}, board, status);
            const auto topology = hardware::BoardLedTopologyFor(static_cast<hardware::BoardRevision>(rev));
            for (uint8_t sq = 0; sq < 64; ++sq) {
                const auto physical = (rotate ? 63U - sq : sq) * topology.physicalLedsPerSquare;
                Check(frame[physical].r == sq + 1U, "every square address survives asymmetric topology/orientation mapping");
            }
        }
        Send(art, Art::Op::Tool, id++, {0, uint8_t(rev == 3 ? 2 : 10), 40, 0, 0});
        Send(art, Art::Op::Paint, id++, {2, 0, 3, 0});
        art.Compose(board, status);
        const auto output = hardware::MapSemanticLedFrame({static_cast<hardware::BoardRevision>(rev), false, false, 0}, board, status);
        Check(output[1].r == 40U, "supported center/top detail maps to actual emitter");
    }
    const auto mask = modules::ble_device_capabilities::CapabilityMask(false, false, false, false, true);
    Check((mask & 1024U) != 0U, "Litchcraft additive capability is available without OTA/V3 requirements");
}
uint16_t Energy(const virtual_litch_color color) {
    return uint16_t(color.r) + color.g + color.b + color.w;
}
void JourneyCheckpoint(runtime::VirtualLitchSession& session, const uint8_t hardware, const bool rotate, const char* stage, const char* next) {
    const auto semantic = session.SemanticFrame().board[9U][0U];
    const uint16_t index = (rotate ? 54U : 9U) * (hardware == 3U ? 4U : 3U);
    const auto physical = session.PhysicalFrame().pixels[index];
    size_t progress = 0U;
    size_t pickProgress = 0U;
    size_t eraseCue = 0U;
    for (const auto color : session.SemanticFrame().status) {
        if (color.r == 70U && color.g == 70U && color.b == 70U) {
            ++progress;
        }
        if (color.r == 32U && color.g == 32U && color.b == 32U) {
            ++pickProgress;
        }
        if (color.r == 18U && color.g == 18U && color.b == 18U) {
            ++eraseCue;
        }
    }
    const auto text = session.DisplayText();
    std::printf("[LITCHCRAFT_JOURNEY] stage=%s hardware=%u rotate=%u t=%u mode=%s occupancy=%016llx waiting=%u revision=%u semantic9=%u,%u,%u physical9=%u,%u,%u,%u hold_leds=%zu pick_leds=%zu erase_leds=%zu display=\"%.*s\" next=%s\n",
                stage, hardware, rotate ? 1U : 0U, session.State().now_ms,
                session.Litchcraft().IsActive() ? "Litchcraft" : "Setup/Play",
                static_cast<unsigned long long>(session.Litchcraft().IsActive() ? session.Litchcraft().Occupancy() : session.State().physical_occupied_mask),
                session.Litchcraft().IsWaitingForLift() ? 1U : 0U, session.Litchcraft().Revision(),
                semantic.r, semantic.g, semantic.b, physical.r, physical.g, physical.b, physical.w,
                progress, pickProgress, eraseCue, static_cast<int>(text.size()), text.empty() ? "" : text.data(), next);
}
void VirtualQuickPaletteLiftJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "quick-palette journey reaches empty Setup");
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && art.IsActive(),
          "quick-palette journey enters through production protocol ownership");
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, true) ==
                  VIRTUAL_LITCH_OK &&
                  session.Advance(30U) == VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, false) ==
                  VIRTUAL_LITCH_OK &&
                  session.Advance(30U) == VIRTUAL_LITCH_OK,
              "raw quick-palette button settles through production debounce");
    };

    tap(runtime::VirtualLitchPhysicalButton::Left);
    Check(art.IsQuickPaletteVisible(),
          "raw Left opens the production physical quick palette");
    const auto wheelStart = art.Color();
    const auto favoriteStart = art.RecentColors();
    for (uint8_t step = 0U; step < 32U; ++step) {
        tap(runtime::VirtualLitchPhysicalButton::Right);
    }
    Check(Color(art.Color(), wheelStart.r, wheelStart.g, wheelStart.b) &&
              std::equal(favoriteStart.begin(), favoriteStart.end(),
                         art.RecentColors().begin(),
                         [](modules::Rgb a, modules::Rgb b) {
                             return Color(a, b.r, b.g, b.b);
                         }),
          "production D-pad wraps one full 32-color turn without changing favorites");
    Check(session.ObserveHallMask(UINT64_C(1) << 59U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "raw Hall swatch occupancy reaches the production palette owner");
    const auto liftedColor = art.Color();
    Check(session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !art.IsQuickPaletteVisible(),
          "an explicit empty Hall mask commits and closes the sampled palette immediately");
    constexpr uint8_t explicitSquare = 20U;
    Check(session.ObserveHallMask(UINT64_C(1) << explicitSquare) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              Color(art.Artwork()[explicitSquare * 4U], liftedColor.r,
                    liftedColor.g, liftedColor.b) &&
              Energy(session.PhysicalFrame().pixels[explicitSquare * 4U]) > 0U,
          "the first post-lift Hall square paints the sampled color through the physical renderer");
    std::printf(
        "[LITCHCRAFT_QUICK_PALETTE_JOURNEY] stage=explicit-lift "
        "input=Left+Hall59+empty+Hall20 mode=Litchcraft result=painted "
        "color=%u,%u,%u next=lift-or-continue-stroke\n",
        liftedColor.r, liftedColor.g, liftedColor.b);

    Check(session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              session.Advance(Art::PHYSICAL_RELEASE_MS + 1U) ==
                  VIRTUAL_LITCH_OK,
          "explicit-lift brush closes before the coalesced journey");
    tap(runtime::VirtualLitchPhysicalButton::Right);
    Check(session.ObserveHallMask(UINT64_C(1) << 58U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "second raw swatch occupancy selects another wheel color");
    const auto coalescedColor = art.Color();
    constexpr uint8_t coalescedSquare = 21U;
    Check(session.ObserveHallMask(UINT64_C(1) << coalescedSquare) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !art.IsQuickPaletteVisible() &&
              Color(art.Artwork()[coalescedSquare * 4U], coalescedColor.r,
                    coalescedColor.g, coalescedColor.b) &&
              Energy(session.PhysicalFrame().pixels[coalescedSquare * 4U]) > 0U,
          "a coalesced swatch-to-destination Hall scan closes and paints without an extra lift");
    std::printf(
        "[LITCHCRAFT_QUICK_PALETTE_JOURNEY] stage=coalesced-lift "
        "input=Right+Hall58-to-Hall21 mode=Litchcraft result=painted "
        "color=%u,%u,%u next=lift-or-continue-stroke\n",
        coalescedColor.r, coalescedColor.g, coalescedColor.b);
}
void VirtualJourney(const uint8_t hardware, const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate ? 1U : 0U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    Check(session.PlacementModeActive(), "empty boot starts in production Setup placement");
    const auto emptySetupFrame = session.SemanticFrame();
    session.Litchcraft().BeginConnection();
    auto send = [&](Art::Op op, uint16_t id, std::initializer_list<uint8_t> data = {}) {
        const auto packet = Request(op, op == Art::Op::Enter ? 0U : session.Litchcraft().SessionId(), id, data);
        session.SendLitchcraftPacket(packet.bytes.data(), packet.size);
    };
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive(),
          "first face-button edge waits inside the bounded chord-priority window");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive(),
          "simultaneous X+O waits for a complete hold and release");
    session.Advance(1460U);
    Check(!session.Litchcraft().IsActive(),
          "completed entry hold remains pending while both buttons are down");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20U);
    Check(session.Litchcraft().IsActive(),
          "release after the completed hold enters Litchcraft");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    Check(session.Litchcraft().IsActive() &&
              session.Litchcraft().IsExitHoldActive(),
          "the same X+O chord starts exit confirmation without a transient undo");
    session.Advance(1460U);
    Check(session.Litchcraft().IsActive(),
          "completed active chord still waits for release");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive(),
          "release after the full active chord exits Litchcraft");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(20U);

    session.ObserveHallMask(UINT64_C(1) << 8U);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive() &&
              session.EdgeCueIntent().kind == modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "X+O rejects Litchcraft entry while even one physical square is occupied");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(20U);
    session.ObserveHallMask(0U);
    session.Advance(20U);

    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.Advance(Art::MODE_CHORD_PRIORITY_MS + 1U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive(),
          "near-simultaneous face presses outside the priority window do not enter");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.Advance(1460U);
    Check(!session.Litchcraft().IsActive(),
          "reverse-order retry also waits for release after the full hold");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(20U);
    Check(session.Litchcraft().IsActive(),
          "reverse-order completed chord enters on first release");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20U);
    session.Litchcraft().Exit();
    session.Advance(16U);
    Check(session.PlacementModeActive(),
          "board-only Litchcraft exit returns to Setup before app entry");
    send(Art::Op::Enter, 1);
    Check(session.Litchcraft().IsActive(), "VirtualLitch empty setup admits explicit Litchcraft transition");
    Check(!session.Litchcraft().IsWaitingForLift(),
          "zero-piece admission has no legacy lift-existing-pieces phase");
    JourneyCheckpoint(session, hardware, rotate, "enter-empty", "touch-piece-to-paint");
    session.Advance(250U); // Finish only the presentation handoff before exact RGB checks.
    const uint16_t mapped9 = (rotate ? 54U : 9U) * (hardware == 3U ? 4U : 3U);
    Check(session.DisplayText().empty(), "painting display has no stale chess or menu label");
    session.ObserveHallMask(UINT64_C(1) << (rotate ? 54U : 9U));
    const auto mark = session.SemanticFrame().board[9U][0U];
    const auto physicalMark = session.PhysicalFrame().pixels[mapped9];
    const virtual_litch_color expectedMark{255U, 116U, 48U, 0U};
    const auto scaledMark = modules::ScaleRgbSaturatingBy256(
        modules::Rgb{expectedMark.r, expectedMark.g, expectedMark.b},
        modules::litchcraft_physical_presentation::CANVAS_SCALE256);
    const auto presentedMark = hardware == 1U
        ? scaledMark
        : modules::rgb_optical_calibration::PresentForS3(scaledMark);
    Check(mark.r == expectedMark.r && mark.g == expectedMark.g &&
              mark.b == expectedMark.b && physicalMark.r == presentedMark.r &&
              physicalMark.g == presentedMark.g &&
              physicalMark.b == presentedMark.b && physicalMark.w == 0U,
          "physical Hall mark retains exact document RGB semantically and uses bounded W=0 presentation");
    JourneyCheckpoint(session, hardware, rotate, "magnetic-mark", "lift-or-choose-color");
    Check(Color(session.Litchcraft().Artwork()[36], 255, 116, 48), "production normalized Hall ingress paints authoritative canvas");
    Check(session.UndoHistoryMoveCount() == 0U && !session.HintDisplayRequested(), "painting creates neither chess moves nor hints");
    virtual_litch_action_request hint{};
    virtual_litch_default_action_request(&hint);
    hint.action = static_cast<uint8_t>(modules::ControlAction::Hint);
    hint.request_id = 300U;
    modules::ControlMutationOutcome outcome;
    session.DispatchControlAction(hint, outcome);
    Check(outcome.reason == modules::ControlMutationReason::Busy, "game hint command is rejected during art rather than creating an engine request");
    send(Art::Op::Tool, 2, {0, 15, 5, 35, 85});
    Check(Color(session.Litchcraft().Artwork()[36], 5, 35, 85), "app tool and physical resting brush share production session");
    send(Art::Op::Paint, 3, {1, 0, 3, 0, 63});
    Check(!session.WaterSloshActive(), "Litchcraft owns rendering exclusively over slosh");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(100U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(100U);
    Check(Color(session.Litchcraft().Artwork()[0], 0, 0, 0), "physical X undoes app stroke through shared history");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::Press, true);
    session.Advance(100U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::Press, false);
    Check(session.Litchcraft().IsActive(), "unused center cannot alter active Litchcraft");
    session.ObserveHallMask(0U);
    send(Art::Op::Exit, 4U);
    session.Advance(16U);
    Check(!session.Litchcraft().IsActive(),
          "app Exit opcode releases production render ownership");
    const auto setupAfterAppExit = session.SemanticFrame().board[9U][0U];
    Check(!(setupAfterAppExit.r == 5U && setupAfterAppExit.g == 35U &&
            setupAfterAppExit.b == 85U),
          "app Exit immediately restores Setup presentation instead of retained artwork");
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    Check(session.PlacementModeActive() &&
              session.State().human_illegal_mask == 0U,
          "app Exit returns an empty untouched board to Setup placement instead of generic invalid feedback");
    for (uint8_t frame = 0U; frame < 30U; ++frame) {
        session.Advance(100U);
        Check(session.PlacementModeActive() &&
                  session.State().human_illegal_mask == 0U,
              "empty Setup keeps ownership throughout post-exit runtime updates");
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (square == 4U || square == 60U) continue;
            for (const uint8_t slot : {uint8_t(0U), uint8_t(2U)}) {
                const auto expected = emptySetupFrame.board[square][slot];
                const auto actual = session.SemanticFrame().board[square][slot];
                Check(expected.r == actual.r && expected.g == actual.g &&
                          expected.b == actual.b,
                      "post-exit outer square LEDs match empty boot Setup, never generic invalid flash");
            }
        }
    }
    send(Art::Op::Enter, 5U);
    Check(session.Litchcraft().IsActive(),
          "fresh Setup can re-enter Litchcraft after app navigation exit");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    Check(session.Litchcraft().IsActive() &&
              session.Litchcraft().IsExitHoldActive(),
          "same-owner X+O starts production exit confirmation");
    session.Advance(1460U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20U);
    Check(!session.Litchcraft().IsActive(),
          "same-owner X+O exits production mode on release after full hold");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(200U);
    session.ObserveHallMask(0U);
    Check(session.UndoHistoryMoveCount() == 0U, "exit brush activity cannot become a ghost chess move");
    std::printf("[LITCHCRAFT_JOURNEY] exit t=%u mode=Setup occupancy=empty moves=%u next=setup-chess hold_expired=1\n",
                session.State().now_ms, session.UndoHistoryMoveCount());
    const auto observe = [&](const uint64_t logical) {
        uint64_t physical = 0U;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((logical & (UINT64_C(1) << square)) != 0U) {
                physical |= UINT64_C(1) << (rotate ? 63U - square : square);
            }
        }
        session.ObserveHallMask(physical);
    };
    observe(opening);
    session.Advance(2500U);
    Check(!session.PlacementModeActive(),
          "completed post-Litchcraft setup returns to ordinary Play input");
    observe(opening & ~(UINT64_C(1) << 52U));
    session.Advance(30U);
    observe((opening & ~(UINT64_C(1) << 52U)) | (UINT64_C(1) << 36U));
    session.Advance(2500U);
    session.Advance(50U);
    Check(session.UndoHistoryMoveCount() == 1U, "after art exit, production setup and e2-e4 create exactly one real chess move");
    std::printf("[LITCHCRAFT_JOURNEY] resume hardware=%u rotate=%u mode=Play move=e2e4 history=%u next=black-move accepted=%u\n",
                hardware, rotate ? 1U : 0U, session.UndoHistoryMoveCount(), session.UndoHistoryMoveCount() == 1U);
    send(Art::Op::Enter, 6);
    Check(!session.Litchcraft().IsActive(), "active/resumable gameplay rejects painting entry");
}

void OccupiedBoardBlocksPhysicalEntryJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    constexpr uint64_t occupied = UINT64_C(1) << 12U;
    session.ObserveBootHallMask(occupied);
    session.Advance(3000U);

    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, true);
    session.Advance(20U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(20U);
    const auto blockedFrame = session.SemanticFrame();
    const auto cue = session.EdgeCueIntent();
    size_t amberStatus = 0U;
    for (const virtual_litch_color color : blockedFrame.status) {
        if (color.r > color.g && color.g > color.b) {
            ++amberStatus;
        }
    }
    std::printf(
        "[LITCHCRAFT_OCCUPIED_ENTRY_JOURNEY] trigger t=%u cue=%u "
        "amber_status=%zu mode=%u\n",
        session.State().now_ms,
        static_cast<unsigned>(cue.kind),
        amberStatus,
        session.Litchcraft().IsActive() ? 1U : 0U);
    Check(!session.Litchcraft().IsActive() &&
              cue.kind == modules::EdgeLedCueKind::OccupiedBoardBlocked &&
              amberStatus == modules::EDGE_LED_COUNT,
          "occupied X+O stays out of Litchcraft and shows a full amber status warning");
    const auto pulsedOff = blockedFrame.board[12U][1U];
    session.Advance(200U);
    const auto pulsedOn = session.SemanticFrame().board[12U][1U];
    Check(pulsedOff.r + pulsedOff.g + pulsedOff.b !=
              pulsedOn.r + pulsedOn.g + pulsedOn.b,
          "occupied X+O pulses the actual blocking piece");
    Check(session.Advance(799U) == VIRTUAL_LITCH_OK &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "occupied-board warning remains visible through its penultimate millisecond");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
              !session.EdgeCueIntent().IsActive() &&
              !session.Litchcraft().IsActive(),
          "occupied-board warning expires after one second without changing mode");
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::X, false);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, false);
    std::printf(
        "[LITCHCRAFT_OCCUPIED_ENTRY_JOURNEY] occupancy=%016llx "
        "mode=Setup accepted=0 pieces=pulse status=amber duration_ms=1000 "
        "next=lift-pieces\n",
        static_cast<unsigned long long>(occupied));
}
void VirtualHallLiftBlendColorToPaintJourney(const bool effect) {
    modules::litchcraft_frame_store::Manifest manifest{};
    manifest.active = true;
    modules::litchcraft_frame_store::Frame frame{};
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        frame.emitters[36U + slot] = {{25U, 75U, 125U},
            static_cast<uint8_t>(effect ? 2U : 0U),
            static_cast<uint16_t>(effect ? 1000U : 0U)};
    }
    modules::litchcraft_frame_stream::FrameStream stream;
    Check(stream.StageFirstFrame(manifest, 9U, frame),
          "lift-blend journey restores its complete Store artwork fixture");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = 1U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Litchcraft().AttachFrameStream(&stream);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "lift-blend journey adopts restored Litchcraft authority");
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto tool = Request(Art::Op::Tool, art.SessionId(), 1U,
                              {0U, 15U, 255U, 116U, 48U});
    session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    Drain(art);
    const auto checkpoint = [&](const char* stage, const char* next) {
        const auto shown = session.SemanticFrame().board[9U][1U];
        const auto painted = session.SemanticFrame().board[10U][1U];
        const auto color = art.Color();
        const auto text = session.DisplayText();
        std::printf("[LITCHCRAFT_LIFT_BLEND] stage=%s effect=%s t=%u hardware=V3 rotate=1 "
                    "mode=%s occupancy=%016llx accepted=1 tool=%s mask=%u "
                    "brush=%u,%u,%u source_top=%u,%u,%u paint_top=%u,%u,%u "
                    "display=\"%.*s\" next=%s expiry=none\n", stage, effect ? "Flash" : "Off",
                    session.State().now_ms, art.IsActive() ? "Litchcraft" : "Setup/Play",
                    static_cast<unsigned long long>(art.Occupancy()),
                    art.IsErasing() ? "Erase" : "Paint", art.Target(),
                    color.r, color.g, color.b, shown.r, shown.g, shown.b,
                    painted.r, painted.g, painted.b, static_cast<int>(text.size()),
                    text.empty() ? "" : text.data(), next);
    };
    const modules::Rgb captured = effect ? modules::Rgb{12U, 37U, 62U} : modules::Rgb{140U, 96U, 86U};
    const auto historyBefore = art.UndoCount();
    Check(session.ObserveHallMask(UINT64_C(1) << 54U) == VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "rotated raw Hall placement paints and completes only the existing dwell");
    const auto dwell = session.SemanticFrame().board[9U][1U];
    Check(dwell.r == 255U && dwell.g == 116U && dwell.b == 48U,
          "dwell leaves the current brush mark visible");
    checkpoint("dwell-complete", "hold-to-blend-or-lift-to-keep-paint");
    session.Advance(500U);
    const auto blend = session.SemanticFrame().board[9U][1U];
    Check(blend.r == captured.r && blend.g == captured.g && blend.b == captured.b,
          "production renderer shows the exact static/effected halfway blend before lift");
    checkpoint("half-blend", "lift-to-capture-shown-RGB");
    Check(session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              Color(art.Color(), captured.r, captured.g, captured.b) &&
              art.Target() == 15U && !art.IsErasing(),
          "production Hall lift captures the visible blend as authoritative brush RGB");
    modules::litchcraft_frame_store::Frame restored{};
    uint16_t index = UINT16_MAX;
    Check(stream.CopyDisplayed(restored, index) &&
              Color(restored.emitters[37U].color, 25U, 75U, 125U) &&
              restored.emitters[37U].effect == (effect ? 2U : 0U) &&
              restored.emitters[37U].effectDurationMs == (effect ? 1000U : 0U) &&
              art.UndoCount() == historyBefore,
          "partial Store pick restores source RGB/effects without phantom Undo");
    Art::Packet packet;
    bool authoritativeTool = false;
    bool picked = false;
    while (art.PeekPacket(packet)) {
        if (packet.bytes[1U] == uint8_t(Art::Event::Tool)) {
            authoritativeTool = packet.bytes[12U] == captured.r &&
                packet.bytes[13U] == captured.g && packet.bytes[14U] == captured.b &&
                packet.bytes[15U] == 0U && packet.bytes[16U] == 15U;
        }
        if (packet.bytes[1U] == uint8_t(Art::Event::ColorPick) &&
            packet.bytes[12U] == 2U) picked = true;
        art.AcceptPacket();
    }
    Check(authoritativeTool && picked,
          "existing Tool and ColorPick receipts publish the captured RGB to the app");
    checkpoint("lift-captured", "place-to-paint-captured-RGB");
    Check(session.ObserveHallMask(UINT64_C(1) << 53U) == VIRTUAL_LITCH_OK,
          "next rotated raw Hall placement is accepted immediately");
    const auto painted = session.SemanticFrame().board[10U][1U];
    const auto paintedSide = session.SemanticFrame().board[10U][0U];
    Check(painted.r == captured.r && painted.g == captured.g && painted.b == captured.b &&
              paintedSide.r == captured.r && paintedSide.g == captured.g && paintedSide.b == captured.b,
          "next production paint uses captured RGB only on selected emitters");
    const auto preview = modules::ScaleRgbSaturatingBy256(captured, 48U);
    Check(std::count_if(std::begin(session.SemanticFrame().status),
                       std::end(session.SemanticFrame().status),
                       [&](const virtual_litch_color c) {
                           return c.r == preview.r && c.g == preview.g && c.b == preview.b;
                       }) == 8U,
          "eight semantic Paint corners show the captured brush color");
    checkpoint("captured-color-painted", "lift-before-dwell-to-finish-stroke");
    session.ObserveHallMask(0U);
    session.Advance(Art::PHYSICAL_RELEASE_MS);
    Check(art.UndoCount() == historyBefore + 1U,
          "painting after partial pick remains one normal Undo operation");
    checkpoint("stroke-finished", "continue-painting-or-Undo");
    if (effect) {
        session.ObserveHallMask(UINT64_C(1) << 54U);
        session.Advance(Art::PICK_ARM_MS + Art::PICK_PROGRESS_MS);
        Check(Color(art.Color(), 25U, 75U, 125U) && !art.IsErasing(),
              "full hold still selects intended source RGB while its Flash effect is off");
        checkpoint("full-hold-intended-source", "lift-and-paint-with-source-RGB");
        session.ObserveHallMask(0U);
    }
}

void VirtualBufferedFacePressCancelsColorPickJourney(
    const runtime::VirtualLitchPhysicalButton button, const bool chord) {
    modules::litchcraft_frame_store::Manifest manifest{};
    manifest.active = true;
    modules::litchcraft_frame_store::Frame frame{};
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        frame.emitters[36U + slot].color = {25U, 75U, 125U};
    }
    modules::litchcraft_frame_stream::FrameStream stream;
    Check(stream.StageFirstFrame(manifest, 9U, frame), "buffered-face fixture seeds source artwork");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Litchcraft().AttachFrameStream(&stream);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto tool = Request(Art::Op::Tool, art.SessionId(), 1U,
                              {0U, 15U, 255U, 116U, 48U});
    session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    // A completed real stroke must survive Undo of the later canceled pick.
    session.ObserveHallMask(UINT64_C(1) << 10U);
    session.ObserveHallMask(0U);
    session.Advance(Art::PHYSICAL_RELEASE_MS);
    const auto priorHistory = art.UndoCount();
    Check(priorHistory == 1U, "prior real stroke establishes the Undo boundary");
    session.ObserveHallMask(UINT64_C(1) << 9U);
    session.Advance(1500U);
    Drain(art);
    const auto historyWhileHeld = art.UndoCount();
    const auto set = [&](const runtime::VirtualLitchPhysicalButton selected, const bool down) {
        return session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, selected, down) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        uint16_t index = UINT16_MAX;
        Check(stream.CopyDisplayed(frame, index), "buffered-face checkpoint reads authoritative frame");
        const auto source = frame.emitters[36U].color;
        const auto prior = frame.emitters[40U].color;
        const auto brush = art.Color();
        const auto text = session.DisplayText();
        std::printf("[LITCHCRAFT_FACE_CANCEL] gesture=%s stage=%s t=%u "
                    "mode=%s occupancy=%016llx brush=%u,%u,%u source=%u,%u,%u "
                    "prior=%u,%u,%u undo=%u erase=%u exit_hold=%u display=\"%.*s\" "
                    "next=%s expiry=750ms-face-priority/1500ms-chord\n",
                    chord ? "X+O" : button == runtime::VirtualLitchPhysicalButton::X ? "X" : "O",
                    stage, session.State().now_ms, art.IsActive() ? "Litchcraft" : "Setup",
                    static_cast<unsigned long long>(art.Occupancy()),
                    brush.r, brush.g, brush.b, source.r, source.g, source.b,
                    prior.r, prior.g, prior.b, art.UndoCount(), art.IsErasing(), art.IsExitHoldActive(),
                    static_cast<int>(text.size()), text.empty() ? "" : text.data(), next);
    };
    Check(set(button, true), "raw face Press passes production 20ms debounce");
    bool idle = false;
    Art::Packet packet;
    while (art.PeekPacket(packet)) {
        if (packet.bytes[1U] == uint8_t(Art::Event::ColorPick)) idle = packet.bytes[12U] == 0U;
        art.AcceptPacket();
    }
    checkpoint("face-accepted", "lift-before-releasing-face");
    Check(idle && Color(frame.emitters[36U].color, 255U, 116U, 48U) &&
              art.UndoCount() == historyWhileHeld && !art.IsErasing(),
          "accepted face Press cancels pick immediately while Undo/toggle remains buffered");
    if (chord) {
        Check(set(runtime::VirtualLitchPhysicalButton::O, true) && art.IsExitHoldActive(),
              "second qualified face Press forms exit chord without Undo or toggle");
    }
    Check(session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK, "Hall lift arrives before face Release");
    checkpoint("lift-before-release", chord ? "complete-exit-hold-and-release" : "release-face-action");
    Check(Color(art.Color(), 255U, 116U, 48U) &&
              Color(frame.emitters[36U].color, 255U, 116U, 48U) &&
              Color(frame.emitters[40U].color, 255U, 116U, 48U),
          "a face-canceled pick cannot capture or restore when Hall lift arrives before Release");
    if (chord) {
        session.Advance(Art::ENTRY_HOLD_MS - 20U);
        Check(art.IsActive() && art.IsExitHoldActive(), "qualified exit still waits for Release");
    }
    if (!chord && button == runtime::VirtualLitchPhysicalButton::O)
        session.Advance(1000U);
    Check(set(button, false), "raw face Release passes production debounce");
    checkpoint("action-released", chord ? "place-chess-pieces" : "continue-painting");
    Check(Color(frame.emitters[40U].color, 255U, 116U, 48U),
          "delayed Undo or exit must preserve the prior real stroke");
    if (chord) {
        Check(!art.IsActive() && Color(frame.emitters[36U].color, 255U, 116U, 48U) &&
                  art.UndoCount() == priorHistory + 1U && !art.IsErasing(),
              "qualified exit chord preserves both strokes without Undo or toggle");
        set(runtime::VirtualLitchPhysicalButton::O, false);
    } else if (button == runtime::VirtualLitchPhysicalButton::X) {
        Check(art.IsActive() && Color(frame.emitters[36U].color, 25U, 75U, 125U) &&
                  art.UndoCount() == priorHistory,
              "delayed X undoes only the canceled pick's paint mark");
    } else {
        Check(art.IsActive() && art.IsErasing() &&
                  Color(frame.emitters[36U].color, 255U, 116U, 48U),
              "long O toggles Erase while retaining the ordinary paint mark");
    }
}

void VirtualColorPickAndHistoryJourney(const uint8_t hardware, const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate ? 1U : 0U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    uint16_t request = 1U;
    const auto send = [&](Art::Op op, std::initializer_list<uint8_t> data = {}) {
        const auto packet = Request(op, op == Art::Op::Enter ? 0U : art.SessionId(), request++, data);
        session.SendLitchcraftPacket(packet.bytes.data(), packet.size);
    };
    const auto tap = [&](runtime::VirtualLitchPhysicalButton button) {
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, true);
        session.Advance(100U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, false);
        session.Advance(100U);
    };
    const auto toggleTool = [&]() {
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true);
        session.Advance(20U);
        session.Advance(1100U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false);
        session.Advance(100U);
    };
    const auto occupancy = [&](uint64_t mask) {
        uint64_t physical = 0U;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((mask & (UINT64_C(1) << square)) != 0U) {
                physical |= UINT64_C(1) << (rotate ? 63U - square : square);
            }
        }
        session.ObserveHallMask(physical);
    };
    send(Art::Op::Enter);
    toggleTool();
    const auto wipeFrame = session.SemanticFrame().status;
    const auto wipeLeds = std::count_if(
        wipeFrame, wipeFrame + modules::EDGE_LED_COUNT,
        [](virtual_litch_color color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    Check(art.IsErasing() && wipeLeds == (hardware == 1U ? 0U : 8U),
          "physical O starts erase with an inward perimeter wipe on V2/V3");
    JourneyCheckpoint(session, hardware, rotate, "erase-wipe",
                      "erase-lit-cells-or-toggle-paint");
    session.Advance(Art::ERASE_WIPE_MS);
    std::array<virtual_litch_color, modules::EDGE_LED_COUNT> firstMarker{};
    std::copy_n(session.SemanticFrame().status, firstMarker.size(),
                firstMarker.begin());
    const auto firstMarkerLeds = std::count_if(
        firstMarker.begin(), firstMarker.end(),
        [](virtual_litch_color color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    Check(firstMarkerLeds == (hardware == 1U ? 0U : 4U),
          "erase wipe settles into two quiet opposite-corner markers");
    JourneyCheckpoint(session, hardware, rotate, "erase-marker-a",
                      "erase-lit-cells-or-toggle-paint");
    session.Advance(Art::ERASE_MARKER_PHASE_MS);
    const auto secondMarker = session.SemanticFrame().status;
    bool markerChanged = false;
    for (size_t i = 0U; i < firstMarker.size(); ++i) {
        markerChanged = markerChanged ||
            firstMarker[i].r != secondMarker[i].r ||
            firstMarker[i].g != secondMarker[i].g ||
            firstMarker[i].b != secondMarker[i].b;
    }
    Check(hardware == 1U || markerChanged,
          "persistent erase marker alternates to the other two corners");
    JourneyCheckpoint(session, hardware, rotate, "erase-marker-b",
                      "erase-lit-cells-or-toggle-paint");
    toggleTool();
    const auto paintPreviewLeds = std::count_if(
        std::begin(session.SemanticFrame().status),
        std::end(session.SemanticFrame().status),
        [](virtual_litch_color color) {
            return color.r != 0U || color.g != 0U || color.b != 0U;
        });
    Check(!art.IsErasing() &&
              paintPreviewLeds == (hardware == 1U ? 0U : 8U),
          "physical O returns to Paint and restores the current-color preview");
    send(Art::Op::Tool, {0U, 15U, 25U, 75U, 125U});
    send(Art::Op::Paint, {1U, 0U, 3U, 9U});
    send(Art::Op::Tool, {0U, 15U, 255U, 116U, 48U});
    const auto historyBefore = art.UndoCount();
    occupancy(UINT64_C(1) << 9U);
    Check(Color(art.Artwork()[36U], 255U, 116U, 48U), "magnet touch paints immediately before hold-to-pick");
    std::array<virtual_litch_color, modules::EDGE_LED_COUNT> prePickStatus{};
    std::copy_n(session.SemanticFrame().status, prePickStatus.size(),
                prePickStatus.begin());
    session.Advance(1000U);
    bool preProgressUnchanged = true;
    for (size_t i = 0U; i < prePickStatus.size(); ++i) {
        const auto actual = session.SemanticFrame().status[i];
        const auto expected = prePickStatus[i];
        preProgressUnchanged = preProgressUnchanged &&
            actual.r == expected.r && actual.g == expected.g &&
            actual.b == expected.b && actual.w == expected.w;
    }
    Check(preProgressUnchanged,
          "the eyedropper perimeter stays quiet through the first one-second hold");
    JourneyCheckpoint(session, hardware, rotate, "armed-color-pick", "keep-holding-for-eyedropper-progress");
    session.Advance(500U);
    const auto halfProgress = std::count_if(
        std::begin(session.SemanticFrame().status),
        std::end(session.SemanticFrame().status),
        [](virtual_litch_color c) {
            return c.r == 6U && c.g == 19U && c.b == 32U;
        });
    Check(halfProgress == 0U,
          "eyedropper progress does not replace the active paint corners");
    const auto fadingTouch = session.SemanticFrame().board[9U][0U];
    Check(fadingTouch.r > 25U && fadingTouch.r < 255U &&
              fadingTouch.g > 75U && fadingTouch.g < 116U &&
              fadingTouch.b > 48U && fadingTouch.b < 125U,
          "the touched emitter fades from its preview mark toward the original artwork during color-read progress");
    JourneyCheckpoint(session, hardware, rotate, "half-color-pick", "keep-holding-to-pick-color");
    session.Advance(500U);
    const auto sampledPickLeds = [&]() {
        return std::count_if(std::begin(session.SemanticFrame().status),
                             std::end(session.SemanticFrame().status),
                             [](virtual_litch_color color) {
                                 const auto expected = modules::ScaleRgbSaturatingBy256(
                                     {25U, 75U, 125U}, 48U);
                                 return color.r == expected.r && color.g == expected.g &&
                                        color.b == expected.b;
                             });
    };
    Check(Color(art.Artwork()[36U], 25U, 75U, 125U) && Color(art.Color(), 25U, 75U, 125U) &&
              !art.IsErasing() && art.UndoCount() == historyBefore &&
              sampledPickLeds() == (hardware == 1U ? 0U : 8U),
          "qualified hold plus progress restores pre-touch art and picks color without phantom undo history");
    if (hardware == 3U) {
        const auto topology = hardware::BoardLedTopologyFor(
            static_cast<hardware::BoardRevision>(hardware));
        bool fullPhysicalSample = true;
        for (uint8_t ringIndex = 0U; ringIndex < 32U; ++ringIndex) {
            const auto physicalSample = session.PhysicalFrame().pixels[
                topology.physicalBoardLedCount + ringIndex];
            const bool corner = ringIndex % 8U == 0U || ringIndex % 8U == 7U;
            fullPhysicalSample = fullPhysicalSample && (corner
                ? physicalSample.b > physicalSample.g && physicalSample.g > physicalSample.r && physicalSample.w == 0U
                : Energy(physicalSample) == 0U);
        }
        Check(fullPhysicalSample,
              "V3 maps the sampled paint color onto only the physical corners without white extraction");
    }
    JourneyCheckpoint(session, hardware, rotate, "color-picked", "lift-and-paint-with-picked-color");
    const auto pickedRevision = art.Revision();
    session.Advance(599U);
    Check(sampledPickLeds() == (hardware == 1U ? 0U : 8U),
          "sampled color remains on paint corners through 599 milliseconds");
    if (hardware == 3U) {
        const auto topology = hardware::BoardLedTopologyFor(
            static_cast<hardware::BoardRevision>(hardware));
        bool physicalStillFull = true;
        for (uint8_t ringIndex = 0U; ringIndex < 32U; ++ringIndex) {
            const auto color = session.PhysicalFrame().pixels[
                topology.physicalBoardLedCount + ringIndex];
            const bool corner = ringIndex % 8U == 0U || ringIndex % 8U == 7U;
            physicalStillFull = physicalStillFull && (corner
                ? color.b > color.g && color.g > color.r && color.w == 0U
                : Energy(color) == 0U);
        }
        Check(physicalStillFull,
              "V3 physical paint corners remain stable through 599 milliseconds");
    }
    session.Advance(1U);
    Check(sampledPickLeds() == (hardware == 1U ? 0U : 8U) && art.Revision() == pickedRevision,
          "paint corners stay stable at 600 milliseconds without resampling a resting magnet");
    if (hardware == 3U) {
        const auto topology = hardware::BoardLedTopologyFor(
            static_cast<hardware::BoardRevision>(hardware));
        size_t physicalSampleCount = 0U;
        for (uint8_t ringIndex = 0U; ringIndex < 32U; ++ringIndex) {
            const auto color = session.PhysicalFrame().pixels[
                topology.physicalBoardLedCount + ringIndex];
            if (color.b > color.g && color.g > color.r && color.w == 0U) {
                ++physicalSampleCount;
            }
        }
        Check(physicalSampleCount == 8U,
              "the full sampled-color V3 confirmation returns to its eight-LED color preview at 600 milliseconds");
    }
    occupancy(0U);
    session.Advance(400U);
    occupancy(UINT64_C(1) << 10U);
    session.Advance(999U);
    occupancy(0U);
    session.Advance(400U);
    Check(sampledPickLeds() != 32U,
          "lifting during the initial dwell cancels picking without a late confirmation");
    Check(Color(art.Artwork()[40U], 25U, 75U, 125U) && art.UndoCount() == historyBefore + 1U,
          "lifting during the initial dwell retains the paint mark as one undo operation");
    const auto wheel42 = modules::ui_color_wheel::WheelToRgb(42U);
    send(Art::Op::Tool, {0U, 15U, wheel42.r, wheel42.g, wheel42.b});
    tap(runtime::VirtualLitchPhysicalButton::Up);
    const auto wheel43 = modules::ui_color_wheel::WheelToRgb(43U);
    Check(Color(art.Color(), wheel43.r, wheel43.g, wheel43.b),
          "physical Up advances one position around the continuous color wheel");
    tap(runtime::VirtualLitchPhysicalButton::Down);
    Check(Color(art.Color(), wheel42.r, wheel42.g, wheel42.b),
          "physical Down returns one position around the continuous color wheel");
    send(Art::Op::Paint, {2U, 0U, 3U, 11U});
    send(Art::Op::Paint, {3U, 0U, 3U, 12U});
    Drain(art);
    send(Art::Op::Snapshot);
    Art::Packet packet;
    for (unsigned i = 0U; i < 12U; ++i) {
        art.PeekPacket(packet);
        art.AcceptPacket();
    }
    tap(runtime::VirtualLitchPhysicalButton::X);
    tap(runtime::VirtualLitchPhysicalButton::X);
    tap(runtime::VirtualLitchPhysicalButton::X);
    tap(runtime::VirtualLitchPhysicalButton::Press);
    Check(Color(art.Artwork()[40U], 0U, 0U, 0U) && Color(art.Artwork()[44U], 0U, 0U, 0U),
          "three physical X undo presses exhaust history and unused center cannot redo");
    unsigned deltas = 0U;
    while (art.PeekPacket(packet)) {
        Check(packet.bytes[1] != uint8_t(Art::Event::ResyncRequired),
              "production physical history input does not invalidate snapshot synchronization");
        if (packet.bytes[1] == uint8_t(Art::Event::Cell)) {
            ++deltas;
        }
        art.AcceptPacket();
    }
    Check(deltas == 3U, "three physical Undo operations require only three Cell notifications");
    JourneyCheckpoint(session, hardware, rotate, "physical-history-coherent", "continue-painting");
    occupancy((UINT64_C(1) << 13U) | (UINT64_C(1) << 14U));
    session.Advance(1200U);
    Check(Color(art.Artwork()[52U], wheel42.r, wheel42.g, wheel42.b) &&
              Color(art.Artwork()[56U], wheel42.r, wheel42.g, wheel42.b),
          "multi-square resting footprint continues painting and never picks a color");
    occupancy(0U);
    session.Advance(400U);
    JourneyCheckpoint(session, hardware, rotate, "multi-square-pick-rejected", "lift-and-paint");
    Check(session.UndoHistoryMoveCount() == 0U && !session.HintDisplayRequested(),
          "new painting gestures never enter chess history or hint controls");

    Art erasePick;
    erasePick.Configure(hardware, 952U);
    erasePick.BeginConnection();
    Send(erasePick, Art::Op::Enter, 1U);
    Send(erasePick, Art::Op::Tool, 2U, {0U, 15U, 40U, 90U, 180U});
    Drain(erasePick);
    erasePick.ObserveOccupancy(UINT64_C(1) << 4U);
    fxe::ManualMillis::AdvanceMs(Art::PICK_ARM_MS + Art::PICK_PROGRESS_MS);
    erasePick.Update();
    Check(erasePick.IsErasing() && Color(erasePick.Color(), 40U, 90U, 180U),
          "sampling an unlit emitter selects Erase while preserving the last paint color");
    modules::BoardLedFrame eraseBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> eraseStatus{};
    erasePick.Compose(eraseBoard, eraseStatus);
    const auto eraseWipe = std::count_if(
        eraseStatus.begin(), eraseStatus.end(),
        [](modules::Rgb color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    Check(eraseWipe == (hardware == 1U ? 0U : 8U),
          "black color-pick immediately starts the erase sweep");
    const std::array<modules::Rgb, 10U> exactColors{{
        {255U, 116U, 48U}, {255U, 52U, 82U}, {255U, 207U, 45U},
        {55U, 215U, 105U}, {34U, 195U, 226U}, {63U, 106U, 255U},
        {174U, 83U, 255U}, {255U, 255U, 255U}, {40U, 40U, 40U},
        {0U, 0U, 0U}}};
    const uint16_t colorPixel = (rotate ? 48U : 15U) * (hardware == 3U ? 4U : 3U);
    for (uint8_t i = 0U; i < exactColors.size(); ++i) {
        const auto color = exactColors[i];
        send(Art::Op::Tool, {0U, 15U, color.r, color.g, color.b});
        send(Art::Op::Paint, {uint8_t(i + 4U), 0U, 3U, 15U});
        const auto semantic = session.SemanticFrame().board[15U][0U];
        const auto physical = session.PhysicalFrame().pixels[colorPixel];
        const auto canvasColor = modules::ScaleRgbSaturatingBy256(
            color, modules::litchcraft_physical_presentation::CANVAS_SCALE256);
        const auto physicalColor = hardware == 1U
            ? canvasColor
            : modules::rgb_optical_calibration::PresentForS3(canvasColor);
        Check(semantic.r == color.r && semantic.g == color.g &&
                  semantic.b == color.b && physical.r == physicalColor.r &&
                  physical.g == physicalColor.g && physical.b == physicalColor.b &&
                  physical.w == 0U,
              "production palette keeps exact document RGB while applying canvas scale, shared S3 calibration, and W=0");
    }
}

void VirtualLiveOrientationJourney(const char* const settingKey,
                                   const char* const settingValue,
                                   const bool playerEndSetting) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    session.Litchcraft().BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    const auto tool = Request(
        Art::Op::Tool, session.Litchcraft().SessionId(), 2U,
        {0U, 15U, 255U, 0U, 0U});
    session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    const auto paint = Request(
        Art::Op::Paint, session.Litchcraft().SessionId(), 3U,
        {1U, 0U, 3U, 0U});
    session.SendLitchcraftPacket(paint.bytes.data(), paint.size);
    const auto orientationApplied = [&]() {
        return playerEndSetting
                   ? session.SettingsForPersistenceTest().ReadValue(
                         "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U
                   : session.SettingsForPersistenceTest().ReadValue(
                         "BOARD_ROTATE", false);
    };

    session.ObserveHallMask(UINT64_C(1) << 9U);
    const auto occupiedRevision = session.Litchcraft().Revision();
    modules::ControlMutationOutcome occupiedOutcome;
    Check(session.MutateSettingJson(
              90U, settingKey, settingValue, occupiedOutcome) ==
              VIRTUAL_LITCH_OK &&
              occupiedOutcome.status ==
                  modules::ControlMutationStatus::Rejected &&
              occupiedOutcome.reason == modules::ControlMutationReason::Busy &&
              !orientationApplied() &&
              session.Litchcraft().Occupancy() == (UINT64_C(1) << 9U) &&
              session.Litchcraft().Revision() == occupiedRevision &&
              Color(session.Litchcraft().Artwork()[54U * 4U], 0U, 0U, 0U),
          "LITCHCRAFT_LIVE_ROTATION rejects orientation changes until the "
          "resting brush is lifted without painting the remapped square");
    Check(session.EdgeCueIntent().kind ==
              modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "app orientation rejection with a resting brush shows occupied feedback");
    const auto warningOff = session.SemanticFrame().board[9U][0U];
    session.Advance(200U);
    const auto warningOn = session.SemanticFrame().board[9U][0U];
    Check(warningOff.r == 0U && warningOff.g == 0U && warningOff.b == 0U &&
              warningOn.r != 0U && session.Litchcraft().Revision() == occupiedRevision,
          "resting brush follows the shared occupied-piece blink without editing artwork");
    session.ObserveHallMask(0U);
    session.Advance(400U);
    const auto before = session.PhysicalFrame();

    modules::ControlMutationOutcome outcome;
    Check(session.MutateSettingJson(
              91U, settingKey, settingValue, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::Ordinary,
          "LITCHCRAFT_LIVE_ROTATION accepts the app Rotate board setting while art owns the empty board");
    const auto after = session.PhysicalFrame();
    const size_t rotatedSquare = 63U * 4U;
    Check(session.Litchcraft().IsActive() &&
              orientationApplied() &&
              before.pixels[0U].r != 0U &&
              (playerEndSetting
                   ? after.pixels[0U].r != 0U && after.pixels[rotatedSquare].r == 0U
                   : after.pixels[0U].r == 0U && after.pixels[rotatedSquare].r != 0U),
          "LITCHCRAFT_LIVE_ROTATION only BOARD_ROTATE changes physical artwork coordinates");
    std::printf(
        "[LITCHCRAFT_LIVE_ROTATION] mode=%s key=%s setting=%u logical_square=a8 "
        "before_physical=a8 after_physical=%s accepted=%u next=continue-painting\n",
        session.Litchcraft().IsActive() ? "Litchcraft" : "Setup/Play",
        settingKey,
        orientationApplied() ? 1U : 0U,
        playerEndSetting ? "a8" : "h1",
        outcome.status == modules::ControlMutationStatus::Applied ? 1U : 0U);
}

void VirtualLiteralOrientationJourney(const bool firstP2, const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.first_player_is_p2 = firstP2;
    config.board_rotate = rotate;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    // Absolute corner fixture shared with the app's actual-widget panels.
    // These are raw V3 addresses, not derived through the rotation helper.
    constexpr std::array<uint8_t, 4> LOGICAL_CORNERS{0U, 7U, 56U, 63U};
    constexpr std::array<uint8_t, 4> TURNED_RAW_CORNERS{63U, 56U, 7U, 0U};
    constexpr std::array<modules::Rgb, 4> CORNER_COLORS{{
        {255U, 0U, 0U}, {0U, 255U, 0U}, {0U, 0U, 255U}, {255U, 255U, 0U}}};
    for (uint8_t corner = 0U; corner < 4U; ++corner) {
        const auto color = CORNER_COLORS[corner];
        const auto cornerTool = Request(Art::Op::Tool, art.SessionId(), 10U + corner * 2U,
            {0U, 15U, color.r, color.g, color.b});
        session.SendLitchcraftPacket(cornerTool.bytes.data(), cornerTool.size);
        const auto cornerPaint = Request(Art::Op::Paint, art.SessionId(), 11U + corner * 2U,
            {static_cast<uint8_t>(10U + corner), 0U, 3U, LOGICAL_CORNERS[corner]});
        session.SendLitchcraftPacket(cornerPaint.bytes.data(), cornerPaint.size);
    }
    session.Advance(250U);
    for (uint8_t corner = 0U; corner < 4U; ++corner) {
        const uint8_t raw = rotate ? TURNED_RAW_CORNERS[corner] : LOGICAL_CORNERS[corner];
        const auto actual = session.PhysicalFrame().pixels[raw * 4U];
        const auto expected = CORNER_COLORS[corner];
        Check((actual.r != 0U) == (expected.r != 0U) &&
                  (actual.g != 0U) == (expected.g != 0U) &&
                  (actual.b != 0U) == (expected.b != 0U),
              "V3 physical corner hues remain red/green far and blue/yellow near after board-pose compensation");
    }
    std::printf("[VIEWER_NEAR_EDGE] mode=Litchcraft rotate=%u first=P%u raw_near_left=%u "
                "display_near_left=a1 color=blue raw_near_right=%u color=yellow next=paint\n",
                rotate, firstP2 ? 2U : 1U, rotate ? 7U : 56U, rotate ? 0U : 63U);
    const auto tool = Request(Art::Op::Tool, art.SessionId(), 20U,
                              {0U, 1U, 255U, 0U, 0U});
    session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    const auto paint = Request(Art::Op::Paint, art.SessionId(), 21U,
                               {20U, 0U, 3U, 10U});
    session.SendLitchcraftPacket(paint.bytes.data(), paint.size);
    session.Advance(250U);
    const uint8_t physicalSquare = rotate ? 53U : 10U;
    const uint8_t physicalSlot = rotate ? 2U : 0U;
    const auto displayed = session.PhysicalFrame().pixels[physicalSquare * 4U + physicalSlot];
    Check(displayed.r != 0U && displayed.g == 0U && displayed.b == 0U,
          "literal orientation maps asymmetric app square/emitter independently of first player");
    const auto brush = Request(Art::Op::Tool, art.SessionId(), 22U,
                               {0U, 1U, 0U, 255U, 0U});
    session.SendLitchcraftPacket(brush.bytes.data(), brush.size);
    session.ObserveHallMask(UINT64_C(1) << physicalSquare);
    session.Advance(1U);
    Check(art.Occupancy() == (UINT64_C(1) << 10U) &&
              Color(art.Artwork()[40U], 0U, 255U, 0U) &&
              Color(art.Artwork()[53U * 4U], 0U, 0U, 0U),
          "raw Hall touch paints the same displayed square without first-player remapping");
    const auto touched = session.PhysicalFrame().pixels[physicalSquare * 4U + physicalSlot];
    Check(touched.r == 0U && touched.g != 0U && touched.b == 0U,
          "physical brush roundtrip updates the exact asymmetric displayed emitter");
    session.ObserveHallMask(0U);
    for (const bool down : {true, false}) {
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
                              runtime::VirtualLitchPhysicalButton::Right, down);
        session.Advance(30U);
    }
    Check(art.IsQuickPaletteVisible(), "physical right opens the palette at the same printed control bank");
    const uint8_t logicalSwatch = rotate ? 4U : 59U;
    session.ObserveHallMask(UINT64_C(1) << 59U);
    session.Advance(1U);
    Check(art.Occupancy() == (UINT64_C(1) << logicalSwatch),
          "physical palette swatch Hall mapping ignores first-player choice");
    for (uint8_t index = 0U; index < 32U; ++index) {
        const auto color = session.PhysicalFrame().pixels[256U + index];
        const bool lit = color.r || color.g || color.b;
        Check(lit == (index % 8U == 0U || index % 8U == 7U),
              "literal orientation palette retains exactly eight active-color corners");
    }
    session.ObserveHallMask(0U);
    session.Advance(300U);
    Check(!art.IsQuickPaletteVisible(), "palette lift/fade recovers normal painting after literal mapping");
    std::printf("[LITCHCRAFT_LITERAL_ORIENTATION] first=P%u rotate=%u input=Enter+app:c7-left+Hall%u "
                "mode=Litchcraft result=accepted logical=10 physical=%u slot=%u "
                "palette_raw=59 palette_logical=%u corners=8 next=continue-painting expiry=300ms\n",
                firstP2 ? 2U : 1U, rotate, physicalSquare, physicalSquare, physicalSlot, logicalSwatch);
}

void VirtualPlayEndAssignmentJourney(const bool farP1, const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.first_player_is_p2 = farP1;
    config.board_rotate = rotate;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    constexpr uint64_t opening = UINT64_C(0xffff00000000ffff);
    session.ObserveBootHallMask(opening);
    session.Advance(3000U);
    // The app presents the same raw near-left corner, but first-player
    // placement decides whether that corner contains P1's or P2's rook.
    const uint8_t rawNearLeft = rotate ? 7U : 56U;
    const auto nearRook = session.PhysicalFrame().pixels[rawNearLeft * 4U + 1U];
    Check(nearRook.b > nearRook.r,
          "Play physical near edge keeps blue for P1 normally and P2 after end swap in either hardware pose");
    std::printf("[VIEWER_NEAR_EDGE] mode=Play rotate=%u first=P%u raw_near_left=%u "
                "display_near_left=%s near_owner=P%u next=lift-P1-pawn\n",
                rotate, farP1 ? 2U : 1U, rawNearLeft, farP1 ? "h8" : "a1", farP1 ? 2U : 1U);
    const bool banksSwapped = rotate != farP1;
    const uint8_t p1Bank = banksSwapped ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    const uint8_t p1Edge = banksSwapped ? 0U : 16U;
    bool p1TurnAtAssignedEnd = false;
    for (uint8_t tick = 0U; tick < 8U; ++tick) {
        session.Advance(50U);
        for (uint8_t index = p1Edge; index < p1Edge + 8U; ++index) {
            const auto led = session.PhysicalFrame().pixels[256U + index];
            p1TurnAtAssignedEnd |= farP1 ? led.r > led.b && led.r > led.g
                                        : led.b > led.r && led.b > led.g;
        }
    }
    Check(session.State().side_to_move_black == 0U && p1TurnAtAssignedEnd,
          "logical P1 always opens and Turn uses the fixed hue at its assigned control end");
    for (const bool down : {true, false}) {
        session.SetButtonDown(p1Bank, runtime::VirtualLitchPhysicalButton::Up, down);
        session.Advance(30U);
    }
    auto& settings = session.SettingsForPersistenceTest();
    const uint32_t expectedNearWheel = farP1 ? 170U : 171U;
    const uint32_t expectedFarWheel = farP1 ? 1U : 0U;
    Check(settings.ReadValue("P1_PIECE_WHEEL", uint32_t{170U}) == expectedNearWheel &&
              settings.ReadValue("P2_PIECE_WHEEL", uint32_t{0U}) == expectedFarWheel,
          "assigned P1 raw control bank edits only the palette stored for its physical end");
    const uint8_t rawE2 = (rotate != farP1) ? 11U : 52U;
    session.ObserveHallMask(opening & ~(UINT64_C(1) << rawE2));
    Check(session.State().selected_square == 52U &&
              session.State().valid_destination_mask ==
                  ((UINT64_C(1) << 44U) | (UINT64_C(1) << 36U)),
          "selected P1 end maps the raw pawn lift to logical e2 and canonical legal destinations");
    session.ObserveHallMask(opening);
    session.Advance(2200U);
    modules::ControlMutationOutcome outcome;
    Check(session.MutateSettingJson(90U, "GAME_FIRST_PLAYER", farP1 ? "1" : "2", outcome) ==
              VIRTUAL_LITCH_OK && outcome.status == modules::ControlMutationStatus::Applied,
          "empty-handed opening allows end reassignment while preserving logical coordinates");
    session.Advance(1U);
    Check(session.KingQueenSwapActive() && session.State().side_to_move_black == 0U &&
              session.State().expected_occupied_mask == opening,
          "end reassignment requests separate K/Q correction while P1 and opening coordinates remain authoritative");
    uint64_t hall = opening;
    for (const uint8_t square : {3U, 4U, 59U, 60U}) {
        hall &= ~(UINT64_C(1) << square);
        session.ObserveHallMask(hall);
        hall |= UINT64_C(1) << square;
        session.ObserveHallMask(hall);
    }
    session.Advance(2200U);
    Check(!session.KingQueenSwapActive() && session.State().side_to_move_black == 0U &&
              settings.ReadValue("P1_PIECE_WHEEL", uint32_t{170U}) == expectedNearWheel &&
              settings.ReadValue("P2_PIECE_WHEEL", uint32_t{0U}) == expectedFarWheel,
          "K/Q correction completes with logical P1 opening and unchanged stored palettes");
    std::printf("[PLAY_END_ASSIGNMENT] selection=%s rotate=%u raw_P1_bank=%u turn_edge=%u "
                "input=Up+Hall%u+end-change+KQ mode=Play result=accepted logical=e2 "
                "opener=P1 palettes=171,0 next=first-move expiry=KQ-completed\n",
                farP1 ? "far" : "near", rotate, p1Bank, p1Edge, rawE2);
}

void VirtualQuickMenuDirectionJourney(const bool rotate, const uint8_t bank) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = rotate;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    const auto set = [&](const runtime::VirtualLitchPhysicalButton button, const bool down) {
        return session.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK &&
               session.Advance(30U) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button) {
        return set(button, true) && set(button, false);
    };
    Check(set(runtime::VirtualLitchPhysicalButton::X, true) &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              set(runtime::VirtualLitchPhysicalButton::X, false) && session.QuickSettingsActive(),
          "physical X hold opens Quick Setup at CPU difficulty");
    auto& settings = session.SettingsForPersistenceTest();
    const auto level = [&]() { return settings.ReadValue("VS_CPU_LEVEL", uint32_t{0U}); };
    Check(level() == 0U && tap(runtime::VirtualLitchPhysicalButton::Up) && level() == 1U,
          "physical Up increments Quick Setup value in either rotated state and control bank");
    Check(tap(runtime::VirtualLitchPhysicalButton::Down) && level() == 0U,
          "physical Down decrements Quick Setup value in either rotated state and control bank");
    Check(tap(runtime::VirtualLitchPhysicalButton::X) && !session.QuickSettingsActive(),
          "physical X exits Quick Setup after direction checks");
    std::printf("[QUICK_MENU_DIRECTIONS] rotate=%u raw_bank=P%u input=X-hold,Up,Down,X "
                "values=0,1,0 mode=Setup accepted=1 next=place-pieces\n", rotate, bank);
}

void BlankOffVisibilityJourney(const uint8_t hardware, const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate ? 1U : 0U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);

    const uint8_t logicalSquare = 20U;
    const uint8_t physicalSquare = rotate ? 63U - logicalSquare : logicalSquare;
    session.ObserveHallMask(UINT64_C(1) << physicalSquare);
    session.Advance(Art::PICK_ARM_MS + Art::PICK_PROGRESS_MS);
    const auto eraseCue = std::count_if(
        std::begin(session.SemanticFrame().status),
        std::end(session.SemanticFrame().status),
        [](virtual_litch_color color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    bool blankSemanticCanvas = true;
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const auto color = session.SemanticFrame().board[square][slot];
            blankSemanticCanvas = blankSemanticCanvas && color.r == 0U &&
                color.g == 0U && color.b == 0U;
        }
    }
    Check(art.IsErasing() && blankSemanticCanvas &&
              eraseCue == (hardware == 1U ? 0U : 8U),
          "sampling a blank emitter selects Erase and immediately starts its perimeter wipe");
    if (hardware == 3U) {
        const auto topology = hardware::BoardLedTopologyFor(
            static_cast<hardware::BoardRevision>(hardware));
        size_t physicalEraseCue = 0U;
        bool physicalCanvasBlank = true;
        for (size_t pixel = 0U; pixel < topology.physicalBoardLedCount; ++pixel) {
            const auto color = session.PhysicalFrame().pixels[pixel];
            physicalCanvasBlank = physicalCanvasBlank && color.r == 0U &&
                color.g == 0U && color.b == 0U && color.w == 0U;
        }
        for (uint8_t ringIndex = 0U; ringIndex < 32U; ++ringIndex) {
            const auto color = session.PhysicalFrame().pixels[
                topology.physicalBoardLedCount + ringIndex];
            if (color.r != 0U || color.g != 0U || color.b != 0U ||
                color.w != 0U) {
                ++physicalEraseCue;
            }
        }
        Check(physicalCanvasBlank && physicalEraseCue == 8U,
              "V3 blank color-pick leaves only the eight physical erase-wipe LEDs lit");
    }
    JourneyCheckpoint(session, hardware, rotate, "blank-pick-selects-erase",
                      "lift-and-erase-or-toggle-paint");
}

void VirtualIdentifyInLitchcraftJourney(const uint8_t hardware,
                                        const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate ? 1U : 0U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto enter = Request(Art::Op::Enter, 0U, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    const auto tool = Request(
        Art::Op::Tool, art.SessionId(), 2U, {0U, 15U, 34U, 195U, 226U});
    session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    const auto paint = Request(
        Art::Op::Paint, art.SessionId(), 3U, {1U, 0U, 3U, 9U});
    session.SendLitchcraftPacket(paint.bytes.data(), paint.size);
    const auto canvasHash = session.SemanticFrame().combined_hash;

    session.ShowIdentifyOverlay(7U, {55U, 215U, 105U});
    session.Advance(250U);
    size_t identifyPixels = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const auto color = session.SemanticFrame().board[square][slot];
            if (color.g > color.r && color.g > color.b) {
                ++identifyPixels;
            }
        }
    }
    Check(art.IsActive() && session.DisplayText() == "7" &&
              identifyPixels != 0U &&
              session.SemanticFrame().combined_hash != canvasHash,
          "accepted Identify visibly overlays an active Litchcraft canvas without exiting its mode");
    std::printf("[LITCHCRAFT_IDENTIFY_JOURNEY] hardware=%u rotate=%u mode=Litchcraft request=Identify display=\"%.*s\" identify_pixels=%zu accepted=%u next=wait-for-identify-expiry\n",
                hardware, rotate ? 1U : 0U,
                static_cast<int>(session.DisplayText().size()),
                session.DisplayText().data(), identifyPixels,
                art.IsActive() && identifyPixels != 0U ? 1U : 0U);
    session.StopIdentifyOverlay();
    session.Advance(250U);
    Check(art.IsActive() &&
              session.SemanticFrame().combined_hash == canvasHash,
          "Identify stop restores the same Litchcraft canvas without a mode transition");
    std::printf("[LITCHCRAFT_IDENTIFY_JOURNEY] hardware=%u rotate=%u mode=Litchcraft request=Identify-stop canvas_restored=%u accepted=%u next=continue-painting\n",
                hardware, rotate ? 1U : 0U,
                session.SemanticFrame().combined_hash == canvasHash ? 1U : 0U,
                art.IsActive() ? 1U : 0U);
}

void ErasePickRolloverJourney() {
    const uint32_t restoreMs = fxe::ManualMillis::NowMs();
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 1500U);
    Art art;
    art.Configure(3U, 953U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Send(art, Art::Op::Tool, 2U, {0U, 15U, 40U, 90U, 180U});
    Drain(art);
    art.ObserveOccupancy(UINT64_C(1) << 4U);
    fxe::ManualMillis::AdvanceMs(Art::PICK_ARM_MS + Art::PICK_PROGRESS_MS);
    art.Update();
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    art.Compose(board, status);
    const auto beforeDeadline = std::count_if(
        status.begin(), status.end(), [](modules::Rgb color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    fxe::ManualMillis::AdvanceMs(Art::ERASE_WIPE_MS);
    art.Compose(board, status);
    const auto atDeadline = std::count_if(
        status.begin(), status.end(), [](modules::Rgb color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    Check(art.IsErasing() && beforeDeadline == 8U && atDeadline == 4U,
          "black color-pick crosses millis rollover and immediately starts erase wipe then corner markers");

    fxe::ManualMillis::SetNowMs(UINT32_MAX - 150U);
    Art ordinaryErase;
    ordinaryErase.Configure(3U, 954U);
    ordinaryErase.BeginConnection();
    Send(ordinaryErase, Art::Op::Enter, 1U);
    Send(ordinaryErase, Art::Op::Tool,
         2U, {1U, 15U, 40U, 90U, 180U});
    fxe::ManualMillis::AdvanceMs(Art::ERASE_WIPE_MS);
    ordinaryErase.Compose(board, status);
    const auto settledMarkers = std::count_if(
        status.begin(), status.end(), [](modules::Rgb color) {
            return color.r == 18U && color.g == 18U && color.b == 18U;
        });
    Check(settledMarkers == 4U,
          "an ordinary Erase selection crosses millis rollover and completes its wipe on schedule");
    fxe::ManualMillis::SetNowMs(restoreMs);
}
} // namespace
void SharedLitchcraftPaletteVectors() {
    std::ifstream input("test/fixtures/litchcraft_palette_wheel_v1.csv");
    Check(input.good(), "shared Litchcraft palette fixture exists");
    std::string line;
    std::getline(input, line);
    size_t count = 0U;
    while (std::getline(input, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        unsigned index, wheel, r, g, b;
        Check(bool(row >> index >> wheel >> r >> g >> b), "palette fixture row parses");
        const auto color = modules::litchcraft_palette::Color(index);
        Check(index == count && wheel == index * 8U && Color(color, r, g, b),
              "physical palette matches exact shared app RGB fixture");
        Check(std::max({color.r, color.g, color.b}) == 255U,
              "each visible palette swatch has full RGB value");
        ++count;
    }
    Check(count == 32U, "shared wheel has exactly 32 swatches");
}

void SupportPaintGestureJourney() {
    Art art;
    trace::SupportActionHistory actions;
    art.SetSupportActions(&actions);
    art.Configure(3U, 953U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U); Drain(art);
    Send(art, Art::Op::Tool, 2U, {0U, 15U, 33U, 44U, 55U}); Drain(art);
    Send(art, Art::Op::Paint, 3U, {1U, 0U, 1U, 5U}); Drain(art);
    Check(actions.Size() == 1U && actions.At(0U).outcome == trace::SupportActionOutcome::Requested,
          "support paint begins one requested gesture, not a packet log");
    Send(art, Art::Op::Paint, 3U, {1U, 0U, 1U, 5U}); Drain(art);
    Send(art, Art::Op::Paint, 4U, {1U, 0U, 2U, 6U}); Drain(art);
    Check(actions.Size() == 1U && actions.At(0U).kind == trace::SupportActionKind::Paint &&
          actions.At(0U).outcome == trace::SupportActionOutcome::Applied && actions.At(0U).count == 2U,
          "actual protocol gesture spans packets and replay once, with two affected squares and applied outcome");
}

int main() {
    CompactModeReturnsCanonicalStatusAndAck();
    SupportPaintGestureJourney();
    ModeReceiptsSurvivePublicationResync();
    ReplacementPresenceRetainsSparseEdgesAndConnectionOwnership();
    SharedLitchcraftPaletteVectors();
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    UnitJourney();
    PhysicalModeTransitionNotifications();
    EntryChordUsesCapturedEventTime();
    ActiveModeChordUsesCapturedEventTime();
    PhysicalQuickPaletteJourney();
    VirtualQuickPaletteLiftJourney();
    FavoritePaletteTransportJourney();
    PhysicalHistoryDuringSnapshot();
    DetailedHistoryAndColorPick();
    ColorPickLiftTimingAndSourceBoundaries();
    VirtualHallLiftBlendColorToPaintJourney(false);
    VirtualHallLiftBlendColorToPaintJourney(true);
    VirtualBufferedFacePressCancelsColorPickJourney(runtime::VirtualLitchPhysicalButton::X, false);
    VirtualBufferedFacePressCancelsColorPickJourney(runtime::VirtualLitchPhysicalButton::O, false);
    VirtualBufferedFacePressCancelsColorPickJourney(runtime::VirtualLitchPhysicalButton::X, true);
    StaticDeltaHistoryRetainsTypicalWorkload();
    LiveStrokePublishesTransientHistoryCount();
    ExactDocumentColorJourney();
    AdversarialProtocol();
    BulkArtworkProtocol();
    Topologies();
    WireFixtureJourney();
    StartedClockAdmission();
    AppOccupiedEntryFeedbackJourney();
    LocalEntryOwnsConcurrentAppAdmissionJourney();
    OccupiedBoardBlocksPhysicalEntryJourney();
    VirtualLiveOrientationJourney("BOARD_ROTATE", "true", false);
    VirtualLiveOrientationJourney("GAME_FIRST_PLAYER", "2", true);
    for (const bool firstP2 : {false, true}) {
        for (const bool rotate : {false, true}) {
            VirtualLiteralOrientationJourney(firstP2, rotate);
            VirtualPlayEndAssignmentJourney(firstP2, rotate);
        }
    }
    for (const bool rotate : {false, true}) {
        VirtualQuickMenuDirectionJourney(rotate, VIRTUAL_LITCH_OWNER_P1);
        VirtualQuickMenuDirectionJourney(rotate, VIRTUAL_LITCH_OWNER_P2);
    }
    for (uint8_t hardware : {uint8_t(VIRTUAL_LITCH_HARDWARE_C3), uint8_t(VIRTUAL_LITCH_HARDWARE_S3), uint8_t(VIRTUAL_LITCH_HARDWARE_V3)}) {
        for (bool rotate : {false, true}) {
            VirtualJourney(hardware, rotate);
            VirtualColorPickAndHistoryJourney(hardware, rotate);
            BlankOffVisibilityJourney(hardware, rotate);
            VirtualIdentifyInLitchcraftJourney(hardware, rotate);
        }
    }
    ErasePickRolloverJourney();
    std::printf("[LITCHCRAFT] failures=%d sizeof_session=%zu\n", g_failures, sizeof(Art));
    return g_failures == 0 ? 0 : 1;
}
