#include "bounded_hint_engine.hpp"
#include "virtual_litch_ble_adapter.hpp"

#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "foxie_embedded/millis.hpp"
#include "hardware/lis2dw12.hpp"
#include "logger.hpp"
#include "modules/orientation.hpp"
#include "runtime/square_mapping.hpp"
#include "virtual_litch_frame_checks.hpp"

namespace {

void AddBytes(JsonArray output, const std::string& bytes) {
    for (const unsigned char byte : bytes) {
        output.add(byte);
    }
}

std::string ReadBytes(const JsonArrayConst bytes) {
    if (bytes.isNull() || bytes.size() > 512U) {
        throw std::invalid_argument("characteristic bytes must fit 512 bytes");
    }
    std::string result;
    for (JsonVariantConst byte : bytes) {
        if (!byte.is<uint8_t>()) {
            throw std::invalid_argument("invalid characteristic byte");
        }
        result.push_back(static_cast<char>(byte.as<uint8_t>()));
    }
    return result;
}

int32_t ReadMotionAxis(const JsonVariantConst value) {
    if (!value.is<int32_t>() || value.as<int32_t>() < -2000 ||
        value.as<int32_t>() > 2000) {
        throw std::invalid_argument("motion axes require -2000..2000 integer mg");
    }
    return value.as<int32_t>();
}

void AddColor(JsonArray output, const virtual_litch_color color) {
    output.add(color.r);
    output.add(color.g);
    output.add(color.b);
    output.add(color.w);
}

void AddMask(JsonObject out, const char* key, const uint64_t mask) {
    char value[17] = {};
    std::snprintf(value, sizeof(value), "%016llX",
                  static_cast<unsigned long long>(mask));
    out[key] = std::string(value);
}

void Frame(runtime::VirtualLitchSession& session,
           const modules::VirtualLitchBleAdapter& ble, JsonObject out) {
    const auto& state = session.State();
    const auto& semantic = session.SemanticFrame();
    const auto& physical = session.PhysicalFrame();
    out["now_ms"] = state.now_ms;
    out["litchcraft_active"] = session.Litchcraft().IsActive();
    out["litchcraft_visible"] = session.Litchcraft().IsCanvasVisible();
    out["litchcraft_session"] = session.Litchcraft().SessionId();
    out["litchcraft_revision"] = session.Litchcraft().Revision();
    out["litchcraft_erasing"] = session.Litchcraft().IsErasing();
    out["home_sequence"] = ble.HomeSequence();
    out["display"] = std::string(session.DisplayText());
    out["pending_token"] = state.pending_action_token;
    out["phase"] = state.interaction_phase;
    out["variant"] = static_cast<uint8_t>(session.GameVariant());
    out["duck_square"] = session.DuckSquare();
    out["required_action"] = state.required_action;
    AddMask(out, "occupancy", state.physical_occupied_mask);
    AddMask(out, "expected", state.expected_occupied_mask);
    AddMask(out, "destinations", state.valid_destination_mask);
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    auto mapping = out.createNestedArray("logical_to_physical");
    auto board = out.createNestedArray("board");
    for (uint8_t square = 0U; square < 64U; ++square) {
        mapping.add(runtime::MapSquareIndex(square, rotate));
        auto slots = board.createNestedArray();
        for (uint8_t slot = 0U; slot < semantic.leds_per_square; ++slot) {
            AddColor(slots.createNestedArray(), semantic.board[square][slot]);
        }
    }
    auto pixels = out.createNestedArray("physical");
    for (uint16_t index = 0U; index < physical.led_count; ++index) {
        AddColor(pixels.createNestedArray(), physical.pixels[index]);
    }
    auto badges = out.createNestedArray("physical_coaching_badges");
    badges.add(virtual_litch_test::HasPhysicalCoachingBadge(
        physical, modules::UiOwner::P1));
    badges.add(virtual_litch_test::HasPhysicalCoachingBadge(
        physical, modules::UiOwner::P2));
}

void StoreFrame(runtime::VirtualLitchSession& session,
                modules::VirtualLitchBleAdapter& ble,
                const uint16_t index, JsonObject out) {
    // Observe the same session clock as Pump; wall time would make a
    // replay frame anchor appear expired. The guard restores the provider.
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(session.State().now_ms);
    const auto authority = ble.StoreLiveAuthority();
    out["has_artwork"] = ble.HasStoredArtwork();
    out["root_generation"] = ble.StoredGeneration();
    out["live_generation"] = authority.generation;
    out["live_revision"] = authority.revision;
    out["dirty"] = authority.dirty;
    out["read_active"] = ble.FrameReadActive();
    out["structural_pending"] = ble.StoreStructuralPending();
    out["selected_frame"] = session.Litchcraft().SelectedFrame();
    out["frame_elapsed_ms"] = session.Litchcraft().AnimationFrameElapsedMs();
    out["active"] = session.Litchcraft().IsActive();
    out["playing"] = session.Litchcraft().IsAnimationPlaying();
    if (!ble.HasStoredArtwork()) return;
    const auto& manifest = ble.StoredManifest();
    out["frame_count"] = manifest.frameCount;
    out["topology"] = manifest.topology;
    out["playback_on_entry"] = manifest.playbackOnEntry;
    out["sparkle_seed"] = manifest.sparkleSeed;
    auto favorites = out.createNestedArray("favorite_colors");
    for (const auto& color : manifest.favorites) {
        favorites.add((uint32_t(color.r) << 16U) |
                      (uint32_t(color.g) << 8U) | color.b);
    }
    modules::litchcraft_frame_store::Frame frame{};
    if (!ble.ReadCleanStoredFrame(index, frame)) return;
    out["frame_index"] = index;
    out["duration_ms"] = frame.durationMs;
    auto emitters = out.createNestedArray("emitters");
    for (const auto& emitter : frame.emitters) {
        auto item = emitters.createNestedArray();
        item.add((uint32_t(emitter.color.r) << 16U) |
                 (uint32_t(emitter.color.g) << 8U) | emitter.color.b);
        item.add(emitter.effect);
        item.add(static_cast<uint16_t>(emitter.effectDurationMs));
    }
}

void Require(const virtual_litch_result result) {
    if (result != VIRTUAL_LITCH_OK) {
        throw std::runtime_error("VirtualLitch input rejected: " +
                                 std::to_string(result));
    }
}

void Execute(const JsonObjectConst request,
             runtime::VirtualLitchSession& session,
             modules::VirtualLitchBleAdapter& ble, JsonObject response) {
    const std::string op = request["op"] | "";
    const std::string channel = request["channel"] | "";
    if (op == "connect") {
        ble.Connect(request["connected"] | false, request["mtu"] | 247U);
    } else if (op == "subscribe") {
        ble.Subscribe(channel, request["enabled"] | false);
    } else if (op == "write") {
        ble.Write(channel, ReadBytes(request["bytes"].as<JsonArrayConst>()));
    } else if (op == "read") {
        AddBytes(response.createNestedArray("bytes"), ble.Read(channel));
    } else if (op == "hall") {
        const std::string mask = request["mask"] | "";
        if (mask.size() != 16U ||
            mask.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            throw std::invalid_argument("Hall mask requires 16 hex digits");
        }
        Require(session.ObserveHallMask(std::stoull(mask, nullptr, 16)));
        ble.Pump();
    } else if (op == "load_fen") {
        const JsonString input = request["fen"].as<JsonString>();
        const std::string fen = input.isNull() ? std::string{}
            : std::string{input.c_str(), input.size()};
        if (fen.find('\0') != std::string::npos ||
            !session.LoadExternalFen(fen.c_str())) {
            throw std::invalid_argument("FEN rejected by the production loader");
        }
        ble.Pump();
    } else if (op == "button") {
        const uint8_t button = request["button"] | UINT8_MAX;
        if (button > static_cast<uint8_t>(runtime::VirtualLitchPhysicalButton::O)) {
            throw std::invalid_argument("invalid physical button");
        }
        Require(session.SetButtonDown(request["owner"] | 0U,
            static_cast<runtime::VirtualLitchPhysicalButton>(button),
            request["down"] | false));
        ble.Pump();
    } else if (op == "motion") {
        hardware::Lis2dw12Sample sample{};
        sample.milliGx = ReadMotionAxis(request["x"]);
        sample.milliGy = ReadMotionAxis(request["y"]);
        sample.milliGz = ReadMotionAxis(request["z"]);
        // Samples bypass I2C and sensor filtering; the production qualifier
        // uses the existing session clock to accept or reject the gesture.
        session.ObserveMotionSample(sample);
        ble.Pump();
    } else if (op == "advance") {
        if (!request["ms"].is<uint32_t>() ||
            request["ms"].as<uint32_t>() > 60000U) {
            throw std::invalid_argument("advance requires 0..60000 milliseconds");
        }
        uint32_t remaining = request["ms"].as<uint32_t>();
        // Tick the existing owner and transport at explicit 10 ms intervals.
        // This preserves notification pacing and held-button timing.
        do {
            const uint32_t step = remaining > 10U ? 10U : remaining;
            Require(session.Advance(step));
            ble.Pump();
            remaining -= step;
        } while (remaining != 0U);
    } else if (op == "health") {
        virtual_litch_runtime_health_fact fact{};
        virtual_litch_default_runtime_health_fact(&fact);
        fact.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BROWNOUT;
        fact.brownout_detected = request["brownout"] | false;
        Require(session.DispatchRuntimeHealthFact(fact));
        ble.Pump();
    } else if (op == "frame") {
        Frame(session, ble, response.createNestedObject("frame"));
    } else if (op == "store_worker") {
        if (!request["enabled"].is<bool>()) {
            throw std::invalid_argument("store_worker requires enabled bool");
        }
        ble.SetStoreWorkerEnabled(request["enabled"].as<bool>());
        ble.Pump();
    } else if (op == "store_ingress") {
        if (!request["enabled"].is<bool>()) {
            throw std::invalid_argument("store_ingress requires enabled bool");
        }
        ble.SetStoreIngressEnabled(request["enabled"].as<bool>());
        ble.Pump();
    } else if (op == "store_frame") {
        const uint16_t index = request["index"] | UINT16_MAX;
        if (index >= modules::litchcraft_frame_store::FRAME_COUNT) {
            throw std::invalid_argument("Store frame index is out of range");
        }
        StoreFrame(session, ble, index, response.createNestedObject("store_frame"));
    } else {
        throw std::invalid_argument("unsupported VirtualLitch envelope operation");
    }
}

} // namespace

