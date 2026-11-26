#pragma once

#include "bounded_hint_engine.hpp"
#include "virtual_litch_session.hpp"
#include "modules/ble_identify_presentation.hpp"
#include "android_pairing_consent.hpp"

namespace {
class OwnerPreviewPersistence final : public modules::BoardOwnerPersistence {
  public:
    bool release = true;
    uint64_t repairLease = 0U;
    unsigned repairs = 0U;
    bool RequestIdentityRepair(uint64_t lease) override {
        if (repairLease == lease) return false;
        repairLease = lease;
        ++repairs;
        return true;
    }
    bool SaveAddingJournal(const modules::BoardOwnerAddBaseline& value, Receipt& receipt) override {
        baseline = value;
        return value.Valid() && SaveOwnerRecord(modules::BoardOwnerRecord::Adding, receipt);
    }
    modules::BoardOwnerAddBaseline AddingBaseline() const override { return baseline; }
    bool SaveOwnerRecord(modules::BoardOwnerRecord record, Receipt& receipt) override {
        written = record; receipt = ++next; outcome = Outcome::Pending; return true;
    }
    Outcome GetOutcome(Receipt receipt) const override { return receipt == next ? outcome : Outcome::Unknown; }
    bool Release(Receipt receipt) override { return release && receipt == next && outcome != Outcome::Pending; }
    Receipt next = 0U;
    Outcome outcome = Outcome::Unknown;
    modules::BoardOwnerRecord written = modules::BoardOwnerRecord::Invalid;
    modules::BoardOwnerAddBaseline baseline{};
};

void TestOwnerPreviewSharedConsentAndNativeLease(bool phoneInitiated = false) {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Controller = modules::BoardOwnerPairingController;
    using Record = modules::BoardOwnerRecord;
    namespace wire = modules::connection_session_codec;
    owner_native_fixture::Reset();
    NimBLEDevice::ResetSecurityRequests();
    Harness harness;
    modules::BleService service;
    OwnerPreviewPersistence persistence;
    modules::BoardOwnerPairingRuntime runtime(persistence);
    modules::BoardIdentityPersistence identity;
    StaticJsonDocument<256> identityDocument;
    Check(identity.Prepare(identityDocument, [](void*, uint8_t* out, size_t size) {
        std::fill_n(out, size, uint8_t{0x59U}); return true;
    }, nullptr), "owner preview identity prepares through production owner");
    identity.OnDurableSettingsSnapshot(identityDocument);
    runtime.Boot(Record::Absent, {true, 0U}, true);
    Check(Peer::Configure(service, harness.settings, harness.controlMutation), "owner preview queues configure");
    Peer::ConfigureOwnerPreview(service, runtime, identity);
    service.OnConnected(7U, "owner-preview", 23U, 1U);
    service.DrainEventQueue();
    const uint32_t epoch = Peer::ProducerEpoch(service);
    Check(Peer::ActivateMinimumConnectionObjectSession(service, epoch), "owner preview activates real minimum-MTU session");
    service.UpdateOwnerPairing();
    Check(!Peer::ProductAccess(service) && !service.NativePairingAllowed(7U),
          "unowned session bootstrap grants neither product access nor native pairing");
    runtime.SyncLocalClearContext(true, 0U); service.UpdateOwnerPairing();
    Check(Peer::OwnerWireState(service) == modules::owner_pairing_session_wire::State::Unpaired &&
              runtime.Presentation().cue == modules::BoardOwnerPairingRuntime::PhysicalCue::Clear,
          "State40 keeps idle already-clear Bluetooth pairable for the app's normal Begin gate");
    runtime.ObserveButton({0U, true, true, fxe::Millis()}); service.UpdateOwnerPairing();
    Check(Peer::OwnerWireState(service) == modules::owner_pairing_session_wire::State::ClearPrompt,
          "State40 exposes exclusive local confirmation while already-clear input is held");
    runtime.ObserveButton({0U, true, false, fxe::Millis()}); service.UpdateOwnerPairing();
    Check(Peer::OwnerWireState(service) == modules::owner_pairing_session_wire::State::Unpaired,
          "releasing local already-clear input restores normal phone pairing admission");
    std::array<uint8_t, 20U> packet{};
    size_t packetSize = 0U;
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
        Peer::ConnectionToken(service), 2U}, {open.data(), open.size()}},
        {packet.data(), packet.size()}, packetSize) == wire::Error::None, "owner preview opens shared Ordered lane");
    service.OnConnectionSessionWrite(packet.data(), packetSize); service.DrainEventQueue();
    auto command = [&](uint32_t sequence, uint8_t action, uint16_t attempt) {
        const std::array<uint8_t, 5U> body{{1U, action, uint8_t(attempt), uint8_t(attempt >> 8U), 0U}};
        Check(wire::Codec::Encode({{wire::Kind::Ordered, 27U, Peer::OrderedToken(service), sequence},
            {body.data(), body.size()}}, {packet.data(), packet.size()}, packetSize) == wire::Error::None,
            "owner action encodes through canonical shared session");
        service.OnConnectionSessionWrite(packet.data(), packetSize); service.DrainEventQueue();
    };
    command(1U, phoneInitiated ? 3U : 1U, 31U);
    Check(runtime.Custody().GetPhase() == Controller::Phase::Armed && !service.NativePairingAllowed(7U),
          "Ordered Begin acknowledges its armed prompt without starting native security");
    command(2U, 2U, 31U);
    Check(runtime.Custody().Pairable(), "immediate ordered Cancel follows admitted Begin at next position");
    command(3U, phoneInitiated ? 3U : 1U, 32U);
    runtime.ObserveButton({0U, true, true, 0U});
    runtime.ObserveButton({0U, false, true, 0U});
    service.UpdateOwnerPairing();
    const auto connection = Peer::OwnerConnection(service);
    const auto effect = runtime.Custody().PendingEffect();
    Check(effect.type == Controller::EffectType::RequestSecurity && service.NativePairingAllowed(7U) &&
              !service.NativePairingAllowed(8U), "fresh physical consent leases only exact native connection");
    Check(NimBLEDevice::SecurityRequests() == (phoneInitiated ? 0U : 1U) &&
              ((Peer::OwnerWireFlags(service) & modules::owner_pairing_session_wire::NativeBondAllowed) != 0U) == phoneInitiated,
          "first phone chooses exact post-consent native initiation mode");
    const auto android = android_pairing_consent::Replay(NimBLEDevice::SecurityRequests());
    Check(android.securityConsent == (phoneInitiated ? 0U : 1U) && android.scConsent == 1U,
          "first phone retains SC consent while phone initiation removes Security Request consent");
    modules::BleOwnerSecurityEvent security{};
    security.connHandle = 7U; security.connectionEpoch = epoch;
    security.admissionToken = effect.token; security.sessionToken = connection.sessionToken;
    security.nativeStatus = 0; security.inventoryAvailable = true; security.peerCount = 1U;
    security.encrypted = true; security.bonded = true; security.peerMatches = true;
    auto stale = security; --stale.connectionEpoch;
    service.OnOwnerSecurity(stale); service.DrainEventQueue();
    Check(runtime.Custody().GetPhase() == Controller::Phase::AwaitSecurity,
          "queued stale native epoch cannot acknowledge a reused handle");
    service.OnOwnerSecurity(security); service.DrainEventQueue();
    Check(persistence.written == Record::Active && !Peer::ProductAccess(service) &&
              runtime.Custody().GetPhase() == Controller::Phase::SavingOwner,
          "raw native success requires durable Active receipt before product access");
    persistence.outcome = modules::BoardOwnerPersistence::Outcome::Saved;
    runtime.UpdateDurableState(Record::Active, true);
    service.UpdateOwnerPairing();
    Check(Peer::ProductAccess(service) && !service.NativePairingAllowed(7U),
          "exact durable owner admits product traffic and revokes first-claim pairing lease");
    service.OnDisconnected(8U, 19);
    Check(Peer::ProductAccess(service), "another native handle cannot retire live owner connection");
    service.OnConnected(9U, "replacement", 23U, 1U);
    Check(!Peer::ProductAccess(service) && !service.NativePairingAllowed(7U),
          "replacement callback revokes access and admission before cooperative projection");
    service.OnOwnerSecurity(security); service.DrainEventQueue();
    Check(!Peer::ProductAccess(service), "stale encryption event cannot promote replacement connection");
    std::puts("[BLE_OWNER_PREVIEW] shared-consent=pass native-lease=pass durable-gate=pass stale-event=pass");
}
}

namespace {
struct OwnerPreviewFixture {
    Harness harness;
    OwnerPreviewPersistence persistence;
    modules::BoardOwnerPairingRuntime runtime{persistence};
    modules::BoardIdentityPersistence identity;
    modules::BleService service;
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    explicit OwnerPreviewFixture(modules::BoardOwnerRecord owner = modules::BoardOwnerRecord::Absent,
                                 bool durableIdentity = true) {
        owner_native_fixture::Reset();
        NimBLEDevice::ResetSecurityRequests();
        StaticJsonDocument<256> document;
        if (!durableIdentity) {
            modules::BoardIdentity existing;
            existing.bytes.fill(0x47U);
            Check(modules::WriteBoardIdentity(document, existing), "recovery boundary retains an installed identity");
        }
        Check(identity.Prepare(document, [](void*, uint8_t* bytes, size_t size) {
            std::fill_n(bytes, size, uint8_t{0x47U}); return true;
        }, nullptr), "boundary fixture prepares actual durable identity");
        if (durableIdentity) identity.OnDurableSettingsSnapshot(document);
        runtime.Boot(owner, {true, uint8_t(owner == modules::BoardOwnerRecord::Active ? 1U : 0U)}, durableIdentity);
        Check(Peer::Configure(service, harness.settings, harness.controlMutation), "boundary fixture configures service");
        Peer::ConfigureOwnerPreview(service, runtime, identity);
        service.OnConnected(7U, "owner-boundary", 23U, 1U); service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(service, Peer::ProducerEpoch(service)),
              "boundary fixture activates canonical shared session");
        service.UpdateOwnerPairing();
        // Finish native evidence work before tests assume an empty ingress queue.
        service.DrainEventQueue(); service.UpdateOwnerPairing();
    }
    modules::BleOwnerSecurityEvent Security(bool consent = true) {
        const auto connection = Peer::OwnerConnection(service);
        if (consent) {
            Check(runtime.Begin({modules::owner_pairing_session_wire::Action::Begin, 73U, 0U}, connection),
                  "boundary fixture opens exact claim");
            runtime.ObserveButton({0U, true, true, 0U}); runtime.ObserveButton({0U, false, true, 0U});
            service.UpdateOwnerPairing();
        }
        modules::BleOwnerSecurityEvent event{};
        event.connHandle = connection.handle; event.connectionEpoch = connection.epoch;
        event.sessionToken = connection.sessionToken;
        event.admissionToken = consent ? runtime.Custody().PendingEffect().token : 0U;
        event.nativeStatus = 0; event.inventoryAvailable = true; event.peerCount = 1U;
        event.encrypted = true; event.bonded = true; event.peerMatches = true;
        return event;
    }
};

