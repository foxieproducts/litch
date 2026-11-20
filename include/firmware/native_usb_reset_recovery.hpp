#pragma once

namespace firmware_internal {

// Existing DCD and event-queue boundaries; no second USB stack.
void CaptureNativeUsbDcdInterrupt(void* handle);
bool NativeUsbDcdInterruptBegin(unsigned raw, unsigned mask);
void NativeUsbDcdInterruptEnd(bool configuredLoss);
bool NativeUsbRecoveryBlocksConnect();
bool BeforeNativeUsbEvent(const void* event, bool inIsr);
void AfterNativeUsbEvent(const void* event);
void BeginNativeUsbResetRecovery();
void PollNativeUsbResetRecovery();

} // namespace firmware_internal
