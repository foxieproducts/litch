#include "runtime/panic_diagnostic.hpp"
#if defined(ARDUINO) && defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
#include <string_view>
#include "foxie_embedded/console.hpp"
#include "runtime/panic_capture_internal.hpp"
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if defined(LITCH_ESP32)
#include <esp_task.h>
#include "firmware/led_animation_task.hpp"
#endif
#endif

namespace runtime::panic_diagnostic {
namespace {
void PrintStackProbe(fxe::Console& console) {
    const auto& probe = g_stackProbe;
    if (!ProbeValid(probe)) {
        console.PrintLn("panic_probe: state=unavailable missing_or_partial=inconclusive");
        return;
    }
    const bool bound = ProbeBound(probe, g_capsule);
    console.PrintLn("panic_probe: state=%s token=%08lx pan_checksum=%08lx pan_pc=%08lx flags=%lu trial=%s age=%s",
                    probe.magic == PROBE_ARMED ? "armed" : bound ? "captured_bound" : "captured_unbound",
                    static_cast<unsigned long>(probe.token), static_cast<unsigned long>(probe.panChecksum),
                    static_cast<unsigned long>(probe.panPc), static_cast<unsigned long>(probe.flags),
                    probe.token == 0U ? "unarmed_no_attribution" : "token_requires_external_record",
                    probe.token == 0U ? "unknown" : "external_trial_required");
    if (!bound) return;
    console.PrintLn("panic_probe: saved_context=%s ps=%08lx sar=%08lx registers=raw_no_window_flush",
                    (probe.flags & ProbeInvalidFrame) == 0U ? "available" : "unavailable",
                    static_cast<unsigned long>(probe.ps), static_cast<unsigned long>(probe.sar));
    if ((probe.flags & ProbeInvalidFrame) == 0U) {
        for (unsigned i = 0U; i < 14U; ++i) {
            console.PrintLn("panic_probe: a%u=%08lx", i + 2U, static_cast<unsigned long>(probe.a[i]));
        }
    }
    console.PrintLn("panic_probe: raw_stack=%s address=%08lx words=%lu clipped=%u task_identity=unknown",
                    (probe.flags & ProbeInvalidStack) == 0U ? "available" : "unavailable",
                    static_cast<unsigned long>(probe.stackAddress), static_cast<unsigned long>(probe.stackWords),
                    static_cast<unsigned>((probe.flags & ProbeStackClipped) != 0U));
    if ((probe.flags & ProbeInvalidStack) != 0U) return;
    const unsigned count = probe.stackWords < PROBE_STACK_WORDS ? probe.stackWords : PROBE_STACK_WORDS;
    for (unsigned i = 0U; i < count;) {
        if (count - i >= 4U) {
            console.PrintLn("panic_probe: stack_at=%08lx raw=%08lx,%08lx,%08lx,%08lx",
                            static_cast<unsigned long>(probe.stackAddress + i * 4U),
                            static_cast<unsigned long>(probe.stack[i]), static_cast<unsigned long>(probe.stack[i + 1U]),
                            static_cast<unsigned long>(probe.stack[i + 2U]), static_cast<unsigned long>(probe.stack[i + 3U]));
            i += 4U;
        } else {
            console.PrintLn("panic_probe: stack_at=%08lx raw=%08lx",
                            static_cast<unsigned long>(probe.stackAddress + i * 4U),
                            static_cast<unsigned long>(probe.stack[i]));
            ++i;
        }
    }
}
bool ParseProbeHex(const std::string_view text, uint32_t& value) {
    auto digits = text;
    if (digits.size() >= 2U && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.remove_prefix(2U);
    if (digits.empty() || digits.size() > 8U) return false;
    value = 0U;
    for (const char c : digits) {
        unsigned digit = 16U;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10U;
        if (digit >= 16U) return false;
        value = (value << 4U) | digit;
    }
    return true;
}
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC)
struct TaskStackBudget {
    const char* name;
    uint32_t configuredBytes;
};
constexpr TaskStackBudget HOST_STACK{"nimble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE};
#if defined(LITCH_ESP32)
constexpr TaskStackBudget TASK_STACKS[]{
    {"loopTask", CONFIG_ARDUINO_LOOP_STACK_SIZE},
    {"led_animation", firmware::LedAnimationTask::TASK_STACK_BYTES},
    {"cpu_think", 8192U},          // AsyncThinkWorker
    {"settings_io", 7168U},        // SettingsPersistenceWorker
    {"ota_boot", 4096U},           // OtaPostBootWorker
    {"factory_ble", 6144U},        // FactoryBleDutTask
    {"btController", ESP_TASK_BT_CONTROLLER_STACK},
    {"usbd", 4096U},               // Arduino TinyUSB device task
    {"arduino_usb_eve", 2048U},    // Arduino USB event task, truncated by FreeRTOS
    {"esp_timer", ESP_TASK_TIMER_STACK},
    {"ipc0", CONFIG_ESP_IPC_TASK_STACK_SIZE},
    {"ipc1", CONFIG_ESP_IPC_TASK_STACK_SIZE},
    {"IDLE0", CONFIG_FREERTOS_IDLE_TASK_STACKSIZE},
    {"IDLE1", CONFIG_FREERTOS_IDLE_TASK_STACKSIZE},
};
static_assert(configMAX_TASK_NAME_LEN == 16U, "verify SDK task names in the private stack census");
#endif

bool PrintTaskStackBudget(fxe::Console& console, const char* record, const TaskStackBudget& budget) {
    // The loop owns radio teardown. Read from its console after the workload.
    static_assert(sizeof(StackType_t) == 1U, "pinned ESP-IDF watermark units are bytes");
    const TaskHandle_t task = xTaskGetHandle(budget.name);
    const auto configuredBytes = static_cast<unsigned long>(budget.configuredBytes);
    if (task == nullptr) {
        console.PrintLn("%s: task=%s found=0 configured_bytes=%lu", record, budget.name, configuredBytes);
        return false;
    }
    const UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(task);
    if (freeBytes > budget.configuredBytes) {
        console.PrintLn("%s: task=%s found=1 configured_bytes=%lu valid=0 minimum_free_bytes=%lu used_pattern_bytes=unknown",
                        record, budget.name, configuredBytes,
                        static_cast<unsigned long>(freeBytes));
        return true;
    }
    console.PrintLn("%s: task=%s found=1 configured_bytes=%lu valid=1 minimum_free_bytes=%lu used_pattern_bytes=%lu tcb=%08lx",
                    record, budget.name, configuredBytes,
                    static_cast<unsigned long>(freeBytes),
                    static_cast<unsigned long>(budget.configuredBytes - freeBytes),
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(task)));
    return true;
}

#if defined(LITCH_ESP32)
void PrintTaskStackBudgets(fxe::Console& console) {
    // Name lookups cover tasks with stable handles on this loop. The two counts
    // bracket the read for reconciliation and may include transient tasks.
    const UBaseType_t before = uxTaskGetNumberOfTasks();
    unsigned knownPresent = 0U;
    console.BeginBulkOutput();
    for (const auto& budget : TASK_STACKS) {
        knownPresent += PrintTaskStackBudget(console, "task_stack", budget);
    }
    knownPresent += PrintTaskStackBudget(console, "host_budget", HOST_STACK);
    console.PrintLn("task_stack: task=main configured_bytes=%lu observed=0 reason=transient_boot_task",
                    static_cast<unsigned long>(ESP_TASK_MAIN_STACK));
    console.PrintLn("task_stack: task=fw_digest configured_bytes=4096 observed=0 source=ota_image_log");
    console.PrintLn("task_stack: scope=interrupt configured_bytes_per_core=%lu observed=0",
                    static_cast<unsigned long>(CONFIG_FREERTOS_ISR_STACKSIZE));
    console.PrintLn("task_stack_census: known_present=%u rtos_count_before=%lu rtos_count_after=%lu scope=stable_known_names",
                    knownPresent, static_cast<unsigned long>(before),
                    static_cast<unsigned long>(uxTaskGetNumberOfTasks()));
    console.EndBulkOutput();
}
#endif
#endif

bool StackProbeCommand(fxe::Console& console, const fxe::Console::Args& args) {
    if (!args.Has(1U) || (args.Get(1U) == "status" && !args.Has(2U))) {
        PrintRetained(console);
        return true;
    }
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC) && defined(LITCH_ESP32)
    if (!args.overflow && args.Get(1U) == "tasks" && !args.Has(2U)) {
        PrintTaskStackBudgets(console);
        return true;
    }
#endif
    uint32_t token = 0U, checksum = 0U, pc = 0U;
    if (args.overflow || args.Get(1U) != "arm" || !args.Has(4U) || args.Has(5U) ||
        !ParseProbeHex(args.Get(2U), token) || !ParseProbeHex(args.Get(3U), checksum) ||
        !ParseProbeHex(args.Get(4U), pc)) {
        console.PrintLn("usage: panicprobe [status|"
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC) && defined(LITCH_ESP32)
                        "tasks|"
#endif
                        "arm <increasing_token_hex> <current_pan_checksum_hex> <current_pan_pc_hex>]");
        return false;
    }
    const bool panValid = Valid(g_capsule);
    if (token == 0U || (ProbeValid(g_stackProbe) && token <= g_stackProbe.token)) {
        console.PrintLn("panic_probe: rejected=token_must_increase_nonzero retained_unchanged=1");
        return false;
    }
    if ((panValid && (checksum != g_capsule.checksum || pc != g_capsule.pc)) ||
        (!panValid && (checksum != 0U || pc != 0U))) {
        console.PrintLn("panic_probe: rejected=stale_pan_baseline retained_unchanged=1");
        return false;
    }
    // Operator must retain this output and exact ELF externally before the
    // one-shot reset trial. No hidden clear on boot, query or support read.
    PrintRetained(console);
    auto* words = reinterpret_cast<volatile uint32_t*>(&g_stackProbe);
    for (unsigned i = 0U; i < sizeof(StackProbe) / 4U; ++i) words[i] = 0U;
    g_stackProbe.token = token;
    g_stackProbe.panChecksum = checksum;
    g_stackProbe.panPc = pc;
    PublishProbe(g_stackProbe, PROBE_ARMED);
    // If interrupted before this clear, the old PAN2 remains and the extension
    // is merely armed. Neither reader can label that old fault as fresh.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_capsule.magic = 0U;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    console.PrintLn("panic_probe: armed=1 token=%08lx pan_cleared=1 next_action=one_authorized_boot_trial retain_prior_output_and_exact_elf=required",
                    static_cast<unsigned long>(token));
    return true;
}
const char* TaskName(const uint32_t task) {
    return task == 0U ? "ota_record_io" : task == 1U ? "fw_digest" : "unknown";
}
const char* RequestName(const unsigned task, const uint32_t request) {
    if (task == 1U) return request == 1U ? "image_hash" : "none";
    switch (request) {
        case 1U: return "load";
        case 2U: return "save_pending";
        case 3U: return "save_phase";
        case 4U: return "running_state";
        case 5U: return "running_image";
        case 6U: return "mark_valid";
        case 7U: return "mark_invalid";
        default: return "none";
    }
}
const char* StepName(const unsigned task, const uint32_t step) {
    if (task == 1U) return step == 1U ? "compute_start" : step == 2U ? "compute_end" : "none";
    switch (step) {
        case 1U: return "request_start";
        case 2U: return "process";
        case 3U: return "initial_state";
        case 4U: return "image_hash_start";
        case 5U: return "recheck_state";
        case 6U: return "trial_marker";
        case 7U: return "request_end";
        case 8U: return "image_hash_end";
        case 9U: return "reacquire";
        default: return "none";
    }
}
void PrintObservation(fxe::Console& console, const char* source, const unsigned task,
                      const volatile Observation& observation) {
    Observation copy{};
    const auto* words = reinterpret_cast<const volatile uint32_t*>(&observation);
    auto* target = reinterpret_cast<uint32_t*>(&copy);
    const uint32_t before = words[0];
    for (unsigned i = 0U; i < sizeof(copy) / 4U; ++i) target[i] = words[i];
    const bool stable = (before & 1U) == 0U && before == words[0];
    if (copy.revision == 0U) {
        console.PrintLn("panic_diag: source=%s task=%s observed=0 watermark=unavailable",
                        source, TaskName(task));
        return;
    }
    console.PrintLn("panic_diag: source=%s task=%s request=%s step=%s stable=%u result=%lu",
                    source, TaskName(task), RequestName(task, copy.request),
                    StepName(task, copy.step), static_cast<unsigned>(stable), static_cast<unsigned long>(copy.result));
    console.PrintLn("panic_diag: source=%s task=%s at=%lu start=%lu end=%lu stack_free_bytes=%lu tcb=%08lx",
                    source, TaskName(task), static_cast<unsigned long>(copy.atMs),
                    static_cast<unsigned long>(copy.startedAtMs), static_cast<unsigned long>(copy.endedAtMs),
                    static_cast<unsigned long>(copy.watermarkBytes), static_cast<unsigned long>(copy.task));
}
}
void RegisterStackProbeCommand(fxe::Console& console) {
    if (!console.RegisterCommand("panicprobe", "Private boot panic probe: status | "
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC) && defined(LITCH_ESP32)
            "tasks | "
#endif
            "arm <increasing_token_hex> <current_pan_checksum_hex> <current_pan_pc_hex>",
            [&console](const fxe::Console::Args& args) { return StackProbeCommand(console, args); })) {
        console.PrintLn("panic_probe: registration=failed");
    }
}
void PrintRetained(fxe::Console& console) {
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
    const auto& entry = litch_boot_entry_witness;
    console.PrintLn("boot_entry_diag: valid=%u token=%08lx inverse=%08lx stage=%lu stage_known=%u age=external_trial_required missing=inconclusive",
                    static_cast<unsigned>(EntryWitnessValid(entry)),
                    static_cast<unsigned long>(entry.token),
                    static_cast<unsigned long>(entry.inverseToken),
                    static_cast<unsigned long>(entry.stage),
                    static_cast<unsigned>(EntryStageKnown(entry)));
    console.PrintLn("boot_entry_reset: valid=%u capture=app_entry_pre_sdk reset_state=%08lx reset_hint=%08lx brown_out=%08lx brown_out_scope=post_bootloader",
                    static_cast<unsigned>(EntryWitnessValid(entry)),
                    static_cast<unsigned long>(entry.resetState),
                    static_cast<unsigned long>(entry.resetHint),
                    static_cast<unsigned long>(entry.brownOut));
    if (EntryWitnessValid(entry)) {
        if (EntryResetHintKnown(entry)) {
            console.PrintLn("boot_entry_reset_decode: cpu0_raw=%lu cpu1_raw=%lu hint_valid=1 hint_reason=%lu",
                            static_cast<unsigned long>(entry.resetState & 0x3fU),
                            static_cast<unsigned long>((entry.resetState >> 6U) & 0x3fU),
                            static_cast<unsigned long>(entry.resetHint & 0x7fffU));
        } else {
            console.PrintLn("boot_entry_reset_decode: cpu0_raw=%lu cpu1_raw=%lu hint_valid=0 hint_reason=unknown",
                            static_cast<unsigned long>(entry.resetState & 0x3fU),
                            static_cast<unsigned long>((entry.resetState >> 6U) & 0x3fU));
        }
    }
#endif
#if defined(LITCH_NIMBLE_HOST_BUDGET_DIAGNOSTIC)
    (void)PrintTaskStackBudget(console, "host_budget", HOST_STACK);
#endif
    PrintStackProbe(console);
    if (!Valid(g_capsule)) {
        console.PrintLn("panic_diag: retained=unavailable missing_marker=inconclusive");
        return;
    }
    const auto& c = g_capsule;
    console.PrintLn("panic_diag: checksum=%08lx", static_cast<unsigned long>(c.checksum));
    const bool extended = (c.flags & Extended) != 0U;
    console.PrintLn("panic_diag: retained=%s age=unknown elf=external_pin_required core=%lu exception=%lu cause=%lu flags=%lu task=unknown tcb=%08lx",
                    extended ? "first_extended_generation" : RecognizedLegacy(c) ? "archived_legacy24" : "preserved_unknown_format",
                    static_cast<unsigned long>(c.core), static_cast<unsigned long>(c.exception),
                    static_cast<unsigned long>(c.cause), static_cast<unsigned long>(c.flags),
                    static_cast<unsigned long>(c.currentTcb));
    console.PrintLn("panic_diag: address=%08lx frame=%08lx pc=%08lx sp=%08lx a0=%08lx fault_address=%08lx",
                    static_cast<unsigned long>(c.address), static_cast<unsigned long>(c.frame),
                    static_cast<unsigned long>(c.pc), static_cast<unsigned long>(c.sp),
                    static_cast<unsigned long>(c.a0), static_cast<unsigned long>(c.faultAddress));
    for (unsigned i = 0U; i < c.traceCount && i < TRACE_CAPACITY; ++i) {
        console.PrintLn("panic_diag: trace=%u pc=%08lx sp=%08lx", i,
                        static_cast<unsigned long>(c.tracePc[i]), static_cast<unsigned long>(c.traceSp[i]));
    }
    if (!extended) {
        console.PrintLn("panic_diag: preservation=first_fault");
        for (unsigned i = 0U; i < 2U; ++i) PrintObservation(console, "fault", i, c.observations[i]);
        return;
    }
    const bool markers = c.observations[0].revision == 2U && c.observations[1].revision == 2U &&
        c.observations[0].request == 0U && c.observations[1].request == 0U &&
        c.observations[0].step == 0U && c.observations[1].step == 0U;
    console.PrintLn("panic_diag: extended_format=PAN2+64 markers=%s raw_metadata=1 legacy_fault_labels=transport_only",
                    markers ? "valid" : "unknown");
    // Always expose all words losslessly, even if future/partial markers differ.
    for (unsigned i = 0U; i < DETAIL_WORDS; ++i) {
        console.PrintLn("panic_diag: detail=%u raw=%08lx", i, static_cast<unsigned long>(DetailWord(c, i)));
    }
    if (!markers) return;
    const bool frameAvailable = (c.flags & (InvalidInfo | InvalidFrame)) == 0U;
    const bool tcbAvailable = (c.flags & RawTcbInvalid) == 0U;
    console.PrintLn("panic_diag: debug_snapshot=raw_current_cpu semantic_scope=%s debugcause=%08lx dbreaka1=%08lx dbreakc1=%08lx",
                    c.exception == 0U && (c.flags & Abort) == 0U ? "classified_debug" : "nondebug_do_not_classify",
                    static_cast<unsigned long>(DetailWord(c, 0U)), static_cast<unsigned long>(DetailWord(c, 1U)),
                    static_cast<unsigned long>(DetailWord(c, 2U)));
    console.PrintLn("panic_diag: raw_frame_cause=%s raw=%08lx raw_tcb_fields=%s",
                    frameAvailable ? "available" : "unavailable", static_cast<unsigned long>(DetailWord(c, 3U)),
                    tcbAvailable ? "available_unvalidated" : "unavailable");
    if (!tcbAvailable) return;
    console.PrintLn("panic_diag: raw_tcb_top=%08lx base=%08lx end=%08lx core=%08lx interpretation=unvalidated",
                    static_cast<unsigned long>(DetailWord(c, 4U)), static_cast<unsigned long>(DetailWord(c, 5U)),
                    static_cast<unsigned long>(DetailWord(c, 6U)), static_cast<unsigned long>(DetailWord(c, 7U)));
    char name[17]{};
    bool terminated = false;
    for (unsigned i = 0U; i < 16U; ++i) {
        const uint8_t byte = static_cast<uint8_t>(DetailWord(c, 8U + i / 4U) >> ((i % 4U) * 8U));
        if (byte == 0U) terminated = true;
        name[i] = terminated ? '\0' : byte >= 32U && byte < 127U ? static_cast<char>(byte) : '?';
    }
    console.PrintLn("panic_diag: raw_inline_name_ascii=%s nul_terminated=%u task_identity=unknown",
                    name, static_cast<unsigned>(terminated));
}
} // namespace runtime::panic_diagnostic
#endif