void TestPairedBluetoothIdentitySuffix() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
    const uint8_t publicAddress[6] = {0x81U, 0x3fU, 0x52U, 0xa6U, 0x10U, 0x24U};
    NimBLEDevice::SetTestAddress(publicAddress, true);
    constexpr const char* query = R"({"cmd":"info","bs":1})";
    const auto generation = Peer::ProducerEpoch(fixture.service);
    namespace wire = modules::connection_session_codec;
    std::array<uint8_t, 10U> fragment{{1U, 1U, 0U, 0U}};
    fxe::WriteU16LE(static_cast<uint16_t>(std::strlen(query)), fragment.data() + 2U);
    std::copy_n(query, 6U, fragment.begin() + 4U);
    std::array<uint8_t, 20U> packet{}; size_t packetSize = 0U;
    Check(wire::Codec::Encode({{wire::Kind::Object, modules::ConnectionSession::OTA_CONTROL_OBJECT_OPCODE,
              Peer::ObjectToken(fixture.service), 1U}, {fragment.data(), fragment.size()}},
              {packet.data(), packet.size()}, packetSize) == wire::Error::None,
          "pre-owner selector has a valid shared Object frame");
    fixture.service.OnConnectionSessionWrite(packet.data(), packetSize);
    fixture.service.DrainEventQueue();
    Check(NimBLEDevice::AddressReads() == 0U,
          "raw Object admission denies the pre-owner selector without reading identity");
    Check(QueueSharedStatusCommand(fixture.service, query, generation, true),
          "unpaired suffix query reaches the shared ingress fence");
    fixture.service.DrainEventQueue();
    SharedStatusRecords before;
    for (unsigned i = 0U; i < 16U; ++i) before.Take(fixture.service);
    bool leaked = false;
    for (const auto& record : before.records) leaked |= record.find("\"bs\"") != std::string::npos;
    Check(NimBLEDevice::AddressReads() == 0U && !leaked,
          "pre-owner Identify-only session neither reads nor emits local identity");
    fixture.service.OnOwnerSecurity(fixture.Security(false));
    fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
    Check(Peer::ProductAccess(fixture.service), "suffix requires actual paired owner access");
    Check(QueueSharedStatusCommand(fixture.service, R"({"cmd":"info"})", generation, true),
          "ordinary info still queues for the paired owner");
    fixture.service.DrainEventQueue();
    SharedStatusRecords plain;
    for (unsigned i = 0U; i < 16U; ++i) plain.Take(fixture.service);
    Check(plain.intact && !plain.records.empty() &&
              std::all_of(plain.records.begin(), plain.records.end(), [](const std::string& record) {
                  return record == R"({"i":[3,0,575]})";
              }) && NimBLEDevice::AddressReads() == 0U,
          "ordinary info remains byte-for-byte unchanged and never reads local identity");
    Peer::HandleOtaPacket(fixture.service, query, std::strlen(query));
    Check(NimBLEDevice::AddressReads() == 0U, "legacy direct ingress never exposes suffix");
    SharedStatusRecords direct;
    for (unsigned i = 0U; i < 16U; ++i) direct.Take(fixture.service);
    for (const auto& record : direct.records)
        Check(record.find("\"bs\"") == std::string::npos, "legacy response contains only public info");
    const auto run = [&](const uint8_t* address, bool isPublic, const char* expected) {
        NimBLEDevice::SetTestAddress(address, isPublic);
        Check(QueueSharedStatusCommand(fixture.service, query, generation, true), "paired suffix query queues");
        fixture.service.DrainEventQueue();
        SharedStatusRecords records;
        for (unsigned i = 0U; i < 32U; ++i) records.Take(fixture.service);
        Check(records.intact && records.records.size() == 1U && records.records[0] == expected &&
                  NimBLEDevice::AddressReads() == 1U,
              "paired suffix returns one bounded status from the selected local identity");
    };
    run(publicAddress, true, R"({"bs":"3F81"})");
    run(publicAddress, false, R"({"bs":null})");
    const uint8_t absent[6]{};
    run(absent, true, R"({"bs":null})");
    {
        modules::SettingsStore settings;
        modules::ControlMutationService control;
        modules::BleService legacy;
        Check(Peer::Configure(legacy, settings, control) &&
                  Peer::ActivateMinimumConnectionObjectSession(legacy),
              "legacy non-pairing fixture opens the shared Object channel");
        Peer::PrepareProducerGeneration(legacy);
        NimBLEDevice::SetTestAddress(publicAddress, true);
        Check(QueueSharedStatusCommand(legacy, query, 7U, true), "legacy shared selector queues");
        legacy.DrainEventQueue();
        SharedStatusRecords records;
        for (unsigned i = 0U; i < 32U; ++i) records.Take(legacy);
        Check(records.intact && records.records.size() == 1U &&
                  records.records[0].find("{\"i\":[") == 0U && NimBLEDevice::AddressReads() == 0U,
              "non-pairing profile returns unchanged public info without reading local identity");
    }
}

void TestOwnerStoreDiagnosticCountsAreReadOnlyAndChecked() {
    owner_native_fixture::Reset();
    modules::BleService cold;
    Check(!cold.ReadOwnerStoreDiagnostics().available && owner_native_fixture::inventoryReads == 0U &&
              owner_native_fixture::snapshotReads == 0U,
          "store diagnostics cannot initialize a cold native owner");
    OwnerPreviewFixture fixture;
    OwnerPreviewFixture::Peer::MarkInitialized(fixture.service);
    owner_native_fixture::inventoryResult = 0;
    owner_native_fixture::peerCount = 2;
    owner_native_fixture::peers[0U].type = BLE_ADDR_PUBLIC_ID;
    owner_native_fixture::peers[0U].val[0U] = 1U;
    owner_native_fixture::peers[1U].type = BLE_ADDR_RANDOM_ID;
    owner_native_fixture::peers[1U].val[0U] = 2U;
    owner_native_fixture::rpaCount = 3;
    for (uint8_t i = 0U; i < 3U; ++i) {
        auto& rpa = owner_native_fixture::rpas[i];
        rpa.peer_rpa_addr.type = BLE_ADDR_RANDOM;
        rpa.peer_rpa_addr.val[0U] = 81U + i;
        rpa.peer_addr.type = i == 1U ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        rpa.peer_addr.val[0U] = i == 2U ? 99U : i + 1U;
    }
    const auto originalPeers = owner_native_fixture::peers;
    const auto originalRpas = owner_native_fixture::rpas;
    const auto saves = fixture.persistence.next;
    const auto securityRequests = NimBLEDevice::SecurityRequests();
    const auto counts = fixture.service.ReadOwnerStoreDiagnostics();
    Check(counts.available && counts.savedPeers == 2U && counts.rpas == 3U && counts.orphanRpas == 1U,
          "fresh checked tables expose two saved peers and a full mapping table with one orphan");
    Check(std::memcmp(originalPeers.data(), owner_native_fixture::peers.data(), sizeof originalPeers) == 0 &&
              std::memcmp(originalRpas.data(), owner_native_fixture::rpas.data(), sizeof originalRpas) == 0 &&
              fixture.persistence.next == saves && NimBLEDevice::SecurityRequests() == securityRequests,
          "diagnostic counting changes no raw identities keys persistence or security authority");
    owner_native_fixture::rpas[2U].peer_addr = owner_native_fixture::peers[0U];
    const auto duplicateMapping = fixture.service.ReadOwnerStoreDiagnostics();
    Check(duplicateMapping.available && duplicateMapping.rpas == 3U && duplicateMapping.orphanRpas == 0U,
          "canonical identity aliases match and duplicate saved-peer mappings are not orphaned");
    owner_native_fixture::snapshotResult = 28;
    Check(!fixture.service.ReadOwnerStoreDiagnostics().available,
          "unavailable RPA inventory cannot be reported as empty or complete");
    owner_native_fixture::snapshotResult = 0;
    owner_native_fixture::rpaCount = 4;
    Check(!fixture.service.ReadOwnerStoreDiagnostics().available,
          "out-of-range mapping inventory is unavailable");
    owner_native_fixture::rpaCount = 0;
    owner_native_fixture::inventoryResult = 28;
    Check(!fixture.service.ReadOwnerStoreDiagnostics().available,
          "unavailable security inventory cannot borrow a complete mapping read");
    owner_native_fixture::inventoryResult = 0;
    owner_native_fixture::peerCount = 0;
    const auto empty = fixture.service.ReadOwnerStoreDiagnostics();
    Check(empty.available && empty.savedPeers == 0U && empty.rpas == 0U && empty.orphanRpas == 0U,
          "checked empty native tables remain distinguishable from unavailable");
    OwnerPreviewFixture::Peer::ShutdownLifetime(fixture.service);
    const auto inventoryReads = owner_native_fixture::inventoryReads;
    const auto snapshotReads = owner_native_fixture::snapshotReads;
    Check(OwnerPreviewFixture::Peer::OwnerWasBooted(fixture.service) &&
              !fixture.service.ReadOwnerStoreDiagnostics().available &&
              owner_native_fixture::inventoryReads == inventoryReads &&
              owner_native_fixture::snapshotReads == snapshotReads,
          "a previously booted but stopped owner cannot access deinitialized native tables");
    std::puts("[BLE_OWNER_STORE_COUNTS] saved2-rpa3-orphan1=pass aliases=pass read-only=pass unavailable=pass empty=pass stopped=pass");
}

