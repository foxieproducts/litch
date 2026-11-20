#pragma once
#include <cstddef>
#include "runtime/panic_diagnostic.hpp"

namespace runtime::panic_diagnostic {
#define LITCH_PANIC_INLINE inline __attribute__((always_inline))
// Full pinned S3 internal DRAM, including the upper 64 KiB omitted by the
// older failed probe. No PSRAM, flash, RTC, or peripheral pointers are read.
LITCH_PANIC_INLINE bool Readable(const uintptr_t address, const size_t size) {
    return (address & 3U) == 0U && address >= 0x3fc88000U &&
           address < 0x3fd00000U && size <= 0x3fd00000U - address;
}
LITCH_PANIC_INLINE bool Executable(const uint32_t pc) {
    return (pc >= 0x40000000U && pc < 0x40070000U) ||
           (pc >= 0x40370000U && pc < 0x403e0000U) ||
           (pc >= 0x42000000U && pc < 0x44000000U);
}
LITCH_PANIC_INLINE uint32_t Checksum(const volatile Capsule& capsule) {
    const auto* words = reinterpret_cast<const volatile uint32_t*>(&capsule);
    uint32_t hash = 2166136261U;
    for (unsigned i = 2U; i < sizeof(Capsule) / 4U; ++i) {
        hash = (hash ^ words[i]) * 16777619U;
    }
    return hash;
}
LITCH_PANIC_INLINE bool Valid(const volatile Capsule& capsule) {
    return capsule.magic == MAGIC && capsule.checksum == Checksum(capsule);
}
// Prefix of the pinned XtExcFrame through EXCVADDR. Checked against SDK offsets
// at the platform boundary. Read only after validating the entire prefix.
struct ExceptionFrame {
    uint32_t exit, pc, ps, a0, a1, a[14], sar, cause, faultAddress;
};
static_assert(sizeof(ExceptionFrame) == 88U, "pinned Xtensa frame prefix");
static_assert(sizeof(Capsule) == 196U && offsetof(Capsule, observations) == 124U,
              "PAN2 extent and field offsets must remain stable");
static_assert(sizeof(Observation) == 36U && offsetof(Observation, task) == 4U &&
              offsetof(Observation, request) == 8U && offsetof(Observation, step) == 12U &&
              offsetof(Observation, atMs) == 16U && offsetof(Observation, watermarkBytes) == 20U &&
              offsetof(Observation, result) == 24U && offsetof(Observation, startedAtMs) == 28U &&
              offsetof(Observation, endedAtMs) == 32U, "legacy numeric transport offsets");

LITCH_PANIC_INLINE uint32_t DetailWord(const volatile Capsule& capsule, unsigned index) {
    const auto& slot = capsule.observations[index / 6U];
    switch (index % 6U) {
        case 0U: return slot.task;
        case 1U: return slot.atMs;
        case 2U: return slot.watermarkBytes;
        case 3U: return slot.result;
        case 4U: return slot.startedAtMs;
        default: return slot.endedAtMs;
    }
}
LITCH_PANIC_INLINE bool RecognizedLegacy(const volatile Capsule& capsule) {
    if (!Valid(capsule) || capsule.flags != (PseudoCause | TruncatedTrace) ||
        capsule.exception != 0U || capsule.cause != 0xffffffffU ||
        capsule.knownTask != 0xffffffffU || capsule.core >= 2U) return false;
    const auto* words = reinterpret_cast<const volatile uint32_t*>(capsule.observations);
    for (unsigned i = 0U; i < sizeof(capsule.observations) / 4U; ++i) {
        if (words[i] != 0U) return false;
    }
    return true;
}
LITCH_PANIC_INLINE bool ShouldCapture(const volatile Capsule& capsule) {
    // A valid first fault must survive even when its format predates this image.
    return !Valid(capsule);
}

// Inlined into the IRAM wrapper: bounded volatile reads/writes only. No SDK,
// heap, flash, logger or task API. The caller claims the first-fault guard.
template <typename Info>
LITCH_PANIC_INLINE void Capture(volatile Capsule& out, const Info* const info,
                               void* volatile const* const currentTasks,
                               const FaultRegisters registers,
                               const bool sdkAbort = false) {
    auto* words = reinterpret_cast<volatile uint32_t*>(&out);
    for (unsigned i = 0U; i < sizeof(Capsule) / 4U; ++i) words[i] = 0U;
    out.knownTask = 0xffffffffU;
    out.core = out.exception = out.cause = 0xffffffffU;
    out.flags = Extended | RawTcbInvalid;
    out.observations[0].revision = out.observations[1].revision = 2U;
    out.observations[0].task = registers.debugCause;
    out.observations[0].atMs = registers.breakAddress;
    out.observations[0].watermarkBytes = registers.breakControl;
    if (!Readable(reinterpret_cast<uintptr_t>(info), sizeof(Info))) {
        out.flags |= InvalidInfo;
    } else {
        out.core = static_cast<uint32_t>(info->core);
        out.exception = static_cast<uint32_t>(info->exception);
        out.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->addr));
        out.frame = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->frame));
        if (out.core < 2U) {
            out.currentTcb = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(currentTasks[out.core]));
            // Pinned SDK DWARF: TCB size344; read only the needed80-byte
            // prefix. These remain raw fields even when internally inconsistent.
            if (Readable(out.currentTcb, 80U)) {
                const auto* tcb = reinterpret_cast<const volatile uint32_t*>(out.currentTcb);
                out.observations[0].startedAtMs = tcb[0];  // pxTopOfStack +0
                out.observations[0].endedAtMs = tcb[13];   // pxStack +52
                out.observations[1].task = tcb[19];        // pxEndOfStack +76
                out.observations[1].atMs = tcb[18];        // xCoreID +72
                out.observations[1].watermarkBytes = tcb[14]; // inline name +56
                out.observations[1].result = tcb[15];
                out.observations[1].startedAtMs = tcb[16];
                out.observations[1].endedAtMs = tcb[17];
                out.flags &= ~RawTcbInvalid;
            }
        }
        if (info->pseudo_excause) out.flags |= PseudoCause;
        // The pinned abort path deliberately faults after setting its flag.
        // Retain a validated frame/trace, but do not call its synthetic cause
        // the underlying reason for the abort.
        if (sdkAbort || out.exception == 3U) {
            // The real pinned handler applies this normalization after entry.
            out.exception = 3U;
            out.flags |= Abort;
        }
        if (!Readable(out.frame, sizeof(ExceptionFrame))) {
            out.flags |= InvalidFrame;
        } else {
            const auto* frame = reinterpret_cast<const volatile ExceptionFrame*>(info->frame);
            out.pc = frame->pc; out.sp = frame->a1; out.a0 = frame->a0;
            out.faultAddress = frame->faultAddress;
            out.observations[0].result = frame->cause; // raw even for pseudo/abort
            if ((out.flags & (PseudoCause | Abort)) == 0U) out.cause = frame->cause;
            out.tracePc[0] = out.pc; out.traceSp[0] = out.sp; out.traceCount = 1U;
            uint32_t sp = out.sp;
            uint32_t link = out.a0;
            while (link != 0U && out.traceCount < TRACE_CAPACITY) {
                const uint32_t pc = (link & 0x3fffffffU) | 0x40000000U;
                if ((sp & 15U) != 0U || sp < 16U || !Readable(sp - 16U, 16U) ||
                    !Executable(pc)) { out.flags |= TruncatedTrace; break; }
                const auto* saved = reinterpret_cast<const volatile uint32_t*>(sp - 16U);
                const uint32_t nextSp = saved[1];
                if (nextSp <= sp || (nextSp & 15U) != 0U || !Readable(nextSp, 16U)) {
                    out.flags |= TruncatedTrace; break;
                }
                const unsigned index = out.traceCount;
                out.tracePc[index] = pc; out.traceSp[index] = nextSp;
                out.traceCount = index + 1U;
                link = saved[0]; sp = nextSp;
            }
            if (link != 0U && out.traceCount == TRACE_CAPACITY) out.flags |= TruncatedTrace;
        }
    }
    out.checksum = Checksum(out);
    // Publish last: an interrupted capture never presents as valid evidence.
