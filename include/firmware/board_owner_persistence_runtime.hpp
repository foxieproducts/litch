#pragma once

#ifdef ARDUINO

#include "firmware/settings_persistence_worker.hpp"
#include "modules/board_owner_persistence.hpp"
#include "modules/board_identity_persistence.hpp"

namespace firmware {

// The cooperative caller owns the live settings document and all public calls.
// Only NativeClearPort runs on settings_io. Its context must live until release.
class BoardOwnerPersistenceRuntime final : public modules::BoardOwnerPersistence {
  public:
    struct NativeClearPort {
        void* context = nullptr;
        // Called outside the native host lock; each NVS leaf owns exclusion. Must
        // delete all bonds and freshly reconcile all four durable peer tables.
        bool (*clearAndReconcile)(void*) = nullptr;
    };
    struct NativeRollbackPort {
        void* context = nullptr;
        bool (*rollbackAndReconcile)(void*, const modules::BoardOwnerAddBaseline&) = nullptr;
    };

    BoardOwnerPersistenceRuntime(fxe::SettingsStore& settings,
                                 SettingsPersistenceWorker& worker,
                                 modules::BoardIdentityPersistence& identity)
        : m_settings(settings), m_worker(worker), m_identity(identity) {}

    // Boot-only RAM preparation, after load and before any runtime save. Invalid
    // records/capacity fail closed. No filesystem or native-store access.
    bool Begin();
    modules::BoardOwnerRecord DurableOwnerRecord() const {
        return m_prepared ? m_durableOwner : modules::BoardOwnerRecord::Invalid;
    }
    void Update();
    bool SaveOwnerRecord(modules::BoardOwnerRecord record, Receipt& receipt) override;
    bool SaveAddingJournal(const modules::BoardOwnerAddBaseline& baseline, Receipt& receipt) override;
    bool RequestIdentityRepair(uint64_t lease) override;
    modules::BoardOwnerAddBaseline AddingBaseline() const override { return m_durableBaseline; }
    Outcome GetOutcome(Receipt receipt) const override;
    bool Release(Receipt receipt) override;

    // Caller first proves durable Clearing and quiesces/terminates peer traffic.
    // This admits one tracked Clearing snapshot with the native prerequisite.
    // Release the terminal result BEFORE acknowledging deletion to the controller;
    // otherwise its final Absent save cannot acquire the retained worker slot.
    bool RequestNativeClear(NativeClearPort port);
    // Requires durable Adding, and invokes only the targeted rollback port.
    bool RequestNativeRollback(NativeRollbackPort port);
    Outcome GetNativeRollbackOutcome() const;
    bool ReleaseNativeRollback();
    Outcome GetNativeClearOutcome() const;
    bool ReleaseNativeClear();

  private:
    enum class Job : uint8_t { None, Owner, NativeClear, NativeRollback, IdentityRepair };
    static bool ClearNativeOnWorker(void* context);
    static bool RollbackNativeOnWorker(void* context);
    bool SetPreparedOwner(modules::BoardOwnerRecord record,
                          const modules::BoardOwnerAddBaseline& baseline = {});
    bool SaveIntent(modules::BoardOwnerRecord record,
                    const modules::BoardOwnerAddBaseline& baseline, Receipt& receipt);
    bool RequestNativeJob(modules::BoardOwnerRecord expected, bool (*prerequisite)(void*));
    bool ReleaseCurrent();

    fxe::SettingsStore& m_settings;
    SettingsPersistenceWorker& m_worker;
    modules::BoardIdentityPersistence& m_identity;
    uint64_t m_identityRepairLease = 0U;
    NativeClearPort m_nativeClearPort{};
    NativeRollbackPort m_nativeRollbackPort{};
    Receipt m_receipt = 0U;
    Outcome m_outcome = Outcome::Unknown;
    Job m_job = Job::None;
    modules::BoardOwnerRecord m_durableOwner = modules::BoardOwnerRecord::Invalid;
    modules::BoardOwnerRecord m_previousOwner = modules::BoardOwnerRecord::Invalid;
    modules::BoardOwnerRecord m_requestedOwner = modules::BoardOwnerRecord::Invalid;
    modules::BoardOwnerAddBaseline m_durableBaseline{}, m_previousBaseline{}, m_requestedBaseline{};
    bool m_prepared = false;
};

} // namespace firmware

#endif