void TestOwnerPreviewRetirementQueueAndQuietReconnect() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Record = modules::BoardOwnerRecord;
    using Phase = modules::BoardOwnerPairingController::Phase;
    for (const auto record : {Record::Absent, Record::Adding, Record::Clearing, Record::Invalid}) {
        OwnerPreviewFixture fixture(record, false);
        fixture.service.OnOwnerSecurity(fixture.Security(false));
        fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(fixture.persistence.repairs == 0U && !Peer::ProductAccess(fixture.service),
              "new or transitional custody cannot borrow saved-owner identity repair admission");
    }
    for (unsigned scenario = 0U; scenario < 5U; ++scenario) {
        OwnerPreviewFixture fixture(Record::Active, false);
        Check(!Peer::ProductAccess(fixture.service) &&
                  Peer::OwnerWireFlags(fixture.service) == modules::owner_pairing_session_wire::OwnerRetained,
              "unpromoted identity reaches actual State40 without product authority");
        auto event = fixture.Security(false);
        if (scenario == 1U) --event.connectionEpoch;
        if (scenario == 2U) event.bonded = false;
        if (scenario == 3U) event.peerMatches = false;
        if (scenario == 4U) ++event.connHandle;
        fixture.service.OnOwnerSecurity(event);
        fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(fixture.persistence.repairs == (scenario == 0U ? 1U : 0U) &&
                  !Peer::ProductAccess(fixture.service),
              "only current native saved-owner proof reaches persistence repair; no proof admits product early");
        if (scenario != 0U) continue;
        const auto connection = Peer::OwnerConnection(fixture.service);
        Check(fixture.persistence.repairLease ==
                  ((static_cast<uint64_t>(connection.epoch) << 32U) | connection.sessionToken),
              "repair is addressed to the activated native epoch and session");
        fixture.service.UpdateOwnerPairing(); fixture.service.UpdateOwnerPairing();
        Check(fixture.persistence.repairs == 1U, "ordinary native polls cannot duplicate repair admission");
        StaticJsonDocument<256> promoted;
        modules::BoardIdentity sameIdentity; sameIdentity.bytes.fill(0x47U);
        Check(modules::WriteBoardIdentity(promoted, sameIdentity), "boundary models exact durable callback only");
        fixture.identity.OnDurableSettingsSnapshot(promoted);
        fixture.runtime.UpdateDurableState(Record::Active, fixture.identity.IsDurable());
        fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && Peer::OwnerWireReady(fixture.service),
              "existing verified native owner becomes Ready only after matching durable identity publication");
    }
    std::puts("OWNER_RECOVERY_NATIVE saved_peer=1 old_epoch=refused foreign=refused unbonded=refused early_ready=0");
    {
        OwnerPreviewFixture fixture;
        const auto event = fixture.Security();
        const auto connection = Peer::OwnerConnection(fixture.service);
        Check(fixture.runtime.Cancel({modules::owner_pairing_session_wire::Action::Cancel, 73U, 0U}, connection),
              "claim cancellation withdraws security lease after physical consent");
        fixture.service.UpdateOwnerPairing();
        fixture.service.OnOwnerSecurity(event); fixture.service.DrainEventQueue();
        Check(fixture.persistence.written != Record::Active && !Peer::ProductAccess(fixture.service),
              "raw native success after lease withdrawal never persists ownership or Ready");
    }
    {
        OwnerPreviewFixture fixture;
        const auto event = fixture.Security();
        Check(Peer::FillOwnerPriority(fixture.service), "real priority queue reaches bounded capacity");
        fixture.service.OnOwnerSecurity(event);
        Check(!fixture.service.NativePairingAllowed(7U) && !Peer::ProductAccess(fixture.service),
              "lost security completion revokes native lease immediately");
        fixture.service.UpdateOwnerPairing(); fixture.service.DrainEventQueue();
        Check(fixture.persistence.written != Record::Active,
              "queue refusal cannot become a successful native claim");
    }
    {
        OwnerPreviewFixture fixture(Record::Active);
        Check(fixture.runtime.Status(Peer::OwnerConnection(fixture.service)).state ==
                  modules::owner_pairing_session_wire::State::OwnedOther && !Peer::ProductAccess(fixture.service),
              "owned reconnect stays locked before checked native encryption");
        auto event = fixture.Security(false);
        NimBLEService settings;
        Check(Peer::OwnerSettingsCharacteristics(fixture.service, settings), "owned Settings callbacks configure");
        auto* readWrite = settings.CharacteristicAt(1U);
        NimBLEConnInfo owner(23U, 7U), intruder(23U, 8U);
        fixture.service.OnOwnerSecurity(event);
        // The SDK restores bonded CCCDs immediately after raw ENC_CHANGE,
        // before the cooperative loop has consumed native custody evidence.
        readWrite->Callbacks()->onSubscribe(readWrite, owner, 1U);
        fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && fixture.runtime.Status(Peer::OwnerConnection(fixture.service)).state ==
                  modules::owner_pairing_session_wire::State::OwnerReady,
              "quiet reconnect reaches Ready only with raw success encrypted sole matching bond");
        Check(!fixture.service.OwnerAcceptsNewConnection(), "live owner refuses secondary native connection");
        readWrite->Callbacks()->onSubscribe(readWrite, intruder, 2U);
        const std::string request = "{\"k\":\"HINT_THINKING_STYLE\",\"i\":101}";
        readWrite->setValue(reinterpret_cast<const uint8_t*>(request.data()), request.size());
        readWrite->Callbacks()->onRead(readWrite, intruder);
        Check(readWrite->getValue() == "{}", "secondary link cannot read owner's cached Settings request");
        readWrite->setValue(reinterpret_cast<const uint8_t*>(request.data()), request.size());
        readWrite->Callbacks()->onWrite(readWrite, owner);
        fixture.service.DrainEventQueue();
        Check(readWrite->NotifyCount() != 0U && readWrite->LastConnHandle() == 7U && !readWrite->LastWasIndication(),
              "Settings response targets admitted owner handle and rejected subscriber cannot change delivery");
        event.nativeStatus = 5;
        fixture.service.OnOwnerSecurity(event);
        Check(!Peer::ProductAccess(fixture.service), "raw failure revokes access before main event dispatch");
        fixture.service.UpdateOwnerPairing();
        Check(!Peer::ProductAccess(fixture.service) && !Peer::OwnerWireReady(fixture.service),
              "update before raw failure dispatch cannot reuse success or publish Ready");
        fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(!Peer::ProductAccess(fixture.service), "retained native encrypted flag cannot override raw failure");
    }
    {
        OwnerPreviewFixture fixture(Record::Active);
        auto event = fixture.Security(false);
        fixture.service.OnOwnerSecurity(event); fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service), "owned queue-refusal fixture starts Ready");
        Check(Peer::FillOwnerPriority(fixture.service), "owned connection fills real priority queue");
        event.nativeStatus = 5;
        fixture.service.OnOwnerSecurity(event); fixture.service.UpdateOwnerPairing();
        Check(!Peer::ProductAccess(fixture.service) && !Peer::OwnerWireReady(fixture.service),
              "owned queue refusal latches failed epoch against cached Ready");
        fixture.service.DrainEventQueue();
        event.nativeStatus = 0;
        fixture.service.OnOwnerSecurity(event); fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(!Peer::ProductAccess(fixture.service), "same failed native epoch stays locked despite later success flag");
    }
    {
        OwnerPreviewFixture fixture;
        NimBLEService settings;
        Check(Peer::OwnerSettingsCharacteristics(fixture.service, settings), "preclaim actual Settings characteristics create");
        NimBLEConnInfo connection;
        auto* enumerate = settings.CharacteristicAt(0U);
        enumerate->Callbacks()->onRead(enumerate, connection);
        Check(enumerate->getValue() == "[]", "preclaim native Settings read reveals no descriptors or values");
        auto* readWrite = settings.CharacteristicAt(1U);
        const std::string request = "{\"k\":\"HINT_THINKING_STYLE\",\"v\":\"foxie_sweep\"}";
        readWrite->setValue(reinterpret_cast<const uint8_t*>(request.data()), request.size());
        const uint32_t revision = fixture.harness.settings.GetRevision();
        readWrite->Callbacks()->onWrite(readWrite, connection);
        readWrite->Callbacks()->onRead(readWrite, connection);
        Check(readWrite->getValue() == "{}", "preclaim RW read cannot reveal retained write body");
        fixture.service.OnCommandCharacteristicWrite("settings clear\n", 15U);
        const std::array<uint8_t, 8U> craft{{0x4cU, 1U, 0U, 0U, 0U, 0U, 1U, 0U}};
        fixture.service.OnLitchcraftWrite(craft.data(), craft.size());
        fixture.service.DrainEventQueue();
        char ch = 0;
        Check(fixture.harness.settings.GetRevision() == revision && !fixture.service.TryReadConsoleInputChar(ch),
              "preclaim legacy Settings Console and Craft ingress cannot mutate product state");
    }
    std::puts("[BLE_OWNER_BOUNDARIES] withdrawn-lease=pass queue-refusal=pass quiet-reconnect=pass legacy-denied=pass");
}

