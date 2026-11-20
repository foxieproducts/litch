#pragma once

// Only the platform evidence copy is controlled. Requests pass through the
// shipping shared-control reassembler, event drain, formatter and status owner.
class OtaProofProbe final : public modules::OtaPlatformAdapter {
  public:
    modules::OtaBootEvidence boot;
    mutable modules::OtaAdmissionEvidence admission;
    mutable unsigned bootReads = 0U;
    mutable unsigned admissionReads = 0U;
    unsigned mutations = 0U;
    bool bootCopyAvailable = true;
    bool admissionCopyAvailable = true;
    bool changeDuringCopy = false;

    OtaProofProbe() {
        boot.available = true; boot.sample = 9U; boot.capturedAtMs = 100U;
        boot.snapshot.slots = {{{0x10000U, 0x10U}, {0x340000U, 0x11U}}};
        boot.snapshot.running = boot.snapshot.selected = boot.snapshot.slots[0U];
        boot.snapshot.selectedEntry = 0U; boot.snapshot.selectedSequence = 23U;
        boot.runningState = modules::OtaPostBootRunningState::Valid;
        SetEntry(0U, 23U, 2U); SetEntry(1U, 24U, 4U);
    }
    void SetEntry(size_t index, uint32_t sequence, uint32_t state) {
        auto& raw = boot.snapshot.selectors[index]; raw.fill(0xffU);
        fxe::WriteU32LE(sequence, raw.data());
        fxe::WriteU32LE(state, raw.data() + 24U);
        fxe::WriteU32LE(fxe::Crc32(raw.data(), 4U, 0U), raw.data() + 28U);
    }
    void Receiving() {
        admission.attempt = 7U; admission.phase = modules::OtaAdmissionPhase::Receiving;
        admission.source = boot.snapshot.slots[0U];
        admission.target = boot.snapshot.slots[1U];
        admission.sourceSequence = 23U; admission.expectedSequence = 24U;
    }
    bool Begin(uint32_t) override { ++mutations; return false; }
    bool Write(const uint8_t*, size_t) override { ++mutations; return false; }
    bool End() override { ++mutations; return false; }
    void Abort() override { ++mutations; }
    bool ReadAdmissionEvidence(modules::OtaAdmissionEvidence& out) const override {
        ++admissionReads; out = admission; return admissionCopyAvailable;
    }
    bool ReadBootEvidence(modules::OtaBootEvidence& out) const override {
        ++bootReads; out = boot;
        if (changeDuringCopy) ++admission.attempt;
        return bootCopyAvailable;
    }
};

