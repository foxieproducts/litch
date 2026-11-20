#pragma once

#ifdef ARDUINO

#include <cstdint>

#include "firmware/firmware_internal.hpp"
#include "firmware/home_state_bridge.hpp"

namespace firmware {

extern volatile uint32_t g_expanderIntCount[4];

} // namespace firmware

namespace firmware_internal {

// Seed before ADC/RF initialization; metadata wiring occurs after settings load.
void InitializeBoardIdentityEntropy();
void ConfigureBoardIdentityPersistence(FirmwareSystem& sys);
void ProjectControlFeedbackSetting(void* context, const char* canonicalKey);
void ConfigureBoardControlMutations(FirmwareSystem& sys,
    const modules::ControlMutationCallbacks& callbacks);


void SetupApp();
void AdvanceStartup();
void FinishStartup();
void LoopApp();

void LogStateTransitionInternal(FirmwareSystem& sys,
                                const char* before,
                                const char* after);
void LogStateTransitionInternalWithSquares(FirmwareSystem& sys,
                                           const char* before,
                                           const char* after,
                                           uint8_t fromSq,
                                           uint8_t toSq,
                                           bool capValid,
                                           uint8_t capSq);
void LogExpanderInterruptsIfAny();
bool IsHardwareTestSavePending();
bool IsFactoryClearPending();
bool RequestFactoryClear(FirmwareSystem& sys);
void ReleaseHardwareTestInput(FirmwareSystem& sys);
void UpdateHardwareTestPersistence(FirmwareSystem& sys);
void NoteHardwareTestVerifierIntent(bool enabled);
void UpdateHardwareTestRadio(FirmwareSystem& sys, uint32_t nowMs);
void ArmFactoryBootIntent(firmware::FactoryBootIntent intent);
void ApplyRequestedFactoryBoot(FirmwareSystem& sys);
void InitializeEarlyUpdater(FirmwareSystem& sys);
void BeginRecoveryUpdater(FirmwareSystem& sys);
void PumpRecoveryUpdater(FirmwareSystem& sys);
bool RequestFactoryBoot(FirmwareSystem& sys, firmware::FactoryBootIntent intent);
bool PrepareFactoryPersistence(FirmwareSystem& sys);
bool FactoryOtaBeginBusy(void* context);
void PollFactoryCompletionReboot(FirmwareSystem& sys);
bool PollRuntimeBootHealth(FirmwareSystem& sys);
void LoopFactoryRuntime(FirmwareSystem& sys);


} // namespace firmware_internal

#endif