void TestForeignOwnerRefusalReachesSharedSessionBeforeDisconnect() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
    NimBLECharacteristic egress;
    Peer::ConfigureConnectionSessionNotifications(fixture.service, egress);
    Peer::SubscribeDiagnosticState(fixture.service);
    struct Capture { NimBLECharacteristic* output; bool refused = false; } capture{&egress};
    egress.SetNotifyHook([](void* context) {
        auto& result = *static_cast<Capture*>(context);
        const auto& bytes = result.output->LastTx();
        wire::PacketView packet;
        if (wire::Codec::Decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, packet, 20U) ==
                wire::Error::None && packet.header.kind == wire::Kind::State && packet.header.code == 40U) {
            modules::owner_pairing_session_wire::Status status{};
            if (modules::owner_pairing_session_wire::DecodeStatus(packet.body.Data(), packet.body.Size(), status))
                result.refused = status.state == modules::owner_pairing_session_wire::State::OwnedOther &&
                    status.reason == modules::owner_pairing_session_wire::Reason::WrongConnection &&
                    (status.flags & modules::owner_pairing_session_wire::ConnectionOwnerReady) == 0U;
        }
    }, &capture);
    const auto disconnects = NimBLEDevice::getServer()->DisconnectCount();
    owner_native_fixture::inventoryResult = 0; owner_native_fixture::peerCount = 1;
    owner_native_fixture::peers[0U].val[0U] = 0x31U;
    owner_native_fixture::connectionResult = 0;
    owner_native_fixture::connection.peer_id_addr.val[0U] = 0x71U;
    owner_native_fixture::connection.peer_ota_addr = owner_native_fixture::connection.peer_id_addr;
    fixture.service.OnNativeEncryptionChange(7U, 0);
    fixture.service.UpdateOwnerPairing(); // Exercise the pre-dispatch failure race.
    fixture.service.DrainEventQueue();
    for (unsigned pass = 0U; pass < 8U && !capture.refused; ++pass) Peer::UpdateSharedStates(fixture.service);
    Check(capture.refused && !Peer::ProductAccess(fixture.service) &&
              fixture.service.OwnerCallbackAllowed(7U, true) &&
              NimBLEDevice::getServer()->DisconnectCount() == disconnects,
          "proven foreign peer receives terminal State40 through real shared-session egress before ordinary app disconnect");
}

void TestNativePeerIdentityAdmissionAndRecovery() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using State = modules::owner_pairing_session_wire::State;
    using Reason = modules::owner_pairing_session_wire::Reason;
    enum class Evidence { Public, Static, Resolved, Rpa, Nrpa, NoConnection, NoInventory, MultiplePeers, Owner, OwnerResolved };
    for (const auto evidence : {Evidence::Public, Evidence::Static, Evidence::Resolved,
             Evidence::Rpa, Evidence::Nrpa, Evidence::NoConnection, Evidence::NoInventory,
             Evidence::MultiplePeers, Evidence::Owner, Evidence::OwnerResolved}) {
        OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
        owner_native_fixture::inventoryResult = 0; owner_native_fixture::peerCount = 1;
        auto& stored = owner_native_fixture::peers[0U]; stored.val[0U] = 0x31U;
        owner_native_fixture::connectionResult = 0;
        auto& descriptor = owner_native_fixture::connection;
        auto& identity = descriptor.peer_id_addr;
        identity.val[0U] = 0x71U;
        if (evidence == Evidence::Static || evidence == Evidence::Resolved ||
            evidence == Evidence::Rpa || evidence == Evidence::Nrpa) identity.type = BLE_ADDR_RANDOM;
        if (evidence == Evidence::Static) identity.val[5U] = 0xc0U;
        if (evidence == Evidence::Rpa || evidence == Evidence::Resolved) identity.val[5U] = 0x40U;
        if (evidence == Evidence::Owner || evidence == Evidence::OwnerResolved) identity = stored;
        if (evidence == Evidence::OwnerResolved) identity.type = BLE_ADDR_PUBLIC_ID;
        descriptor.peer_ota_addr = identity;
        if (evidence == Evidence::Resolved || evidence == Evidence::OwnerResolved) ++descriptor.peer_ota_addr.val[0U];
        if (evidence == Evidence::NoConnection) owner_native_fixture::connectionResult = 1;
        if (evidence == Evidence::NoInventory) owner_native_fixture::inventoryResult = 28;
        if (evidence == Evidence::MultiplePeers) owner_native_fixture::peerCount = 2;
        const auto disconnects = NimBLEDevice::getServer()->DisconnectCount();
        // A fresh link retires the fixture's initial callback before reusing
        // its handle; callback replay itself must remain idempotent.
        fixture.service.OnDisconnected(7U, 19);
        fixture.service.DrainEventQueue();
        // Real connection callback reads native proof before any encryption event.
        fixture.service.OnConnected(7U, "native-identity", 23U, 1U);
        fixture.service.DrainEventQueue();
        const auto epoch = Peer::ProducerEpoch(fixture.service);
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, epoch),
              "native identity evidence activates actual current-generation session");
        fixture.service.UpdateOwnerPairing();
        const bool foreign = evidence == Evidence::Public || evidence == Evidence::Static || evidence == Evidence::Resolved ||
            evidence == Evidence::Rpa || evidence == Evidence::Nrpa;
        Check(Peer::OwnerWireReason(fixture.service) == (foreign ? Reason::WrongConnection : Reason::None) &&
                  !Peer::ProductAccess(fixture.service) &&
                  NimBLEDevice::getServer()->DisconnectCount() == disconnects,
              "checked nonmember including unresolved RPA offers only physical consent without product access");
        if (evidence == Evidence::Owner || evidence == Evidence::OwnerResolved) {
            Check(Peer::OwnerWireState(fixture.service) == State::OwnedOther,
                  "rightful owner remains pending during delayed quiet encryption");
            descriptor.sec_state.encrypted = 1U; descriptor.sec_state.bonded = 1U;
            fixture.service.OnNativeEncryptionChange(7U, 0);
            fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
            Check(Peer::ProductAccess(fixture.service), "delayed native rightful-owner success still reaches product readiness");
        } else if (!foreign) {
            fixture.service.OnNativeEncryptionChange(7U, 5);
            fixture.service.UpdateOwnerPairing();
            Check(NimBLEDevice::getServer()->DisconnectCount() > disconnects &&
                      Peer::OwnerWireReason(fixture.service) != Reason::WrongConnection,
                  "unknown identity and generic security failure retain ordinary teardown without foreign refusal");
        }
        auto stale = fixture.Security(false); stale.peerMatches = false;
        stale.peerIdentityAvailable = true; stale.peerAddressType = 0U;
        owner_native_fixture::Reset();
        fixture.service.OnConnected(8U, "fresh-unknown", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "replacement epoch activates after terminal or pending owner check");
        fixture.service.OnOwnerSecurity(stale); fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(Peer::OwnerWireReason(fixture.service) == Reason::None && !Peer::ProductAccess(fixture.service),
              "old connection native identity proof cannot reject a new generation");
    }
    {
        OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
        fixture.service.OnDisconnected(7U, 19);
        fixture.service.UpdateOwnerPairing(); // Native retirement precedes queued lifecycle projection.
        Check(Peer::OwnerWireReason(fixture.service) == Reason::None,
              "zero retired epoch cannot manufacture foreign-owner proof before disconnect projection");
    }
    std::puts("[BLE_OWNER_FOREIGN] native-identity=pass retained-state-egress=pass quiet-owner=pass generic-teardown=pass stale-generation=pass");
}