#if defined(__XTENSA__)
    asm volatile("memw" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
    out.magic = MAGIC;
}
#if defined(LITCH_PANIC_DIAGNOSTIC)
LITCH_PANIC_INLINE uint32_t ProbeChecksum(const volatile StackProbe& probe, const uint32_t state) {
    const auto* words = reinterpret_cast<const volatile uint32_t*>(&probe);
    // ARMED and CAPTURED are different evidence claims. Bind the intended
    // final marker without publishing it before the payload is complete.
    uint32_t hash = (2166136261U ^ state) * 16777619U;
    for (unsigned i = 2U; i < sizeof(StackProbe) / 4U; ++i) {
        hash = (hash ^ words[i]) * 16777619U;
    }
    return hash;
}
LITCH_PANIC_INLINE bool ProbeValid(const volatile StackProbe& probe) {
    return (probe.magic == PROBE_CAPTURED || (probe.magic == PROBE_ARMED && probe.token != 0U)) &&
           probe.checksum == ProbeChecksum(probe, probe.magic);
}
LITCH_PANIC_INLINE bool ProbeBound(const volatile StackProbe& probe,
                                  const volatile Capsule& capsule) {
    return ProbeValid(probe) && probe.magic == PROBE_CAPTURED && Valid(capsule) &&
           probe.panChecksum == capsule.checksum && probe.panPc == capsule.pc;
}
LITCH_PANIC_INLINE void PublishProbe(volatile StackProbe& probe, const uint32_t magic) {
    probe.checksum = ProbeChecksum(probe, magic);
#if defined(__XTENSA__)
    asm volatile("memw" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
    probe.magic = magic;
}
// Called under the same first-fault claim as Capture. Retain raw words across
// call0 boundaries without pretending the windowed unwinder can interpret them.
// A reset can destroy the arm record before a new boot fault. Still capture
// that new PAN2's raw context; token0 explicitly carries no trial attribution.
LITCH_PANIC_INLINE void CaptureStackProbe(volatile StackProbe& out,
                                        const volatile Capsule& capsule) {
    const uint32_t token = ProbeValid(out) && out.magic == PROBE_ARMED ? out.token : 0U;
    auto* words = reinterpret_cast<volatile uint32_t*>(&out);
    for (unsigned i = 0U; i < sizeof(StackProbe) / 4U; ++i) words[i] = 0U;
    out.token = token;
    out.panChecksum = capsule.checksum;
    out.panPc = capsule.pc;
    out.flags = ProbeInvalidFrame | ProbeInvalidStack;
    if ((capsule.flags & (InvalidInfo | InvalidFrame)) == 0U &&
        Readable(capsule.frame, sizeof(ExceptionFrame))) {
        const auto* frame = reinterpret_cast<const volatile ExceptionFrame*>(capsule.frame);
        out.ps = frame->ps;
        for (unsigned i = 0U; i < 14U; ++i) out.a[i] = frame->a[i];
        out.sar = frame->sar;
        out.flags &= ~ProbeInvalidFrame;
        const uint32_t base = DetailWord(capsule, 5U);
        const uint32_t end = DetailWord(capsule, 6U);
        const uint32_t sp = capsule.sp;
        // pxEndOfStack is an inclusive BYTE address in this SDK. Read complete
        // aligned words only, within both internal DRAM and raw TCB bounds.
        if ((capsule.flags & RawTcbInvalid) == 0U && Readable(base, 4U) &&
            end >= base && end < 0x3fd00000U && (sp & 3U) == 0U &&
            sp >= base && sp <= end && end - sp >= 3U) {
            const uint32_t start = sp - base >= 16U ? sp - 16U : base;
            const uint32_t available = (end - start + 1U) / 4U;
            const uint32_t count = available < PROBE_STACK_WORDS ? available : PROBE_STACK_WORDS;
            if (count != 0U && Readable(start, count * 4U)) {
                out.stackAddress = start;
                out.stackWords = count;
                out.flags &= ~ProbeInvalidStack;
                if (start != sp - 16U || count != PROBE_STACK_WORDS) out.flags |= ProbeStackClipped;
                const auto* stack = reinterpret_cast<const volatile uint32_t*>(start);
                for (unsigned i = 0U; i < count; ++i) out.stack[i] = stack[i];
            }
        }
    }
    PublishProbe(out, PROBE_CAPTURED);
}
#endif
#undef LITCH_PANIC_INLINE
} // namespace runtime::panic_diagnostic
