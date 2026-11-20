#pragma once

#include <cstdint>

namespace fxe {

struct CrashReportSnapshot {
    uint32_t resetReason = 0;
    uint32_t heapFreeBytes = 0;
    uint32_t heapMinBytes = 0;
    uint32_t heapLargestFreeBytes = 0;
    uint32_t stackHighWaterWords = 0;

    uint32_t breadcrumbSeq = 0;
    char breadcrumb[64] = {};
};

typedef void (*CrashReportOutputFn)(const char* line, void* ctx);

void SetCrashReportOutput(CrashReportOutputFn fn, void* ctx);
void CaptureCrashReport(CrashReportSnapshot& outSnapshot);
void PrintCrashReport(CrashReportOutputFn fn = nullptr, void* ctx = nullptr);

void SetCrashBreadcrumb(const char* text);
const char* GetCrashBreadcrumb();
uint32_t GetCrashBreadcrumbSeq();

const char* ResetReasonToString(const uint32_t reason);

} // namespace fxe