void TestOwnerNativeEvidenceRecoversAfterConnectLookupFailure() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    for (const bool inventoryFailure : {true, false}) {
        OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
        fixture.service.OnDisconnected(7U, 19);
        fixture.service.DrainEventQueue();
        owner_native_fixture::inventoryResult = inventoryFailure ? 28 : 0;
        owner_native_fixture::peerCount = 1;
        owner_native_fixture::peers[0U].val[0U] = 0x31U;
        owner_native_fixture::connectionResult = inventoryFailure ? 0 : 1;
        auto& descriptor = owner_native_fixture::connection;
        descriptor.peer_id_addr = owner_native_fixture::peers[0U];
        descriptor.peer_ota_addr = descriptor.peer_id_addr;
        NimBLEDevice::ResetSecurityRequests();
        const auto initialReads = owner_native_fixture::inventoryReads;
        const auto initialLookups = owner_native_fixture::connectionLookups;
        fixture.service.OnConnected(7U, "owner-after-sleep", 23U, 1U);
        fixture.service.DrainEventQueue();
        Check(owner_native_fixture::inventoryReads == initialReads + 1U &&
                  owner_native_fixture::connectionLookups == initialLookups + 1U,
              "actual reconnect callback reaches the failing native evidence lookup");
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "reconnected owner opens the actual shared session while native proof is incomplete");
        Check(!Peer::ProductAccess(fixture.service) && NimBLEDevice::SecurityRequests() == 0U,
              "incomplete reconnect evidence cannot grant access or request security");
        owner_native_fixture::inventoryResult = 0;
        owner_native_fixture::connectionResult = 0;
        fixture.service.UpdateOwnerPairing();
        fixture.service.DrainEventQueue();
        fixture.service.UpdateOwnerPairing();
        Check(owner_native_fixture::inventoryReads == initialReads + 2U &&
                  owner_native_fixture::connectionLookups == initialLookups + 2U &&
                  NimBLEDevice::SecurityRequests() == 1U && NimBLEDevice::SecurityHandle() == 7U,
              "restored native evidence starts quiet owner security once without another connect or claim");
        if (NimBLEDevice::SecurityRequests() != 1U) continue;
        Check(!Peer::ProductAccess(fixture.service),
              "recovering peer evidence still requires the actual encryption completion");
        descriptor.sec_state.encrypted = 1U;
        descriptor.sec_state.bonded = 1U;
        fixture.service.OnNativeEncryptionChange(7U, 0);
        fixture.service.DrainEventQueue();
        fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && Peer::OwnerWireReady(fixture.service),
              "real native encryption callback publishes current owner Ready after evidence recovery");
        const auto readyReads = owner_native_fixture::inventoryReads;
        const auto readyLookups = owner_native_fixture::connectionLookups;
        for (unsigned turn = 0U; turn < 8U; ++turn) fixture.service.UpdateOwnerPairing();
        Check(owner_native_fixture::inventoryReads == readyReads &&
                  owner_native_fixture::connectionLookups == readyLookups &&
                  NimBLEDevice::SecurityRequests() == 1U,
              "healthy owner updates do not reload inventory or repeat native security");
        std::printf("[OWNER_EVIDENCE_RECOVERY] fault=%s reads=%u lookups=%u security=%u ready=%u\n",
            inventoryFailure ? "inventory" : "connection", readyReads - initialReads,
            readyLookups - initialLookups, NimBLEDevice::SecurityRequests(),
            unsigned(Peer::ProductAccess(fixture.service)));
    }
    {
        OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
        fixture.service.OnDisconnected(7U, 19);
        fixture.service.DrainEventQueue();
        NimBLEDevice::ResetSecurityRequests();
        const auto initialReads = owner_native_fixture::inventoryReads;
        const auto initialLookups = owner_native_fixture::connectionLookups;
        fixture.service.OnConnected(7U, "owner-still-unavailable", 23U, 1U);
        fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "persistent lookup fault still admits the shared bootstrap");
        fixture.service.UpdateOwnerPairing();
        fixture.service.DrainEventQueue();
        for (unsigned turn = 0U; turn < 8U; ++turn) fixture.service.UpdateOwnerPairing();
        Check(owner_native_fixture::inventoryReads == initialReads + 2U &&
                  owner_native_fixture::connectionLookups == initialLookups + 2U &&
                  NimBLEDevice::SecurityRequests() == 0U && !Peer::ProductAccess(fixture.service),
              "persistent native lookup failure gets one reread and never polls inventory or grants access");
        fixture.service.OnDisconnected(7U, 19);
        fixture.service.DrainEventQueue();
        owner_native_fixture::inventoryResult = 0;
        owner_native_fixture::peerCount = 1;
        owner_native_fixture::peers[0U].val[0U] = 0x31U;
        owner_native_fixture::connectionResult = 0;
        auto& descriptor = owner_native_fixture::connection;
        descriptor.peer_id_addr = owner_native_fixture::peers[0U];
        descriptor.peer_ota_addr = descriptor.peer_id_addr;
        const auto freshReads = owner_native_fixture::inventoryReads;
        fixture.service.OnConnected(7U, "owner-fresh-epoch", 23U, 1U);
        fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "actual disconnect recovery creates a fresh shared session");
        fixture.service.UpdateOwnerPairing();
        Check(owner_native_fixture::inventoryReads == freshReads + 1U &&
                  NimBLEDevice::SecurityRequests() == 1U,
              "complete native facts on the fresh epoch require no cooperative reread");
        descriptor.sec_state.encrypted = 1U;
        descriptor.sec_state.bonded = 1U;
        fixture.service.OnNativeEncryptionChange(7U, 0);
        fixture.service.DrainEventQueue();
        fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && Peer::OwnerWireReady(fixture.service),
              "fresh epoch recovers from an exhausted evidence reread through real encryption completion");
        std::puts("[OWNER_EVIDENCE_BOUNDED] persistent_reads=2 security=0 fresh_epoch_ready=1");
    }
}
}


namespace {
void TestOwnerDuplicateNativeConnectPreservesAttempt() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    OwnerPreviewFixture fixture;
    fixture.service.Begin(fixture.harness.settings);
    auto* server = NimBLEDevice::getServer();
    Check(server != nullptr && server->Callbacks() != nullptr,
          "duplicate connect uses the actual registered native server callback");
    NimBLEConnInfo current(23U, 7U);
    const auto epoch = Peer::ProducerEpoch(fixture.service);
    const auto session = Peer::ConnectionToken(fixture.service);
    const auto disconnects = server->DisconnectCount();
    for (unsigned repeat = 0U; repeat < 3U; ++repeat) {
        server->Callbacks()->onConnect(server, current);
        fixture.service.DrainEventQueue();
        Check(server->DisconnectCount() == disconnects &&
                  Peer::ProducerEpoch(fixture.service) == epoch &&
                  Peer::ConnectionToken(fixture.service) == session,
              "repeated native connect callback must not disconnect or replace the same live pairing attempt");
    }
    NimBLEConnInfo secondary(23U, 8U);
    server->Callbacks()->onConnect(server, secondary);
    Check(server->DisconnectCount() == disconnects + 1U &&
              server->LastDisconnect() == 8U &&
              Peer::ProducerEpoch(fixture.service) == epoch,
          "a second native handle stays refused without retiring the current pairing attempt");
    std::puts("[BLE_OWNER_PREFIX] duplicate-native-connect=pass secondary-refused=pass");
}
}

namespace {
void TestRejectedOwnerEpochCannotStartQuietSecurity() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    OwnerPreviewFixture fixture(modules::BoardOwnerRecord::Active);
    fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
    owner_native_fixture::inventoryResult = 0; owner_native_fixture::peerCount = 1;
    owner_native_fixture::peers[0U].val[0U] = 0x31U;
    owner_native_fixture::connectionResult = 0;
    owner_native_fixture::connection.peer_id_addr = owner_native_fixture::peers[0U];
    owner_native_fixture::connection.peer_ota_addr = owner_native_fixture::connection.peer_id_addr;
    fixture.service.OnConnected(7U, "matched-before-activation", 23U, 1U);
    fixture.service.DrainEventQueue();
    Check(!Peer::PortableSessionActive(fixture.service) && NimBLEDevice::SecurityRequests() == 0U,
          "matching native identity caches before shared-session activation without a security request");
    const auto disconnects = NimBLEDevice::getServer()->DisconnectCount();
    fixture.service.OnNativeEncryptionChange(7U, 5);
    Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
          "rejected native epoch can reach queued transport activation before cooperative teardown");
    fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
    Check(NimBLEDevice::SecurityRequests() == 0U,
          "raw failure before activation must not solicit quiet security from the rejected epoch");
    Check(!Peer::ProductAccess(fixture.service) &&
              NimBLEDevice::getServer()->DisconnectCount() > disconnects,
          "rejected matched epoch still tears down without readiness or another OS security request");
    std::puts("[BLE_OWNER_REJECTED_EPOCH] cached-match=pass failure-before-activation=pass request-count=0 teardown=pass");
}