void TestCorrelatedOtaAttemptProofAtProductionBoundary() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStore settings;
    modules::ControlMutationService control;
    modules::BleService service;
    NimBLECharacteristic legacy;
    OtaProofProbe evidence;
    Check(Peer::Configure(service, settings, control) &&
              Peer::ActivateMinimumConnectionObjectSession(service),
          "attempt proof activates the actual minimum-MTU Object status stream");
    Peer::PrepareProducerGeneration(service);
    Peer::ConfigureOtaNotifications(service, legacy);
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));
    service.SetOtaPlatformAdapter(&evidence);
    auto query = [&](const char* command) {
        Check(QueueSharedStatusCommand(service, command),
              "attempt proof request enters production shared-control reassembly");
        SharedStatusRecords capture;
        for (unsigned index = 0U; index < 128U; ++index) {
            service.DrainEventQueue(); capture.Take(service);
        }
        Check(capture.intact && !capture.active && capture.records.size() == 1U,
              "attempt proof survives exact reliable minimum-MTU fragmentation");
        return capture.records.empty() ? std::string{} : capture.records[0U];
    };
    const char* request = R"({"cmd":"ota_state","proof":1,"q":1})";
    Check(query(request) ==
              R"({"op":1,"q":1,"n":"8899aabbccddeeff","v":0,"s":[9,900,0,0,0,23,2,23,2,1,24,4,1],"a":[0,0,255,255,0,0]})",
          "same-image fallback reports source seq23 VALID and target seq24 ABORTED separately");
    service.PublishOtaRuntimeAccepted();
    const auto fallback = query(request);
    Check(fallback.find(R"("v":1,"s":[9,900,0,0,0,23,2,23,2,1,24,4,1])") != std::string::npos,
          "normal acceptance of preserved source cannot erase target ABORTED evidence");
    evidence.Receiving();
    Check(query(request).find(R"("a":[7,1,0,1,23,24])") != std::string::npos,
          "Receiving binds exact adapter attempt, source slot and immutable target sequence");
    evidence.admission.phase = modules::OtaAdmissionPhase::Activated;
    evidence.SetEntry(1U, 24U, 0U);
    evidence.boot.snapshot.selected = evidence.boot.snapshot.slots[1U];
    evidence.boot.snapshot.selectedEntry = 1U;
    evidence.boot.snapshot.selectedSequence = 24U;
    evidence.boot.runningState = modules::OtaPostBootRunningState::Unknown;
    Check(query(request).find(R"("s":[9,900,0,1,1,24,0,23,2,1,24,0,1],"a":[7,2,0,1,23,24])") != std::string::npos,
          "Activated retains actual running source separately from exact SDK-selected NEW target");

    const std::string unavailable = R"({"op":1,"q":1,"e":"unavailable"})";
    evidence.SetEntry(1U, 26U, 0U); evidence.boot.snapshot.selectedSequence = 26U;
    Check(query(request) == unavailable, "higher same-target SDK sequence cannot satisfy old Activated admission");
    evidence = OtaProofProbe{}; evidence.Receiving();
    evidence.changeDuringCopy = true;
    Check(query(request) == unavailable, "admission changes across RAM copy fail closed");
    evidence = OtaProofProbe{}; evidence.bootCopyAvailable = false;
    Check(query(request) == unavailable, "contended boot cache is correlated unavailable");
    evidence = OtaProofProbe{}; evidence.admissionCopyAvailable = false;
    Check(query(request) == unavailable, "contended admission copy is correlated unavailable");
    evidence = OtaProofProbe{}; evidence.boot.available = false;
    Check(query(request) == unavailable, "unavailable latest capture cannot reuse historical VALID");
    evidence = OtaProofProbe{}; evidence.Receiving();
    evidence.SetEntry(0U, 25U, 2U); evidence.boot.snapshot.selectedSequence = 25U;
    Check(query(request) == unavailable, "stale Receiving admission cannot bind newer same-slot source");
    evidence = OtaProofProbe{}; evidence.Receiving();
    evidence.SetEntry(0U, 23U, 0U); evidence.SetEntry(1U, 23U, 2U);
    evidence.boot.runningState = modules::OtaPostBootRunningState::Unknown;
    Check(query(request) == unavailable,
          "duplicate raw VALID record cannot bless SDK-selected NEW Receiving source");
    evidence = OtaProofProbe{}; evidence.boot.snapshot.slots[0U].address = 0x20000U;
    Check(query(request) == unavailable, "subtype alone cannot corroborate actual running slot");
    evidence = OtaProofProbe{}; evidence.boot.snapshot.selectors[0U][28U] ^= 1U;
    Check(query(request) == unavailable, "selected selector CRC disagreement fails closed");
    evidence = OtaProofProbe{}; evidence.boot.runningState = modules::OtaPostBootRunningState::PendingVerify;
    Check(query(request) == unavailable, "cached running state must agree with raw selected state");
    evidence = OtaProofProbe{}; evidence.Receiving(); evidence.admission.target = evidence.admission.source;
    Check(query(request) == unavailable, "same-slot Receiving target cannot claim rolling admission");
    evidence = OtaProofProbe{}; evidence.Receiving(); evidence.admission.expectedSequence += 2U;
    Check(query(request) == unavailable, "unrelated higher target sequence is not admitted proof");
    evidence = OtaProofProbe{}; evidence.admission.attempt = 1U;
    Check(query(request) == unavailable, "partial Begin publication cannot masquerade as None");
    evidence = OtaProofProbe{};
    service.SetConnectionSessionBootNonce(0U);
    Check(query(request) == unavailable, "missing Hello nonce cannot supply proof");
    service.SetConnectionSessionBootNonce(UINT64_C(0x8899aabbccddeeff));
    service.SetOtaPlatformAdapter(nullptr);
    Check(query(request) == unavailable, "unsupported adapter supplies only correlated unavailable");
    service.SetOtaPlatformAdapter(&evidence);

    for (const char* malformed : {
        R"({"cmd":"ota_state","proof":0,"q":1})", R"({"cmd":"ota_state","proof":2,"q":1})",
        R"({"cmd":"ota_state","proof":"1","q":1})", R"({"cmd":"ota_state","proof":true,"q":1})",
        R"({"cmd":"ota_state","proof":null,"q":1})", R"({"cmd":"ota_state","proof":1})",
        R"({"cmd":"ota_state","proof":1,"q":0})", R"({"cmd":"ota_state","proof":1,"q":-1})",
        R"({"cmd":"ota_state","proof":1,"q":4294967296})", R"({"cmd":"ota_state","proof":1,"q":"1"})",
        R"({"cmd":"ota_state","proof":1,"q":1.5})", R"({"cmd":"ota_state","proof":1,"q":true})"}) {
        const auto response = query(malformed);
        Check(response.find("invalid_proof") != std::string::npos &&
                  response.find("\"ov\"") == std::string::npos,
              "malformed or unsupported proof never falls back to compact acceptance");
    }
    Check(query(R"({"cmd":"ota_state"})") == R"({"ov":"v"})",
          "request without proof preserves compact read-only compatibility response");

    // Large age is diagnostic: a stable VALID cache has no invented expiry.
    evidence.Receiving(); evidence.boot.sample = UINT32_MAX;
    evidence.admission.attempt = UINT32_MAX;
    evidence.SetEntry(0U, UINT32_MAX - 2U, 2U);
    evidence.SetEntry(1U, UINT32_MAX, UINT32_MAX);
    evidence.boot.snapshot.selectedSequence = UINT32_MAX - 2U;
    evidence.admission.sourceSequence = UINT32_MAX - 2U;
    evidence.admission.expectedSequence = UINT32_MAX - 1U;
    // Near-wrap admissions are refused by the actual adapter; maximum scalars
    // remain observable on a boot with no in-boot admission.
    evidence.admission = {};
    evidence.boot.capturedAtMs = 1001U;
    const auto maximum = query(R"({"cmd":"ota_state","proof":1,"q":4294967295})");
    Check(maximum.size() <= 196U && maximum.find("4294967295") != std::string::npos &&
              maximum.find("unavailable") == std::string::npos,
          "maximum uint32 request/sample/age/selector values fit bounded exact reply");

    // Hold the real reverse Object window full, then queue two distinct proofs.
    // The first response must remain intact and the second command must wait.
    evidence = OtaProofProbe{};
    Check(Peer::FillSharedReverseWindow(service), "proof retention test fills actual reverse grants");
    Check(QueueSharedStatusCommand(service, request),
          "first proof enters existing bounded event queue under reverse pressure");
    // This supporting helper bypasses raw envelope dequeue. Pin q1 before
    // directly reassembling q2, as production's front handoff does before the
    // next raw Object. The raw buffered-ingress journey covers that boundary.
    service.DrainEventQueue();
    Check(QueueSharedStatusCommand(service, R"({"cmd":"ota_state","proof":1,"q":2})"),
          "second proof waits behind the first pinned response");
    for (unsigned index = 0U; index < 10U; ++index) service.DrainEventQueue();
    Check(Peer::ReleaseSharedReverseWindow(service), "actual Progress releases proof reply pressure");
    SharedStatusRecords capture;
    for (unsigned index = 0U; index < 128U; ++index) {
        service.DrainEventQueue(); capture.Take(service);
    }
    Check(capture.intact && !capture.active && capture.records.size() == 2U &&
              capture.records[0U].find(R"("q":1,)") != std::string::npos &&
              capture.records[1U].find(R"("q":2,)") != std::string::npos,
          "blocked minimum-MTU proof replies retain both exact request identities in order");
    Check(evidence.mutations == 0U && legacy.NotifyCount() == 0U,
          "proof queries never mutate platform or duplicate direct GATT output");
    char tooSmall[16U];
    Check(!modules::FormatOtaAttemptProof(&evidence, {1U, 1U, true}, tooSmall, sizeof(tooSmall)) &&
              tooSmall[0U] == '\0', "undersized formatter output never publishes truncated proof");
}