int main(int argc, char** argv) {
    // Process isolation is mandatory: VirtualLitch's production hooks are
    // process-wide. This executable owns exactly one EngineBackedJourney.
    if (argc < 3 || argc > 5 || (argc >= 4 && std::string(argv[3]) != "v3") ||
        (argc == 5 && std::string(argv[4]) != "corrupt-store") || (std::string(argv[1]) != "1" &&
                      std::string(argv[1]) != "2") ||
        (std::string(argv[2]) != "0" && std::string(argv[2]) != "1")) {
        std::fprintf(stderr, "usage: virtual-litch-bridge FIRST_PLAYER ROTATE [v3 [corrupt-store]]\n");
        return 2;
    }
    logger::SetOutputLineCallback([](const char* line) {
        std::fprintf(stderr, "%s\n", line);
    });
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = argc >= 4 ? VIRTUAL_LITCH_HARDWARE_V3 : VIRTUAL_LITCH_HARDWARE_S3;
    config.first_player_is_p2 = std::string(argv[1]) == "2";
    config.board_rotate = std::string(argv[2]) == "1";
    config.square_rotate_for_output = modules::ComputeSquareRotate(
        config.board_rotate != 0U, config.first_player_is_p2 != 0U);
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    modules::VirtualLitchBleAdapter ble(session, static_cast<uint8_t>(config.hardware), argc == 5);
    char line[8192] = {};
    while (std::cin.getline(line, sizeof(line))) {
        // A characteristic write needs one slot per byte; every command has
        // at most five fields. Mutable line storage owns strings in place.
        StaticJsonDocument<JSON_ARRAY_SIZE(512U) + JSON_OBJECT_SIZE(5U)> request;
        // Includes the complete semantic and mapped RGBW frame on 64-bit native.
        DynamicJsonDocument response(128U * 1024U);
        const auto error = deserializeJson(request, line);
        response["id"] = request["id"] | 0U;
        try {
            if (error) throw std::invalid_argument(error.c_str());
            Execute(request.as<JsonObjectConst>(), session, ble,
                    response.to<JsonObject>());
            response["ok"] = true;
        } catch (const std::exception& failure) {
            response["ok"] = false;
            response["error"] = std::string(failure.what());
        }
        response["id"] = request["id"] | 0U;
        response["disconnect_requests"] = ble.DisconnectRequests();
        response["disconnect_handle"] = ble.DisconnectHandle();
        response["native_epoch"] = ble.NativeEpoch();
        response["session_activated"] = ble.SessionActivated();
        auto packets = response.createNestedArray("packets");
        for (const auto& packet : ble.TakePackets()) {
            auto out = packets.createNestedObject();
            out["channel"] = packet.channel;
            out["epoch"] = packet.connectionEpoch;
            AddBytes(out.createNestedArray("bytes"), packet.bytes);
        }
        if (response.overflowed()) {
            response.clear();
            response["id"] = request["id"] | 0U;
            response["ok"] = false;
            response["error"] = "VirtualLitch response exceeded 128 KiB";
            response.createNestedArray("packets");
        }
        std::string output;
        serializeJson(response, output);
        std::cout << output << '\n' << std::flush;
    }
    return std::cin.eof() ? 0 : 4;
}