void TestNativeSavedPhonesThroughSharedSession() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Record = modules::BoardOwnerRecord;
    using Controller = modules::BoardOwnerPairingController;
    namespace wire = modules::connection_session_codec;
    for (const bool phoneInitiated : {false, true}) for (uint8_t saved = 1U; saved <= 3U; ++saved) {
        OwnerPreviewFixture fixture(Record::Active);
        fixture.runtime.Boot(Record::Active, {true, saved}, true);
        owner_native_fixture::inventoryResult = 0; owner_native_fixture::peerCount = saved;
        for (uint8_t i = 0U; i < saved; ++i) owner_native_fixture::peers[i].val[0U] = i + 1U;
        owner_native_fixture::rpaCount = 3;
        for (uint8_t i = 0U; i < 3U; ++i) {
            owner_native_fixture::rpas[i].peer_rpa_addr.type = BLE_ADDR_RANDOM;
            owner_native_fixture::rpas[i].peer_rpa_addr.val[0U] = i == 2U ? 0x71U : 81U + i;
            owner_native_fixture::rpas[i].peer_rpa_addr.val[5U] = i == 2U ? 0x40U : 0U;
            owner_native_fixture::rpas[i].peer_addr.val[0U] = i == 2U ? 9U : i == 1U ? 99U : 1U;
        }
        owner_native_fixture::connectionResult = 0;
        auto& descriptor = owner_native_fixture::connection;
        descriptor.peer_id_addr.type = BLE_ADDR_RANDOM;
        descriptor.peer_id_addr.val[0U] = 0x71U; descriptor.peer_id_addr.val[5U] = 0x40U;
        descriptor.peer_ota_addr = descriptor.peer_id_addr; // unresolved new-phone RPA
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        fixture.service.OnConnected(7U, "new-phone-rpa", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "saved-phones journey activates actual minimum-MTU shared session");
        fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == 0U && !Peer::ProductAccess(fixture.service),
              "unresolved private nonmember cannot solicit security or access before consent");
        Check(Peer::NativePairingCandidate(fixture.service, 7U) == (saved < 3U),
              "actual pre-write native admission independently refuses full custody for unresolved peer");
        const auto newIdentity = descriptor.peer_id_addr;
        descriptor.peer_id_addr = owner_native_fixture::peers[0U];
        Check(!Peer::NativePairingCandidate(fixture.service, 7U),
              "key-missing repeat that resolves to saved identity is refused before replacement writes");
        descriptor.peer_id_addr.val[0U] = 10U;
        Check(!Peer::NativePairingCandidate(fixture.service, 7U),
              "new resolved identity cannot overwrite preexisting RPA metadata for another identity");
        descriptor.peer_id_addr = newIdentity;
        std::array<uint8_t, 20U> bytes{}; size_t size = 0U;
        std::array<uint8_t, 8U> open{};
        fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
        open[3U] = modules::ConnectionSession::ORDERED_CAPACITY; fxe::WriteU16LE(10U, open.data() + 4U);
        Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
            Peer::ConnectionToken(fixture.service), 2U}, {open.data(), open.size()}},
            {bytes.data(), bytes.size()}, size) == wire::Error::None, "add journey opens actual Ordered lane");
        fixture.service.OnConnectionSessionWrite(bytes.data(), size); fixture.service.DrainEventQueue();
        const std::array<uint8_t, 5U> begin{{1U, uint8_t(phoneInitiated ? 3U : 1U), 79U, 0U, 2U}};
        Check(wire::Codec::Encode({{wire::Kind::Ordered, 27U, Peer::OrderedToken(fixture.service), 1U},
            {begin.data(), begin.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
            "new phone Begin uses real shared command ingress");
        fixture.service.OnConnectionSessionWrite(bytes.data(), size); fixture.service.DrainEventQueue();
        if (saved == 3U) {
            Check(fixture.runtime.Custody().GetOperation() == Controller::Operation::None &&
                      fixture.persistence.next == 0U && NimBLEDevice::SecurityRequests() == 0U,
                  "fourth unresolved phone is rejected before prompt/security/key mutation");
            continue;
        }
        Check(fixture.runtime.Custody().GetPhase() == Controller::Phase::Armed,
              "second and third phone enter existing either-end consent");
        Check((Peer::OwnerWireFlags(fixture.service) & modules::owner_pairing_session_wire::NativeBondAllowed) == 0U,
              "phone cannot initiate security before fresh physical consent");
        fixture.runtime.ObserveButton({1U, true, true, fxe::Millis()});
        fixture.runtime.ObserveButton({1U, false, true, fxe::Millis()}); fixture.service.UpdateOwnerPairing();
        Check(fixture.persistence.baseline.saved.count == saved && fixture.persistence.baseline.rpaCount == 3U &&
                  fixture.persistence.baseline.rpas[1U].identity.address[0U] == 99U &&
                  NimBLEDevice::SecurityRequests() == 0U,
              "production adapter snapshots all prior identities and orphan RPA before security");
        Check((Peer::OwnerWireFlags(fixture.service) & modules::owner_pairing_session_wire::NativeBondAllowed) == 0U,
              "Securing while Adding journal is pending grants no phone security admission");
        fixture.persistence.outcome = OwnerPreviewPersistence::Outcome::Saved;
        fixture.persistence.release = !phoneInitiated;
        fixture.runtime.UpdateDurableState(Record::Adding, true); fixture.service.UpdateOwnerPairing();
        if (phoneInitiated) {
            Check(NimBLEDevice::SecurityRequests() == 0U && !fixture.service.NativePairingAllowed(7U) &&
                      (Peer::OwnerWireFlags(fixture.service) & modules::owner_pairing_session_wire::NativeBondAllowed) == 0U,
                  "Saved but unreleased Adding receipt cannot advertise or install native admission");
            fixture.persistence.release = true;
            fixture.service.UpdateOwnerPairing();
        }
        Check(NimBLEDevice::SecurityRequests() == (phoneInitiated ? 0U : 1U) &&
                  fixture.service.NativePairingAllowed(7U),
              "released Adding receipt installs exact lease without peripheral request in phone mode");
        Check(((Peer::OwnerWireFlags(fixture.service) & modules::owner_pairing_session_wire::NativeBondAllowed) != 0U) == phoneInitiated,
              "only phone mode publishes the authoritative post-receipt native bond flag");
        for (unsigned replay = 0U; replay < 3U; ++replay) fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == (phoneInitiated ? 0U : 1U),
              "replayed progress never solicits a second peripheral security request");
        const auto android = android_pairing_consent::Replay(NimBLEDevice::SecurityRequests());
        Check(android.securityConsent == (phoneInitiated ? 0U : 1U) && android.scConsent == 1U &&
                  android.securityConsent + android.scConsent == (phoneInitiated ? 1U : 2U),
              "actual AOSP Security Request and SC Just Works stages explain two old consents versus one phone-initiated consent");
        std::printf("[ANDROID_PAIRING_STAGES] saved=%u phone=%u peripheral_request=%u security_consent=%u sc_consent=%u\n",
            unsigned(saved), unsigned(phoneInitiated), unsigned(NimBLEDevice::SecurityRequests()),
            android.securityConsent, android.scConsent);
        owner_native_fixture::peers[saved].val[0U] = 9U;
        descriptor.peer_id_addr = owner_native_fixture::peers[saved];
        Check(Peer::NativePairingCandidate(fixture.service, 7U) == (saved < 3U),
              "resolved new identity can preserve its preexisting exact RPA mapping before key writes");
        owner_native_fixture::peerCount = saved + 1U;
        descriptor.sec_state.encrypted = 1U; descriptor.sec_state.bonded = 1U;
        fixture.service.OnNativeEncryptionChange(7U, 0); fixture.service.DrainEventQueue();
        Check(fixture.persistence.written == Record::Active && !Peer::ProductAccess(fixture.service),
              "actual completion carries complete baseline plus new identity into pending Active save");
        fixture.persistence.outcome = OwnerPreviewPersistence::Outcome::Saved;
        fixture.runtime.UpdateDurableState(Record::Active, true); fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && fixture.runtime.Custody().SavedPhoneCount() == saved + 1U,
              "second and third phone become ready only after durable save");
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        descriptor.peer_id_addr = owner_native_fixture::peers[0U];
        descriptor.peer_id_addr.type = BLE_ADDR_PUBLIC_ID;
        descriptor.peer_ota_addr.type = BLE_ADDR_RANDOM; descriptor.peer_ota_addr.val[0U] = 0x81U;
        fixture.service.OnConnected(7U, "saved-phone-resolved-rpa", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "previous phone reconnects on its new session");
        fixture.service.OnNativeEncryptionChange(7U, 0); fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(Peer::ProductAccess(fixture.service) && fixture.persistence.next == 2U,
              "previous saved phone with resolved RPA reconnects quietly without another consent/save");
    }
}

void TestOwnerNativeSecurityRequestsAreIntentBounded() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Record = modules::BoardOwnerRecord;
    {
        OwnerPreviewFixture fixture(Record::Active);
        for (unsigned replay = 0U; replay < 3U; ++replay) fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == 0U,
              "unknown native identity must not solicit an OS pairing request on an owned board");
        owner_native_fixture::inventoryResult = 0;
        owner_native_fixture::peerCount = 1;
        owner_native_fixture::peers[0U].val[0U] = 0x31U;
        owner_native_fixture::connectionResult = 0;
        owner_native_fixture::connection.peer_id_addr.val[0U] = 0x71U;
        owner_native_fixture::connection.peer_ota_addr = owner_native_fixture::connection.peer_id_addr;
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        fixture.service.OnConnected(7U, "foreign", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "foreign preflight uses the actual activated shared session");
        fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == 0U && Peer::OwnerWireReason(fixture.service) ==
                  modules::owner_pairing_session_wire::Reason::WrongConnection,
              "owned-other public preflight explains ownership with zero native security requests");
        owner_native_fixture::connection.peer_id_addr = owner_native_fixture::peers[0U];
        owner_native_fixture::connection.peer_ota_addr = owner_native_fixture::connection.peer_id_addr;
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        fixture.service.OnConnected(7U, "owner", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "positive native owner match uses current shared-session generation");
        for (unsigned replay = 0U; replay < 3U; ++replay) fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == 1U && NimBLEDevice::SecurityHandle() == 7U,
              "only a positively matched native bond requests quiet security once per link");
    }
    {
        OwnerPreviewFixture fixture;
        const auto success = fixture.Security();
        Check(NimBLEDevice::SecurityRequests() == 1U && fixture.service.NativePairingAllowed(7U),
              "fresh physical consent invokes the real native request callsite exactly once");
        for (unsigned replay = 0U; replay < 3U; ++replay) {
            fixture.service.OnConnected(7U, "duplicate", 23U, 1U);
            fixture.service.UpdateOwnerPairing();
        }
        Check(NimBLEDevice::SecurityRequests() == 1U,
              "repeated connect callback and pending consent cannot repeat the native OS request");
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        fixture.service.OnConnected(7U, "replacement", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "replacement link activates a distinct native epoch");
        fixture.service.OnOwnerSecurity(success); fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        Check(NimBLEDevice::SecurityRequests() == 1U && !fixture.service.NativePairingAllowed(7U) &&
                  !Peer::ProductAccess(fixture.service),
              "late old-session security callback cannot create a ghost request on reconnect");
    }
    std::puts("[BLE_OWNER_REQUESTS] public-preflight=pass unknown-no-prompt=pass consent-once=pass stale-no-ghost=pass");
}
}

