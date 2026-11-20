#pragma once
#include <cstddef>
#include <cstdint>

namespace fxe { class Console; }
namespace runtime::panic_diagnostic {
// Diagnostic-only private ABI observation. Inline TCB name bytes are raw metadata.
// Extended details use the old numeric slots as a lossless transport. They
// are raw fault metadata, never healthy task measurements or a task identity.
constexpr uint32_t MAGIC = 0x50414E32U; // PAN2
constexpr unsigned TRACE_CAPACITY = 8U;
constexpr unsigned DETAIL_WORDS = 12U;
struct FaultRegisters { uint32_t debugCause, breakAddress, breakControl; };
enum Flag : uint32_t {
    InvalidInfo = 1U, InvalidFrame = 2U, Abort = 4U, PseudoCause = 8U,
    TruncatedTrace = 16U, IncompleteObservation = 32U,
    Extended = 64U, RawTcbInvalid = 128U,
};
struct Observation {
    uint32_t revision, task, request, step, atMs, watermarkBytes, result;
    uint32_t startedAtMs, endedAtMs;
};
struct Capsule {
    uint32_t magic, checksum, flags, core, exception, cause, address, frame;
    uint32_t pc, sp, a0, faultAddress, currentTcb, knownTask, traceCount;
    uint32_t tracePc[TRACE_CAPACITY], traceSp[TRACE_CAPACITY];
    Observation observations[2];
};
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
// Separate from PAN2 so entry evidence never destroys a retained first fault.
constexpr uint32_t ENTRY_MAGIC = 0x454e5432U; // ENT2; ENT1 has no reset evidence.
struct EntryWitness {
    uint32_t magic, token, inverseToken, stage, inverseStage;
    uint32_t resetState, inverseResetState, resetHint, inverseResetHint;
    uint32_t brownOut, inverseBrownOut;
};
static_assert(sizeof(EntryWitness) == 44U, "private assembly layout");
static_assert(offsetof(EntryWitness, magic) == 0U &&
              offsetof(EntryWitness, token) == 4U &&
              offsetof(EntryWitness, inverseToken) == 8U &&
              offsetof(EntryWitness, stage) == 12U &&
              offsetof(EntryWitness, inverseStage) == 16U &&
              offsetof(EntryWitness, resetState) == 20U &&
              offsetof(EntryWitness, inverseResetState) == 24U &&
              offsetof(EntryWitness, resetHint) == 28U &&
              offsetof(EntryWitness, inverseResetHint) == 32U &&
              offsetof(EntryWitness, brownOut) == 36U &&
              offsetof(EntryWitness, inverseBrownOut) == 40U,
              "private assembly field offsets");
extern "C" volatile EntryWitness litch_boot_entry_witness;
inline bool EntryWitnessValid(const volatile EntryWitness& witness) {
    return witness.magic == ENTRY_MAGIC &&
           witness.inverseToken == ~witness.token &&
           witness.inverseResetState == ~witness.resetState &&
           witness.inverseResetHint == ~witness.resetHint &&
           witness.inverseBrownOut == ~witness.brownOut;
}
inline bool EntryResetHintKnown(const volatile EntryWitness& witness) {
    // Pinned IDF copies bits14:0 into bits30:16 and marks it with bit31. This is
    // raw STORE6, also shared with the SDK wake-stub owner; never infer a hint
    // from an unmarked or inconsistent word.
    return EntryWitnessValid(witness) && (witness.resetHint & 0x80000000U) != 0U &&
           (witness.resetHint & 0x7fffU) == ((witness.resetHint >> 16U) & 0x7fffU);
}
inline bool EntryStageKnown(const volatile EntryWitness& witness) {
    return EntryWitnessValid(witness) && witness.inverseStage == ~witness.stage &&
           (witness.stage == 1U || witness.stage == 2U);
}
// Runs only after the SDK has initialized C++ and entered the existing Setup.
inline void NoteSetupEntry() {
    if (EntryWitnessValid(litch_boot_entry_witness)) {
        litch_boot_entry_witness.stage = 2U;
        litch_boot_entry_witness.inverseStage = ~2U;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
}
#endif
#if defined(LITCH_PANIC_DIAGNOSTIC)
// Private first-fault extension; PAN2 remains byte-for-byte compatible. Tokens
// are externally recorded, increasing trial numbers, not timestamps. CAPTURED
// token0 means unarmed/no trial attribution; ARMED always requires nonzero.
constexpr uint32_t PROBE_ARMED = 0x53504131U; // SPA1
constexpr uint32_t PROBE_CAPTURED = 0x53504331U; // SPC1
constexpr unsigned PROBE_STACK_WORDS = 40U;
enum ProbeFlag : uint32_t {
    ProbeInvalidFrame = 1U, ProbeInvalidStack = 2U, ProbeStackClipped = 4U,
};
struct StackProbe {
    uint32_t magic, checksum, token, panChecksum, panPc;
    uint32_t stackAddress, stackWords, flags;
    uint32_t ps, a[14], sar;
    uint32_t stack[PROBE_STACK_WORDS];
};
static_assert(sizeof(StackProbe) == 256U && offsetof(StackProbe, ps) == 32U &&
              offsetof(StackProbe, stack) == 96U, "private stack probe layout");
extern volatile Capsule g_capsule;
extern volatile StackProbe g_stackProbe;
void PrintRetained(fxe::Console& console);
void RegisterStackProbeCommand(fxe::Console& console);
#endif
} // namespace runtime::panic_diagnostic
