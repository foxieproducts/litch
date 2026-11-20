"""Narrow checks for the Arduino-only boot orchestration.

Run by the existing ./litch check tools discovery. The cancellation fixture
controls receipt timing without running the LED task or physical hardware.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class BootSequenceContractTest(unittest.TestCase):
    def test_factory_admission_uses_detected_revision_before_hardware_fallback(self):
        # Supplemental source guard: the native fixtures execute raw strap
        # decoding, Console/runtime admission and the actual provisioning/NVS
        # owner, while Arduino's whole SetupApp sequence remains outside them.
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        self.assertIn("hardware::SupportsProductionHardwareTest(\n"
                      "            sys.runtimeHardware.detectedRevision)", boot)
        self.assertIn("sys.factoryProvisioning.Begin(\n"
                      "        sys.runtimeHardware.detectedRevision,", boot)
        self.assertIn("hardware::ShouldEnterHardwareTestFromBoot(\n"
                      "            sys.runtimeHardware.detectedRevision,", boot)
        self.assertIn("sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision,", services)

    def test_retained_startup_witnesses_follow_real_completed_operations(self):
        power = (ROOT / "src/power_rail.cpp").read_text()
        begin = power.split("void PowerRail::Begin() {", 1)[1].split(
            "void PowerRail::Enable5v()", 1)[0]
        self.assertLess(begin.index("Enable5vRail("),
                        begin.index("RecordUsbBootPrecharge("))
        self.assertLess(begin.index("m_fiveVoltOn.store(true, std::memory_order_release);"),
                        begin.index("RecordUsbBootPrecharge("))
        self.assertEqual(power.count("RecordUsbBootPrecharge("), 1)
        returned = (ROOT / "src/firmware_led_boot_receipt.cpp").read_text()
        self.assertIn("sys.bootFirstNonzeroRenderLogged ||", returned)
        self.assertIn("!sys.startupLeds.Fade().IsStarted()", returned)
        self.assertIn("sys.ledPowerStats.appliedLedCurrentMa == 0U", returned)
        self.assertIn("sys.ledAnimation.Receipt().completed : LedOutputCompleted(sys)", returned)
        self.assertLess(returned.index(") return;"),
                        returned.index("sys.bootFirstNonzeroRenderLogged = true;"))
        self.assertLess(returned.index("sys.bootFirstNonzeroRenderLogged = true;"),
                        returned.index("RecordUsbBootFirstNonzeroRender();"))
        self.assertLess(returned.index("RecordUsbBootFirstNonzeroRender();"),
                        returned.index("logger::Trace("))
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        finish = services.split("void FinishStartup() {", 1)[1]
        self.assertLess(finish.index("sys.ble.Begin(sys.settings);"),
                        finish.index("RecordUsbBootServicesReady();"))
        self.assertLess(finish.index("const auto firstSemanticRender = RenderLeds(true);"),
                        finish.index("RecordUsbBootServicesReady();"))
        self.assertLess(finish.index("RecordUsbBootServicesReady();"),
                        finish.index('"boot: services_ms'))
        timing = (ROOT / "src/boot_platform_timing.cpp").read_text()
        app_entry = timing.split('extern "C" void __wrap_app_main() {', 1)[1].split("\n}", 1)[0]
        self.assertLess(app_entry.index("g_platformBootTiming.appMainBeginMs ="),
                        app_entry.index("__real_app_main();"))
        arduino_entry = timing.split('extern "C" void __wrap_initArduino() {', 1)[1].split("\n}", 1)[0]
        positions = [arduino_entry.index(call) for call in (
            "g_platformBootTiming.arduinoBeginMs =", "__real_initArduino();",
            "g_platformBootTiming.arduinoEndMs =")]
        self.assertEqual(positions, sorted(positions))
        for entry in (app_entry, arduino_entry):
            self.assertIn("esp_timer_get_time() / 1000ULL", entry)
            self.assertNotIn("fxe::Millis()", entry)
            self.assertNotIn("PlatformBootTiming()", entry)
        self.assertIn("BootPlatformTiming g_platformBootTiming{};", timing)
        self.assertIn("defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)", timing)
        profile = (ROOT / "platformio.ini").read_text().split("[env:litch-esp32s3]", 1)[1]
        for symbol in ("app_main", "initArduino"):
            self.assertIn(f"-Wl,--wrap={symbol}\n", profile)
        header = (ROOT / "include/firmware/boot_platform_timing.hpp").read_text()
        self.assertIn("UNOBSERVED_MS = UINT32_MAX", header)
        support = (ROOT / "src/firmware_console.cpp").read_text().split(
            '    console.RegisterCommand("support",', 1)[1]
        self.assertIn('"boot_platform: clock=firmware_ms unobserved=%lu', support)
        self.assertIn('"boot_startup: power_to_light=unmeasured', support)

    def test_native_usb_recovery_is_reached_before_startup_and_recovery_exits(self):
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        setup = boot.split("void SetupApp() {", 1)[1]
        self.assertLess(setup.index("BeginNativeUsbResetRecovery();"),
                        setup.index("Serial.begin(115200);"))
        runtime = (ROOT / "src/firmware_app_runtime.cpp").read_text()
        loop = runtime.split("void LoopApp() {", 1)[1]
        self.assertLess(loop.index("PollNativeUsbResetRecovery();"),
                        loop.index("GetSystem().telemetry.OnLoop();"))
        self.assertNotIn("NativeUsbDeepSleepRecovery", boot + runtime)

    def test_settings_load_does_not_own_a_second_rtc_crash_loop_guard(self):
        load = (ROOT / "lib/foxie_embedded/src/settings_store_load.cpp").read_text()
        io = (ROOT / "lib/foxie_embedded/src/settings_store_io.cpp").read_text()
        internal = (ROOT / "lib/foxie_embedded/src/settings_store_internal.hpp").read_text()
        combined = load + io + internal
        self.assertNotIn("SettingsLoadCrashLoop", combined)
        self.assertNotIn("SETTINGS_CRASH_LOOP_MAGIC", combined)
        self.assertNotIn("crash loop detected; settings disabled", load)
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        self.assertIn("sys.bootGuard.IsSafeModeActive()", boot)
        self.assertIn("if (sys.safeModeActive)", boot)

    def test_ambient_begin_owns_the_only_boot_power_on(self):
        boot = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        setup = boot.split("void FinishStartup() {", 1)[1]
        self.assertEqual(setup.count("sys.ambientLights.Begin(Wire);"), 2)
        factory, normal = setup.split("    sys.gamePersistence.SetRngSeed(sys.rngSeed);", 1)
        self.assertEqual(factory.count("sys.ambientLights.Begin(Wire);"), 1)
        self.assertEqual(normal.count("sys.ambientLights.Begin(Wire);"), 1)
        self.assertFalse("sys.ambientLights.PowerOn();" in setup,
                         "Begin already owns the boot power-on transition")
        driver = (ROOT / "src/ambient_light_sensors.cpp").read_text()
        begin = driver.split("void AmbientLightSensors::Begin(TwoWire& wire)", 1)[1]
        begin = begin.split("void AmbientLightSensors::PowerOn()", 1)[0]
        self.assertEqual(begin.count("PowerOn();"), 1)

    def test_first_nonzero_diagnostic_does_not_claim_visibility(self):
        output = (ROOT / "src/firmware_led_boot_receipt.cpp").read_text()
        self.assertFalse("first_visible_led_frame_ms" in output,
                         "a render estimate is not acknowledged visibility")
        self.assertIn("first_nonzero_render_return_ms=%lu", output)
        self.assertIn("render_start_ms=%lu", output)
        self.assertIn("effective_scale_256=%u", output)
        self.assertIn("estimated_current_ma=%u", output)
        # The synchronous renderer was retired. The production startup journey
        # still covers accepted/rejected transfers; runtime consumes receipts.
        render = (ROOT / "src/firmware_tasks_render.cpp").read_text()
        consumed = render.split("void ConsumeLedPresentation(FirmwareSystem& sys) {", 1)[1]
        consumed = consumed.split("bool StartupPresentationComplete(", 1)[0]
        self.assertLess(consumed.index("if (!shown.completed) return;"),
                        consumed.index("RecordBootRenderReturn(sys,"))

    def test_palette_repeats_preserve_confirmed_startup(self):
        # Control the interval before the LED task can publish another receipt.
        # Exercise the production handoff used by both physical and app writes.
        render = (ROOT / "src/firmware_tasks_render.cpp").read_text()
        bodies = []
        for signature in ("void CancelBootPresentation(", "bool StartupPresentationComplete("):
            start = render.index(signature)
            end = render.index("{", start) + 1
            depth = 1
            while depth:
                depth += (render[end] == "{") - (render[end] == "}")
                end += 1
            bodies.append(render[start:end])

        fixture = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct BootDisplay {
    enum class State { Active, Cancelled, Retired };
    State state = State::Active;
    bool IsActive() const { return state == State::Active; }
    bool IsPending() const { return false; }
    bool IsComplete() const { return !IsActive(); }
    bool WasCancelled() const { return state == State::Cancelled; }
    void Cancel() { if (IsActive()) state = State::Cancelled; }
};

struct LedAnimationReceipt {
    bool completed = false;
    uint32_t bootCancelEpoch = 0;
    bool startupComplete = false;
};

struct Animation {
    LedAnimationReceipt shown;
    unsigned cancelRequests = 0;
    LedAnimationReceipt Receipt() const { return shown; }
    void CancelBoot() { ++cancelRequests; }
};

struct FirmwareSystem {
    BootDisplay bootDisplay;
    Animation ledAnimation;
    uint32_t ledBootCancelEpoch = 0;
};

bool StartupPresentationComplete(const FirmwareSystem& sys);
"""
        journey = r"""
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::exit(EXIT_FAILURE);
}

int main() {
    FirmwareSystem sys;
    sys.ledAnimation.shown.startupComplete = true;
    Check(!StartupPresentationComplete(sys), "an unconfirmed frame cannot finish startup");
    sys.ledAnimation.shown = {true, 0, false};
    Check(!StartupPresentationComplete(sys), "a confirmed startup frame can still be incomplete");

    // Physical input can cancel the main-side BootDisplay before the worker is notified.
    sys.bootDisplay.Cancel();
    CancelBootPresentation(sys);
    Check(sys.ledAnimation.cancelRequests == 1 && sys.ledBootCancelEpoch == 1,
        "main-side cancellation must still reach the LED task");
    sys.ledAnimation.shown = {true, 0, true};
    Check(!StartupPresentationComplete(sys), "a stale receipt cannot confirm startup after cancellation");

    sys.bootDisplay.state = BootDisplay::State::Retired;
    sys.ledAnimation.shown = {true, 1, true};
    Check(StartupPresentationComplete(sys), "the matching completed receipt finishes startup");
    for (int repeat = 0; repeat < 3; ++repeat) {
        CancelBootPresentation(sys);
        Check(StartupPresentationComplete(sys), "a palette repeat reopened confirmed startup");
        Check(sys.ledAnimation.cancelRequests == 1 && sys.ledBootCancelEpoch == 1,
            "a palette repeat requested cancellation after startup finished");
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="litch-startup-receipt-") as tmp:
            cpp = Path(tmp) / "startup_receipt.cpp"
            binary = Path(tmp) / "startup_receipt"
            cpp.write_text("\n".join([fixture, *bodies, journey]))
            result = subprocess.run(
                ["c++", "-std=c++17", str(cpp), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_services_follow_the_completed_fade(self):
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        setup = boot.split("void SetupApp() {", 1)[1].split("void AdvanceStartup()", 1)[0]
        order = (
            "EnsureSettingsLoaded();",
            "MaybeRunFactoryResetBootFlow(sys);",
            "InitializeLedPowerBudgetAfterSettingsLoad(sys);",
            "EnsurePowerRailInitialized(sys);",
            "leds.Begin();",
            "sys.startupLeds.Begin(",
        )
        positions = [setup.index(call) for call in order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn(
            "sys.startupLeds.Begin(\n"
            "        firmware::StartupLedSequence::UsesPreServiceGrid(hardwareProfile.revision) &&\n"
            "        modules::white_squares_setting::ReadWhiteSquaresOn(sys.settings) &&\n"
            "        modules::white_squares_setting::ReadBrightness0To8(sys.settings) != 0U);",
            setup,
        )
        early = (ROOT / "src/firmware_update_runtime.cpp").read_text()
        self.assertLess(setup.index("InitializeEarlyUpdater(sys);"),
                        setup.index("EnsureSettingsLoaded();"))
        self.assertLess(setup.index("InitializeEarlyUpdater(sys);"),
                        setup.index("if (sys.safeModeActive)"))
        for call in ("InitializeEsp32OtaPlatformAdapter(", "CheckPendingOtaAtBoot("):
            self.assertIn(call, early)
            self.assertNotIn(call, setup)
        for call in ("LoadGameForPersistenceSession(",
                     "sys.motionWake.Begin(", "sys.boardInput.Begin();",
                     "sys.ambientLights.Begin(Wire);", "sys.ble.Begin(sys.settings);"):
            self.assertNotIn(call, setup)
            self.assertIn(call, services)
        advance = boot.split("void AdvanceStartup() {", 1)[1]
        self.assertIn("sys.startupLeds.Update(", advance)
        self.assertLess(advance.index("ComposeStartupGrid(sys.settings, startupFrame);"),
                        advance.index("RenderPhysicalFrameWithConservativePowerBudget("))
        self.assertIn("sys.Leds().LastShowCompleted()", advance)
        self.assertIn("FinishStartup();", advance)
        runtime = (ROOT / "src/firmware_app_runtime.cpp").read_text()
        self.assertLess(runtime.index("AdvanceStartup();"),
                        runtime.index("EnsureConsoleInitialized();"))
        sequence = (ROOT / "include/firmware/startup_led_sequence.hpp").read_text()
        self.assertLess(sequence.index("if (!render(fadeScale256))"),
                        sequence.index("initialize();"))
        self.assertIn("if (fadeScale256 == 256U)", sequence)
        self.assertLess(setup.index("sys.rngSeed = ComputeBootRngSeed();"),
                        setup.index("EnsureSettingsLoaded();"))
        # Normal startup can overlap BLE only after the production restore,
        # physical observation and storage owners can supply truthful frames.
        # This source seam does not establish physical boot latency.
        normal = services.split("    sys.gamePersistence.SetRngSeed(sys.rngSeed);", 1)[1]
        order = [normal.index(call) for call in (
            "modules::LoadGameForPersistenceSession(",
            "sys.boardInput.Begin();",
            "sys.boardInput.TryReadSnapshot(rawOcc64)",
            "sys.settingsPersistence.Begin(sys.settings)",
            "ConfigureLitchcraftPersistence(sys);",
            "sys.bootDisplay.Begin(",
            "const auto firstSemanticRender = RenderLeds(true);",
            "sys.settingsPersistence.NotifyLitchcraftFrameRuntimeReady();",
            "sys.ble.Begin(sys.settings);",
            "const auto servicesRender = RenderLeds(true);",
            "firmware::RecordUsbBootServicesReady();",
        )]
        self.assertEqual(order, sorted(order))
        early_v33 = normal.split(
            "if (hardwareProfile.revision == hardware::BoardRevision::V3_3S3) {", 1)[1]
        early_v33 = early_v33.split(
            "    runtime::NoteProgress(0U, runtime::ProgressStage::Ble);", 1)[0]
        self.assertIn("const auto firstSemanticRender = RenderLeds(true);", early_v33)
        self.assertIn("if (sys.ledAnimation.IsStarted()) {\n"
                      "            sys.settingsPersistence.NotifyLitchcraftFrameRuntimeReady();", early_v33)
        self.assertEqual(normal.count("sys.bootDisplay.Begin("), 1)
        self.assertIn("!sys.gamePersistence.Litchcraft().IsInitialStoreModePending()", normal)

    def test_artwork_restore_does_not_enqueue_a_boot_settings_write(self):
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        begin = services.index("sys.settingsPersistence.Begin(")
        restore = services.index("ConfigureLitchcraftPersistence(sys);")
        self.assertLess(begin, restore)
        self.assertNotIn("sys.settingsPersistence.Enqueue(sys.settings.Doc())", services,
                         "ordinary boot keeps restored state in RAM until a user-owned mutation")

    def test_support_runtime_binds_loaded_normal_boot_before_worker(self):
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        ordinary = services.split("    sys.gamePersistence.SetRngSeed(sys.rngSeed);", 1)[1]
        self.assertIn("if (sys.settingsLoadAttempted && sys.settingsLoadSucceeded)", ordinary)
        self.assertLess(ordinary.index("ConfigureBoardIdentityPersistence(sys);"),
                        ordinary.index("sys.supportRuntimeStats.BeginBoot(sys.settings.Doc());"))
        self.assertLess(ordinary.index("sys.supportRuntimeStats.BeginBoot(sys.settings.Doc());"),
                        ordinary.index("sys.settingsPersistence.SetSupportRuntimeStats(&sys.supportRuntimeStats);"))
        self.assertLess(ordinary.index("sys.settingsPersistence.SetSupportRuntimeStats(&sys.supportRuntimeStats);"),
                        ordinary.index("sys.settingsPersistence.Begin("))
        early = (ROOT / "src/firmware_update_runtime.cpp").read_text()
        self.assertNotIn("supportRuntimeStats.BeginBoot", early)

    def test_driver_reports_started_and_settled_output(self):
        output = (ROOT / "src/neopixel_led_renderer.cpp").read_text()
        show = output.split("void NeoPixelLedRenderer::Show() {", 1)[1]
        self.assertTrue("m_lastShowCompleted = false;" in show)
        self.assertTrue("m_lastShowCompleted = true;" in show)
        self.assertIn("const bool boardUnchanged = !m_boardStrip.IsDirty();", show)
        self.assertIn("!m_splitStrips || !m_edgeStrip.IsDirty()", show)
        self.assertLess(show.index("!WaitCanShowTrueBounded(m_boardStrip"),
                        show.index("const bool boardUnchanged"))
        self.assertNotIn("portENTER_CRITICAL", show)
        begin = output.split("void NeoPixelLedRenderer::Begin() {", 1)[1]
        begin = begin.split("void NeoPixelLedRenderer::Clear()", 1)[0]
        self.assertIn("Show();", begin)
        self.assertNotIn("m_boardStrip.Show();", begin)
        self.assertLess(show.index("!boardStarted || !edgeStarted || !boardCompleted || !edgeCompleted"),
                        show.rindex("m_lastShowCompleted = true;"))

    def test_faulted_startup_keeps_restricted_console(self):
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        self.assertIn("MaybeEnableConsoleOutput();", boot)
        self.assertIn("console->Update();", boot)
        console = (ROOT / "src/firmware_console.cpp").read_text()
        self.assertIn("!sys.safeModeActive && !sys.startupLeds.IsReady() &&", console)
        self.assertIn("firmware::HandleStartupConsoleLine(", console)
        self.assertIn("firmware_internal::IsLedBrownoutLimitLatched(sys.settings) &&\n"
                      "                sys.settings.IsPersistenceGuardArmed()", console)
        source = (ROOT / "src/firmware_led_power_source.cpp").read_text()
        initialization = source.split("void InitializeLedPowerBudgetAfterSettingsLoad", 1)[1].split("bool ClearLedPowerProtectionLatch", 1)[0]
        self.assertNotIn("RefreshLedPowerSourceStatus(", initialization)
        self.assertIn("SetLedBrownoutLimitLatched(sys.settings, true)", initialization)

    def test_factory_boot_returns_before_ordinary_owners(self):
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        self.assertIn("    if (sys.factoryBootOnly) {", services)
        branch = services.split("    if (sys.factoryBootOnly) {", 1)[1].split(
            "    sys.gamePersistence.SetRngSeed(sys.rngSeed);", 1)[0]
        self.assertIn("        return;", branch)
        for required in ("ConfigureBoardIdentityPersistence(sys);", "sys.boardInput.Begin();",
                         "sys.ambientLights.Begin(Wire);", "sys.ble.Begin(sys.settings);",
                         "sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision"):
            self.assertIn(required, branch)
        for omitted in ("LoadGameForPersistenceSession", "sys.controlMutation.Configure",
                        "sys.currentGameSync.Configure", "sys.settingsPersistence.Begin",
                        "ConfigureLitchcraftPersistence", "sys.gamePersistence."):
            self.assertNotIn(omitted, branch)
        self.assertLess(services.index("InitializeEarlyUpdater(sys);"), services.index("if (sys.factoryBootOnly)"))
        runtime = (ROOT / "src/firmware_app_runtime.cpp").read_text()
        self.assertLess(runtime.index("LoopFactoryRuntime(sys);"),
                        runtime.index("NotifyLitchcraftFrameRuntimeReady();"))
        factory_loop = (ROOT / "src/firmware_factory_runtime.cpp").read_text()
        self.assertNotIn("gamePersistence", factory_loop)
        self.assertNotIn("stateMachine", factory_loop)
        self.assertIn("PollRuntimeBootHealth(sys);", factory_loop)
        self.assertIn("MaybeEnableConsoleOutput();", factory_loop,
                      "minimal factory mode must retain diagnostic logger output")
        self.assertLess(factory_loop.index("MaybeEnableConsoleOutput();"),
                        factory_loop.index("console->Update();"),
                        "minimal factory loop must publish and drain the common logger handoff before servicing console input")

    def test_factory_input_and_render_do_not_touch_uninitialized_game(self):
        buttons = (ROOT / "src/firmware_tasks_buttons.cpp").read_text()
        self.assertIn("    if (sys.factoryBootOnly) {", buttons)
        branch = buttons.split("    if (sys.factoryBootOnly) {", 1)[1].split(
            "    runtime::InteractionDispatcher", 1)[0]
        self.assertNotIn("gamePersistence", branch)
        self.assertIn("sys.hardwareTest.OnButtonEvent(event, nowMs);", branch)
        self.assertIn("return;", branch)
        render = (ROOT / "src/firmware_tasks_render.cpp").read_text()
        # Scene capture replaces the old synchronous renderer's guard spelling.
        factory, ordinary = render.split(
            "} else if (sys.hardwareTest.IsActive() || sys.factoryBootOnly) {", 1)[1].split(
                "} else {", 1)
        self.assertIn("CaptureTest(sys, test);", factory)
        self.assertIn("presentation.scene = test;", factory)
        self.assertNotIn("gamePersistence", factory)
        self.assertIn("sys.gamePersistence.PresentPending", ordinary)
        console = (ROOT / "src/firmware_console.cpp").read_text()
        branch = console.split("    if (sys.factoryBootOnly) {", 2)[2].split(
            '    console.RegisterCommand("support",', 1)[0]
        self.assertNotIn("gamePersistence", branch)
        self.assertNotIn("stateMachine", branch)
        self.assertIn("initialized = true;\n        return;", branch)
        self.assertIn("ordinary game/settings commands unavailable", console)

    def test_factory_save_and_normal_reboot_have_owned_guards(self):
        factory = (ROOT / "src/firmware_hardware_test_runtime.cpp").read_text()
        self.assertIn("record.factoryVerifierEnabled = built &&", factory)
        self.assertIn("record.radioResult == modules::ManufacturingTestRadioResult::PeerVerified &&", factory)
        self.assertIn("sys.hardwareTest.IsAutomaticVerifier();", factory)
        self.assertIn("if (complete && sys.factoryBootOnly && !sys.factoryVerifierBoot)", factory)
        self.assertIn("sameRun", factory)
        self.assertIn("const bool complete = adopted && (provisioningPassed", factory)
        self.assertIn("sys.factoryRadio.HasPendingTerminalReply()", factory)
        self.assertLess(factory.index("sys.factoryRadio.Update(false, false"),
                        factory.index("sys.settingsPersistence.Begin(sys.settings"))
        self.assertIn("sys.factoryVerifierBoot && hardwareTestSupported", factory)
        reboot = (ROOT / "src/firmware_console_reboot.cpp").read_text()
        self.assertIn("sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;", reboot)
        self.assertIn("outcome == Outcome::Saved) Restart", reboot)
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        self.assertIn("esp_reset_reason() == ESP_RST_SW", boot)
        self.assertIn("ConsumeFactoryBootRequest", boot)
        self.assertNotIn("sys.gamePersistence.SetRngSeed", boot)
        stack = (ROOT / "src/ble_service_stack.cpp").read_text()
        self.assertIn("m_factorySavedVerifierRequested && !ConfigureFactoryRadioEndpoint(*settings)", stack)
        for updater in ("m_otaControlCharacteristic", "m_otaDataCharacteristic",
                        "m_connectionSessionIngressCharacteristic", "m_settingsService"):
            self.assertIn(updater, stack)

    def test_requested_factory_boot_waits_for_the_existing_ota_preflight(self):
        # The old source queried the unstarted OTA worker in SetupApp and
        # discarded every S3 RTC request as recovery. Check the production
        # call order, supplementing native fail-closed policy and physical boot.
        boot = (ROOT / "src/firmware_app_boot.cpp").read_text()
        setup = boot.split("void SetupApp() {", 1)[1]
        self.assertNotIn("ReadEsp32OtaRunningState", setup)
        self.assertNotIn("ConsumeFactoryBootRequest", setup)
        admission = boot.split("void ApplyRequestedFactoryBoot(FirmwareSystem& sys) {", 1)[1].split(
            "void SetupApp() {", 1)[0]
        self.assertIn("sys.safeModeActive || sys.otaBootAwaitingHealth", admission)
        self.assertIn("sys.factoryVerifierBoot = !sys.startupHardwareTestRequested &&", admission)
        self.assertIn("ConsumeFactoryBootRequest", admission)
        self.assertNotIn("ReadEsp32OtaRunningState", admission)
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        finish = services.split("void FinishStartup() {", 1)[1]
        # The minimum shared updater now runs before settings/recovery, and
        # explicit service entry retries a failed worker before using its state.
        # Actual-source early/runtime fixtures cover unavailable worker and
        # normal acceptance; this source check preserves only orchestration.
        early = (ROOT / "src/firmware_update_runtime.cpp").read_text()
        order = [early.index(token) for token in (
            "firmware::InitializeEsp32OtaPlatformAdapter(",
            "firmware::ReadEsp32OtaRunningState(runningState)",
            "sys.otaBootAwaitingHealth = modules::RequiresOtaRuntimeAcceptance(",
            "firmware::CheckPendingOtaAtBoot(")]
        self.assertEqual(order, sorted(order))
        order = [finish.index(token) for token in (
            "InitializeEarlyUpdater(sys);", "ApplyRequestedFactoryBoot(sys);",
            "if (sys.factoryBootOnly) {")]
        self.assertEqual(order, sorted(order))
        self.assertEqual(finish.count("ApplyRequestedFactoryBoot(sys);"), 1)

    def test_ota_health_result_returns_to_the_normal_sleep_gate(self):
        # Check the runtime-to-sleep wiring only. The actual-source native
        # boot_health_runtime_sources.cpp tests false before verified health,
        # true after confirmation, and false in safe mode; do not duplicate
        # that behavior with an assertion about the helper's local spelling.
        declarations = (ROOT / "include/firmware/firmware_app_internal.hpp").read_text()
        health = (ROOT / "src/firmware_boot_health_runtime.cpp").read_text()
        normal = (ROOT / "src/firmware_app_runtime.cpp").read_text()
        factory = (ROOT / "src/firmware_factory_runtime.cpp").read_text()
        self.assertIn("bool PollRuntimeBootHealth(FirmwareSystem& sys);", declarations)
        self.assertIn("bool PollRuntimeBootHealth(FirmwareSystem& sys) {", health)
        self.assertIn("const bool otaHealthCommitFinished = PollRuntimeBootHealth(sys);", normal)
        self.assertIn(".otaHealthConfirmationPending = !otaHealthCommitFinished,", normal)
        self.assertIn("(void)PollRuntimeBootHealth(sys);", factory)
        services = (ROOT / "src/firmware_app_boot_services.cpp").read_text()
        self.assertNotIn("PreparePendingOtaBootHealth", services)
        self.assertIn("sys.ble.ReserveBootFirmwareDigest();", services)
        normal_health = health.split("    if (sys.otaBootAwaitingHealth && retryDue)", 1)[1]
        self.assertIn("retryDue && startupLightingReady", normal_health)
        self.assertIn("startupLightingReady &&\n#endif\n", normal_health)
        self.assertIn("!sys.gamePersistence.Litchcraft().IsInitialStoreModePending()", health)
        self.assertIn("sys.bootFirstNonzeroRenderLogged", health)
        self.assertIn("state.boundaryReached = modeResolved &&", health)
        renderer = (ROOT / "src/led_animation_renderer.cpp").read_text()
        self.assertIn("!presentation.initialStoreModePending && StartupComplete()", renderer)

    def test_factory_completion_uses_post_ack_and_fresh_reboot_clock(self):
        loop = (ROOT / "src/firmware_factory_runtime.cpp").read_text()
        self.assertIn("sys.factoryCompletion.NoteFrameAcknowledged(", loop)
        self.assertIn("PollFactoryCompletionReboot(sys);", loop)
        self.assertNotIn("factoryCompletionFrameMs = nowMs", loop)
        self.assertLess(loop.index("const auto result = RenderLeds(sys.factoryCompletion.IsPending());"),
                        loop.index("sys.factoryCompletion.NoteFrameAcknowledged("))
        gate = (ROOT / "include/firmware/factory_completion_reboot.hpp").read_text()
        self.assertIn("m_acknowledgedAtMs = fxe::Millis();", gate)
        self.assertIn("fxe::Millis() - m_acknowledgedAtMs", gate)
        self.assertIn("!operationBusy && otaHealthy", gate)
        owner = (ROOT / "src/firmware_hardware_test_runtime.cpp").read_text()
        self.assertIn("if (complete && sys.factoryBootOnly && !sys.factoryVerifierBoot)", owner)
        self.assertIn("sys.factoryCompletion.Begin();", owner)
        self.assertIn("const bool complete = adopted && (provisioningPassed", owner)
        self.assertIn("sameRun", owner)
        self.assertIn("sys.factoryCompletion.IsReady(operationBusy, otaHealthy)", owner)
        self.assertIn("sys.factoryRadio.HasPendingTerminalReply()", owner)


if __name__ == "__main__":
    unittest.main()