namespace {
void TestIdentifyRatePolicyAndUnpairedOrderedPath() {
    namespace identify = modules::ble_identify;
    using Result = identify::RateLimit::Result;
    identify::RateLimit limit;
    identify::PeerIdentity peer{}; peer.available = true; peer.address[0U] = 7U;
    for (uint32_t now : {1U, 2U, 3U})
        Check(limit.Start(peer, now) == Result::Allowed, "Identify first three new starts are allowed");
    Check(limit.Start(peer, 10U) == Result::Cooldown, "Identify fourth new start begins its cooldown");
    Check(limit.Start(peer, 300009U) == Result::Cooldown, "ignored Identify spam does not extend or shorten cooldown");
    Check(limit.Start(peer, 300010U) == Result::Allowed, "Identify cooldown expires at its fixed fourth-start boundary");
    identify::RateLimit full;
    for (uint8_t index = 0U; index < 8U; ++index) {
        peer.address[0U] = index;
        Check(full.Start(peer, 10U) == Result::Allowed, "Identify reserves each of eight fixed native peers");
    }
    peer.address[0U] = 9U;
    Check(full.Start(peer, 11U) == Result::Capacity, "ninth Identify peer cannot evict active accounting");
    Check(full.Start(peer, 300010U) == Result::Allowed, "idle partial peer buckets expire after five minutes");
    identify::RateLimit wrap;
    peer.type = 1U;
    for (uint32_t now : {0xfffffff0U, 0xfffffff1U, 0xfffffff2U})
        Check(wrap.Start(peer, now) == Result::Allowed, "Identify accounting admits starts across millis wrap");
    Check(wrap.Start(peer, 0xfffffff3U) == Result::Cooldown &&
              wrap.Start(peer, 299986U) == Result::Cooldown && wrap.Start(peer, 299987U) == Result::Allowed,
          "Identify fixed cooldown is unsigned-millis wrap safe");
    identify::Command decoded{};
    const std::array<uint8_t, 8U> start{{1U, 1U, 7U, 0U, 0U, 255U, 0xb8U, 0x0bU}};
    Check(identify::Decode(start.data(), start.size(), decoded) && decoded.number == 7U && decoded.durationMs == 3000U,
          "Identify typed Start is canonical at minimum ATT MTU");
    auto invalid = start; invalid[2U] = 100U;
    Check(!identify::Decode(invalid.data(), invalid.size(), decoded), "Identify rejects out of range wire number");
    invalid = start; invalid[1U] = 0U;
    Check(!identify::Decode(invalid.data(), invalid.size(), decoded), "Identify Probe cannot hide a Start payload");

    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    for (const auto owner : {modules::BoardOwnerRecord::Absent, modules::BoardOwnerRecord::Active}) {
    OwnerPreviewFixture fixture(owner);
    owner_native_fixture::connectionResult = 0;
    owner_native_fixture::connection.peer_id_addr.val[0U] = 7U;
    std::array<uint8_t, 20U> packet{};
    size_t size = 0U;
    std::array<uint8_t, 8U> open{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
    open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
    fxe::WriteU16LE(10U, open.data() + 4U);
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
        Peer::ConnectionToken(fixture.service), 2U}, {open.data(), open.size()}},
        {packet.data(), packet.size()}, size) == wire::Error::None, "Identify opens the existing Ordered lane");
    fixture.service.OnConnectionSessionWrite(packet.data(), size); fixture.service.DrainEventQueue();
    auto send = [&](uint32_t sequence, const std::array<uint8_t, 8U>& body) {
        Check(wire::Codec::Encode({{wire::Kind::Ordered, 28U, Peer::OrderedToken(fixture.service), sequence},
            {body.data(), body.size()}}, {packet.data(), packet.size()}, size) == wire::Error::None,
            "Identify encodes through the actual shared-session ingress");
        fixture.service.OnConnectionSessionWrite(packet.data(), size); fixture.service.DrainEventQueue();
    };
    uint8_t number = 0U, red = 0U, green = 0U, blue = 0U;
    bool padded = false; uint32_t duration = 0U;
    send(1U, start);
    Check(fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration) &&
              number == 7U && blue == 255U && duration == 3000U && !Peer::ProductAccess(fixture.service),
          "unpaired typed Identify reaches the production presentation without opening product access");
    send(1U, start);
    Check(!fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration),
          "duplicate Ordered receipt never restarts Identify or consumes a peer allowance");
    for (uint32_t sequence : {2U, 3U}) {
        send(sequence, start);
        Check(fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration),
              "each of the first three fresh Ordered Starts reaches presentation after a free duplicate receipt");
    }
    send(4U, start);
    Check(!fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration),
          "fourth new Ordered Start is ignored by the production peer rate owner");
    uint16_t reason = 0U;
    Check(Peer::TakeOrderedRejection(fixture.service, 4U, &reason) &&
              reason == static_cast<uint16_t>(identify::Rejection::Cooldown) && fixture.service.IsConnected(),
          "Identify cooldown has its exact domain Result and never denies the native connection");
    const std::array<uint8_t, 8U> stop{{1U, 2U, 0U, 0U, 0U, 0U, 0U, 0U}};
    send(5U, stop);
    Check(fixture.service.ConsumeIdentifyStop(), "typed Stop remains free during Identify cooldown");
    const std::array<uint8_t, 8U> probe{{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
    send(6U, probe);
    Check(!fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration) &&
              !fixture.service.ConsumeIdentifyStop(), "typed support Probe never changes presentation or consumes allowance");
    fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
    owner_native_fixture::connection.peer_id_addr.type = BLE_ADDR_PUBLIC_ID;
    owner_native_fixture::connection.peer_ota_addr.val[0U] = 0x91U;
    fixture.service.OnConnected(7U, "same-resolved-peer", 23U, 1U); fixture.service.DrainEventQueue();
    Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
          "Identify reconnect opens a fresh shared-session epoch");
    Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
        Peer::ConnectionToken(fixture.service), 2U}, {open.data(), open.size()}},
        {packet.data(), packet.size()}, size) == wire::Error::None, "Identify reconnect reopens Ordered");
    fixture.service.OnConnectionSessionWrite(packet.data(), size); fixture.service.DrainEventQueue();
    send(1U, start);
    Check(!fixture.service.ConsumeIdentifyStart(number, red, green, blue, padded, duration) &&
              Peer::TakeOrderedRejection(fixture.service, 1U, &reason) &&
              reason == static_cast<uint16_t>(identify::Rejection::Cooldown) && fixture.service.IsConnected(),
          "resolved native identity retains cooldown across reconnect and changing OTA address");
    fixture.service.OnCommandCharacteristicWrite("identify\n", 9U); fixture.service.DrainEventQueue();
    char ch = 0;
    Check(!fixture.service.TryReadConsoleInputChar(ch) && !Peer::ProductAccess(fixture.service),
          "typed Identify does not open an unpaired legacy Console backdoor");
    }
    {
        OwnerPreviewFixture fixture;
        owner_native_fixture::connectionResult = 0;
        owner_native_fixture::connection.peer_id_addr.val[0U] = 7U;
        std::array<uint8_t, 20U> packet{}; size_t size = 0U;
        std::array<uint8_t, 8U> open{};
        fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, open.data());
        open[3U] = modules::ConnectionSession::ORDERED_CAPACITY;
        fxe::WriteU16LE(10U, open.data() + 4U);
        Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
            Peer::ConnectionToken(fixture.service), 2U}, {open.data(), open.size()}},
            {packet.data(), packet.size()}, size) == wire::Error::None, "disconnect Identify opens Ordered");
        fixture.service.OnConnectionSessionWrite(packet.data(), size); fixture.service.DrainEventQueue();
        Check(wire::Codec::Encode({{wire::Kind::Ordered, 28U, Peer::OrderedToken(fixture.service), 1U},
            {start.data(), start.size()}}, {packet.data(), packet.size()}, size) == wire::Error::None,
            "disconnect Identify encodes Start");
        fixture.service.OnConnectionSessionWrite(packet.data(), size); fixture.service.DrainEventQueue();
        modules::BleIdentifyPresentation presentation;
        modules::MenuLevelOverlay overlay;
        const std::array<modules::Rgb, 64U> background{};
        presentation.Consume(fixture.service, 100U);
        Check(presentation.Update(100U, false, overlay, background), "Identify active before disconnect");
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        presentation.Consume(fixture.service, 101U);
        Check(presentation.Update(3099U, false, overlay, background),
              "disconnect alone leaves the original bounded Identify deadline");
        Check(!presentation.Update(3100U, false, overlay, background) && !overlay.IsActive(),
              "disconnected Identify expires without another phone command");
    }
    std::puts("[BLE_IDENTIFY] typed-unpaired=pass three-starts=pass fourth-cooldown=pass dedupe=pass stop-free=pass fixed-peers=pass");
}
}

