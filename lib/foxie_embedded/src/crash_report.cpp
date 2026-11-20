#include "foxie_embedded/crash_report.hpp"

#include <cstring>

#ifdef ARDUINO
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#endif

#include "foxie_embedded/fixed_string.hpp"

namespace fxe {

namespace {

CrashReportOutputFn g_output = nullptr;
void* g_output_ctx = nullptr;

#ifdef ARDUINO
RTC_DATA_ATTR char g_breadcrumb[64] = {};
RTC_DATA_ATTR uint32_t g_breadcrumb_seq = 0;
#else
char g_breadcrumb[64] = {};
uint32_t g_breadcrumb_seq = 0;
#endif

void EmitLine(const char* line, CrashReportOutputFn fn, void* ctx) {
    if (!line || !fn) {
        return;
    }
    fn(line, ctx);
}

} // namespace

void SetCrashReportOutput(CrashReportOutputFn fn, void* ctx) {
    g_output = fn;
    g_output_ctx = ctx;
}

void SetCrashBreadcrumb(const char* text) {
    if (!text) {
        return;
    }

    std::strncpy(g_breadcrumb, text, sizeof(g_breadcrumb) - 1U);
    g_breadcrumb[sizeof(g_breadcrumb) - 1U] = '\0';
    ++g_breadcrumb_seq;
}

const char* GetCrashBreadcrumb() {
    return g_breadcrumb;
}

uint32_t GetCrashBreadcrumbSeq() {
    return g_breadcrumb_seq;
}

void CaptureCrashReport(CrashReportSnapshot& outSnapshot) {
#ifdef ARDUINO
    outSnapshot.resetReason = static_cast<uint32_t>(esp_reset_reason());
    outSnapshot.heapFreeBytes = esp_get_free_heap_size();
    outSnapshot.heapMinBytes = esp_get_minimum_free_heap_size();
    outSnapshot.heapLargestFreeBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    outSnapshot.stackHighWaterWords = uxTaskGetStackHighWaterMark(nullptr);
#else
    outSnapshot.resetReason = 0;
    outSnapshot.heapFreeBytes = 0;
    outSnapshot.heapMinBytes = 0;
    outSnapshot.heapLargestFreeBytes = 0;
    outSnapshot.stackHighWaterWords = 0;
#endif

    outSnapshot.breadcrumbSeq = g_breadcrumb_seq;
    std::strncpy(outSnapshot.breadcrumb, g_breadcrumb, sizeof(outSnapshot.breadcrumb) - 1U);
    outSnapshot.breadcrumb[sizeof(outSnapshot.breadcrumb) - 1U] = '\0';
}

const char* ResetReasonToString(const uint32_t reason) {
#ifdef ARDUINO
    switch (static_cast<esp_reset_reason_t>(reason)) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
#else
    (void)reason;
    return "unknown";
#endif
}

void PrintCrashReport(CrashReportOutputFn fn, void* ctx) {
    CrashReportSnapshot snapshot;
    CaptureCrashReport(snapshot);

    if (!fn) {
        fn = g_output;
        ctx = g_output_ctx;
    }

    if (!fn) {
        return;
    }

    fxe::FixedString<128> line;
    (void)line.Format("crash: reset_reason=%s (%lu)", ResetReasonToString(snapshot.resetReason),
                      static_cast<unsigned long>(snapshot.resetReason));
    EmitLine(line.CStr(), fn, ctx);

    (void)line.Format("crash: heap_free=%lu min_free=%lu largest_free=%lu",
                      static_cast<unsigned long>(snapshot.heapFreeBytes),
                      static_cast<unsigned long>(snapshot.heapMinBytes),
                      static_cast<unsigned long>(snapshot.heapLargestFreeBytes));
    EmitLine(line.CStr(), fn, ctx);

    (void)line.Format("crash: stack_high_water_words=%lu",
                      static_cast<unsigned long>(snapshot.stackHighWaterWords));
    EmitLine(line.CStr(), fn, ctx);

    (void)line.Format("crash: breadcrumb_seq=%lu breadcrumb=%s",
                      static_cast<unsigned long>(snapshot.breadcrumbSeq), snapshot.breadcrumb);
    EmitLine(line.CStr(), fn, ctx);
}

} // namespace fxe