namespace {
void TestTypedIdentifyVirtualLitchTimedJourney() {
    const uint32_t restoreMs = fxe::ManualMillis::NowMs();
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    namespace wire = modules::connection_session_codec;
    for (const auto hardware : {VIRTUAL_LITCH_HARDWARE_S3, VIRTUAL_LITCH_HARDWARE_V3, VIRTUAL_LITCH_HARDWARE_V3_3}) {
      for (const bool rotated : {false, true}) {
       for (const bool opposite : {false, true}) {
        OwnerPreviewFixture fixture;
        owner_native_fixture::connectionResult = 0;
        owner_native_fixture::connection.peer_id_addr.val[0U] = 7U;
        virtual_litch_config config{}; virtual_litch_default_config(&config);
        config.hardware = hardware; config.board_rotate = rotated; config.first_player_is_p2 = opposite;
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession board(config, engine);
        board.BindOwnerPairingRuntime(fixture.runtime);
        board.ObserveBootHallMask(UINT64_C(0xffff00000000ffff));
        board.Advance(3000U);
        const auto initial = board.HomeStateSnapshot();
        const auto initialHash = board.SemanticFrame().board_hash;
        const auto initialPhysical = board.PhysicalFrame();
        struct Ingress {
            modules::BleService* service;
            uint32_t sequence = 0U;
            std::array<uint8_t, 8U> body{};
        } ingress{&fixture.service};
        auto send = [&](uint32_t sequence, std::array<uint8_t, 8U> body) {
            ingress.sequence = sequence; ingress.body = body;
            board.RunIdentifyIngress([](void* context, modules::BleIdentifyPresentation& presentation, uint32_t now) {
                auto& in = *static_cast<Ingress*>(context);
                std::array<uint8_t, 20U> bytes{}; size_t size = 0U;
                Check(wire::Codec::Encode({{wire::Kind::Ordered, 28U, Peer::OrderedToken(*in.service), in.sequence},
                    {in.body.data(), in.body.size()}}, {bytes.data(), bytes.size()}, size) == wire::Error::None,
                    "timed Identify journey encodes its actual Ordered request");
                in.service->OnConnectionSessionWrite(bytes.data(), size); in.service->DrainEventQueue();
                presentation.Consume(*in.service, now);
            }, &ingress);
        };
        auto open = [&] {
            board.RunIdentifyIngress([](void* context, modules::BleIdentifyPresentation& presentation, uint32_t now) {
                auto& in = *static_cast<Ingress*>(context);
                std::array<uint8_t, 8U> body{};
                fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, body.data());
                body[3U] = modules::ConnectionSession::ORDERED_CAPACITY; fxe::WriteU16LE(10U, body.data() + 4U);
                std::array<uint8_t, 20U> bytes{}; size_t size = 0U;
                Check(wire::Codec::Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
                    Peer::ConnectionToken(*in.service), 2U}, {body.data(), body.size()}},
                    {bytes.data(), bytes.size()}, size) == wire::Error::None, "timed Identify opens real Ordered");
                in.service->OnConnectionSessionWrite(bytes.data(), size); in.service->DrainEventQueue();
                presentation.Consume(*in.service, now);
            }, &ingress);
        };
        auto checkpoint = [&](const char* stage, const char* result, const char* next) {
            size_t semanticBlue = 0U, physicalBlue = 0U;
            for (const auto& square : board.SemanticFrame().board) for (const auto& color : square)
                if (color.b > color.r && color.b > color.g) ++semanticBlue;
            for (size_t pixel = 0U; pixel < board.PhysicalFrame().led_count; ++pixel) {
                const auto& color = board.PhysicalFrame().pixels[pixel];
                if (color.b > color.r && color.b > color.g) ++physicalBlue;
            }
            const auto home = board.HomeStateSnapshot();
            Check(home.freshSetup == initial.freshSetup && home.placementMode == initial.placementMode &&
                      !board.Litchcraft().IsActive() && !Peer::ProductAccess(fixture.service),
                  "timed Identify never changes authoritative mode or opens product access");
            std::printf("[TYPED_IDENTIFY_VIRTUAL_JOURNEY] hw=%u rotate=%u opposite=%u at=%u occupancy=%016llx mode=%s stage=%s result=%s display=\"%.*s\" blue=%zu physical_blue=%zu phase=%u next=%s\n",
                unsigned(hardware), unsigned(rotated), unsigned(opposite), unsigned(board.State().now_ms),
                static_cast<unsigned long long>(board.State().physical_occupied_mask), home.freshSetup ? "Setup" : "Play",
                stage, result, int(board.DisplayText().size()), board.DisplayText().data(), semanticBlue, physicalBlue,
                unsigned(board.State().interaction_phase), next);
            return semanticBlue != 0U && physicalBlue != 0U;
        };
        open();
        const std::array<uint8_t, 8U> start{{1U, 1U, 7U, 0U, 0U, 255U, 0xb8U, 0x0bU}};
        send(1U, start);
        bool physicalBoardChanged = false;
        const size_t boardPixels = hardware == VIRTUAL_LITCH_HARDWARE_S3 ? 64U : 256U;
        for (size_t pixel = 0U; pixel < boardPixels; ++pixel) {
            const auto& before = initialPhysical.pixels[pixel];
            const auto& after = board.PhysicalFrame().pixels[pixel];
            physicalBoardChanged = physicalBoardChanged || before.r != after.r || before.g != after.g ||
                before.b != after.b || before.w != after.w;
        }
        Check(board.DisplayText() == "7" && board.SemanticFrame().board_hash != initialHash && physicalBoardChanged &&
                  checkpoint("first-start", "accepted", "wait-3000ms"),
              "typed accepted Identify produces its number in changed semantic and mapped physical board LEDs");
        send(1U, start); // Exact receipt replay neither restarts nor consumes.
        board.Advance(2999U);
        Check(board.DisplayText() == "7", "Identify remains readable until its actual duration boundary");
        board.Advance(1U);
        Check(board.DisplayText() != "7" && board.SemanticFrame().board_hash == initialHash,
              "production Identify timing expires exactly and restores the underlying board");
        checkpoint("timed-expiry", "expired", "request-second-start");
        send(2U, start); Check(board.DisplayText() == "7", "second fresh Start is admitted after free replay");
        const std::array<uint8_t, 8U> stop{{1U, 2U, 0U, 0U, 0U, 0U, 0U, 0U}};
        send(3U, stop);
        Check(board.DisplayText() != "7", "typed Stop cancels the production timing owner immediately");
        checkpoint("stop", "accepted", "request-third-start");
        send(4U, start); Check(board.DisplayText() == "7", "third fresh Start remains allowed");
        board.Advance(3000U);
        send(5U, start); // Fourth NEW Start starts the fixed cooldown.
        uint16_t reason = 0U;
        Check(board.DisplayText() != "7" && Peer::TakeOrderedRejection(fixture.service, 5U, &reason) &&
                  reason == static_cast<uint16_t>(modules::ble_identify::Rejection::Cooldown) && fixture.service.IsConnected(),
              "fourth Start is ignored with cooldown receipt while connection remains admitted");
        checkpoint("fourth-start", "cooldown", "wait-fixed-300000ms");
        auto malformed = start; malformed[0U] = 2U;
        send(6U, malformed);
        Check(board.DisplayText() != "7" && Peer::TakeOrderedRejection(fixture.service, 6U),
              "unknown Identify version is rejected without a cue or new allowance");
        send(7U, stop);
        fixture.service.OnDisconnected(7U, 19); fixture.service.DrainEventQueue();
        board.Advance(299999U);
        fixture.service.OnConnected(7U, "same-peer-reconnect", 23U, 1U); fixture.service.DrainEventQueue();
        Check(Peer::ActivateMinimumConnectionObjectSession(fixture.service, Peer::ProducerEpoch(fixture.service)),
              "same peer reconnect remains permitted during Identify cooldown");
        open(); send(1U, start);
        Check(board.DisplayText() != "7" && Peer::TakeOrderedRejection(fixture.service, 1U, &reason) &&
                  reason == static_cast<uint16_t>(modules::ble_identify::Rejection::Cooldown),
              "reconnect and ignored spam cannot reset or extend the fixed cooldown");
        checkpoint("same-peer-reconnect", "cooldown", "wait-final-1ms");
        board.Advance(1U); send(2U, start);
        Check(board.DisplayText() == "7" && checkpoint("cooldown-expired", "accepted", "stop-or-timed-expiry"),
              "next new Start recovers at the original fourth-start boundary after reconnect");
        send(3U, stop);
        Check(board.DisplayText() != "7", "free Stop remains available after cooldown recovery");
        auto edgeX = [&](bool down) {
            Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, down) == VIRTUAL_LITCH_OK,
                  "Identify priority journey uses actual physical X levels");
            board.Advance(20U);
        };
        auto openQuick = [&] {
            edgeX(true); board.Advance(1000U); edgeX(false);
            Check(board.QuickSettingsActive(), "actual long-X opens Quick Setup before Identify");
        };
        auto closeQuick = [&] {
            edgeX(true); edgeX(false);
            Check(!board.QuickSettingsActive(), "actual X closes Quick Setup for the retained Identify deadline");
        };
        openQuick(); send(4U, start);
        Check(board.DisplayText() != "7", "Quick Setup preempts an otherwise accepted typed Identify");
        checkpoint("QuickSetup-open", "accepted-but-suppressed", "close-before-expiry");
        closeQuick();
        Check(board.DisplayText() == "7", "closing Quick Setup before expiry exposes the admitted Identify number");
        board.Advance(2959U); Check(board.DisplayText() == "7", "Quick Setup close never restarts Identify duration");
        board.Advance(1U); Check(board.DisplayText() != "7", "original Identify deadline remains exact after menu close");
        checkpoint("QuickSetup-closed-before-expiry", "expired-on-original-deadline", "open-again");
        openQuick(); send(5U, start); board.Advance(3000U);
        Check(board.QuickSettingsActive() && board.DisplayText() != "7", "suppressed Identify expires while Quick Setup remains open");
        closeQuick();
        Check(board.DisplayText() != "7", "closing Quick Setup after expiry cannot resurrect Identify");
        checkpoint("QuickSetup-closed-after-expiry", "expired-no-late-cue", "ordinary-board-use");
        board.ShowIdentifyOverlay(7U, {0U, 0U, 255U});
        board.Advance(100U);
        openQuick();
        Check(board.DisplayText() != "7", "opening Quick Setup preempts active Identify");
        closeQuick();
       }
      }
    }
    fxe::ManualMillis::SetNowMs(restoreMs);
}
}

namespace {
void TestOwnerSettingsClearExecutionAdmission() {
    using Peer = modules::BleServiceConnectionEpochTestPeer;
    using Record = modules::BoardOwnerRecord;
    for (const bool authenticated : {false,true}) {
        OwnerPreviewFixture fixture(Record::Active);
        if (authenticated) {
            fixture.service.OnOwnerSecurity(fixture.Security(false));
            fixture.service.DrainEventQueue(); fixture.service.UpdateOwnerPairing();
        }
        const auto connection = Peer::OwnerConnection(fixture.service);
        const uint64_t expectedLease = (static_cast<uint64_t>(connection.epoch) << 32U) | connection.sessionToken;
        Check(fixture.service.ConsoleInputGeneration() == (authenticated ? expectedLease : 0U),
              "actual transport lease losslessly reflects admitted native epoch and shared token");
        const bool accepted = fixture.service.RequestOwnerSettingsClear();
        Check(accepted == authenticated,
              "actual BLE reset entry requires current native-authenticated shared-session owner");
        if (authenticated) {
            Check(!Peer::ProductAccess(fixture.service) && fixture.service.ConsoleInputGeneration() == 0U &&
                      fixture.persistence.written == Record::Clearing &&
                      fixture.runtime.Custody().GetPhase() ==
                          modules::BoardOwnerPairingController::Phase::SavingTombstone,
                  "owner clear first admits durable Clearing without pretending native cleanup completed");
            Check(!fixture.service.RequestOwnerSettingsClear() && fixture.persistence.next == 1U,
                  "repeated owner clear cannot duplicate the accepted destructive request");
        } else Check(fixture.persistence.next == 0U,
                     "unproven owner cannot write even the clearing journal");
    }
    OwnerPreviewFixture stale(Record::Active);
    stale.service.OnOwnerSecurity(stale.Security(false));
    stale.service.DrainEventQueue(); stale.service.UpdateOwnerPairing();
    stale.service.OnDisconnected(7U,23U); stale.service.DrainEventQueue();
    Check(!stale.service.RequestOwnerSettingsClear() && stale.persistence.next == 0U,
          "disconnected owner cannot authorize reset from queued Console history");
}
} // namespace
