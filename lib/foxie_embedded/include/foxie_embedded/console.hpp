#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ArduinoJson.h>

#include "foxie_embedded/console_transport.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_function.hpp"

namespace fxe {
class SettingsStore;
struct SettingDescriptor;

class Console final {
  public:
    enum class SettingsSetResult : uint8_t {
        Ok = 0,
        UnknownKey,
        TypeMismatch,
        OutOfRange,
        InvalidEnumValue,
        Applied,
        Noop,
        NotAtStart,
        Busy,
        StorageUnavailable,
        AccessDenied,
    };

    static constexpr const char* SettingsSetResultToString(const SettingsSetResult result) {
        switch (result) {
            case SettingsSetResult::Ok:
                return "ok";
            case SettingsSetResult::UnknownKey:
                return "unknown_key";
            case SettingsSetResult::TypeMismatch:
                return "type_mismatch";
            case SettingsSetResult::OutOfRange:
                return "out_of_range";
            case SettingsSetResult::InvalidEnumValue:
                return "invalid_enum_value";
            case SettingsSetResult::Applied:
                return "applied";
            case SettingsSetResult::Noop:
                return "noop";
            case SettingsSetResult::NotAtStart:
                return "not_at_start";
            case SettingsSetResult::Busy:
                return "busy";
            case SettingsSetResult::StorageUnavailable:
                return "storage_unavailable";
            case SettingsSetResult::AccessDenied:
                return "access_denied";
        }

        return "unknown_error";
    }

    struct Config {
        bool installBuiltins = true;
    };

    static constexpr size_t MAX_ARGS = 8;
    // Headroom above 41 product/private registrations; +896 bytes on S3 vs 40.
    static constexpr size_t MAX_COMMANDS = 48;
    static constexpr size_t INPUT_CAPACITY = 256;
    static constexpr size_t HISTORY_CAPACITY = 5;

    struct Args {
        struct Arg {
            const char* ptr = nullptr;
            uint8_t len = 0;

            [[nodiscard]] std::string_view View() const {
                return ptr ? std::string_view(ptr, len) : std::string_view();
            }

            [[nodiscard]] bool Equals(const std::string_view s) const {
                return View() == s;
            }
        };

        uint8_t argc = 0;
        Arg argv[MAX_ARGS] = {};
        bool overflow = false;

        [[nodiscard]] bool Has(const size_t index) const {
            return (index < argc) && (argv[index].ptr != nullptr);
        }

        [[nodiscard]] std::string_view Get(const size_t index) const {
            return Has(index) ? argv[index].View() : std::string_view();
        }

        [[nodiscard]] const char* CStr(const size_t index) const {
            return Has(index) ? argv[index].ptr : nullptr;
        }

        [[nodiscard]] bool Equals(const size_t index, const std::string_view s) const {
            return Has(index) && argv[index].Equals(s);
        }
    };

    using CommandHandler = fxe::FixedFunction<bool(const Args& args), 32>;
    // Optional fallback after registered commands; absent from help/completion.
    using UnknownCommandHandler = bool (*)(Console&, const Args&, void* context);

    // Valid during submitted-line/paste-line callbacks and command dispatch;
    // edits/Enter, or paste begin plus payload, must retain one known source.
    // Programmatic commands remain Unknown.
    ConsoleInputSource CommandInputSource() const { return m_commandInputSource; }
    uint64_t CommandRemoteInputGeneration() const { return m_commandRemoteInputGeneration; }

    // \brief Notification hook for executed command lines.
    // \note Called with the raw command line before tokenization mutates it.
    using CommandLineCallback = fxe::FixedFunction<void(const char* line), 64>;

    // \brief Optional callback invoked for every submitted line, including blank Enter.
    // \returns true when the callback consumed the line and normal command execution should stop.
    using EnteredLineCallback = fxe::FixedFunction<bool(const char* line), 64>;

    // \brief Provide tab completion candidates for a specific argument position.
    // Note: argIndex is the token index in the input (0=command name, 1=subcommand, ...).
    // prefix is the current token prefix to match.
    // Returns number of matches written to outMatches.
    using CompletionHandler = fxe::FixedFunction<size_t(const Args& args,
                                                        size_t argIndex,
                                                        const char* prefix,
                                                        const char** outMatches,
                                                        size_t outMatchesCap),
                                                 32>;

    // \brief Enumerate extra well-known setting keys for tab completion.
    // \note This is intentionally project-defined; the console library does not
    //       bake in any setting schema.
    using SettingsKeyEnumCallback = fxe::FixedFunction<size_t(const char* prefix,
                                                              const char** outMatches,
                                                              size_t outMatchesCap),
                                                       64>;

    // \brief Decide whether writing a given key should use the must-save tier.
    // \returns true if the callback handled the key; false means use defaults.
    // \note Intended for rare keys requiring the short persistence deadline.
    using SettingsWritePolicyCallback = fxe::FixedFunction<bool(std::string_view key,
                                                                bool* outForceSaveNow),
                                                           64>;

    // \brief Invoked immediately before a settings mutation.
    // \note Persistence guards belong to SettingsStore so deferred retries and
    //       non-console callers receive the same protection. An empty key
    //       denotes a whole-store or bulk persistence operation.
    // Optional diagnostic key filter: hides reads/listing and blocks raw paste deletion.
    // Typed writes retain their existing validation; whole-store reset is unaffected.
    using SettingsDiagnosticAccessCallback = fxe::FixedFunction<bool(std::string_view key), 64>;

    using SettingsMutationCallback = fxe::FixedFunction<void(std::string_view key), 64>;

    // \brief Optional owner of the whole `settings clear|erase` request.
    // \note Invoked before any deletion, mutation callback or reload. True means
    //       accepted, possibly asynchronous; false leaves storage untouched.
    //       Without this callback the built-in synchronous erase is retained.
    using SettingsClearCallback = fxe::FixedFunction<bool(), 32>;
    using SettingsLoadCallback = fxe::FixedFunction<bool(), 32>;

    // Whole-store operations can replace private authority records as well as
    // public preferences. Reject before any filesystem or live-document change.
    enum class SettingsStorageOperation : uint8_t { Load, Erase, Corrupt, Format };
    using SettingsStorageAdmissionCallback =
        fxe::FixedFunction<bool(SettingsStorageOperation), 32>;

    enum class StorageReformatAction : uint8_t { Status, Confirm };
    enum class StorageReformatStatus : uint8_t {
        Unavailable, Idle, Busy, Pending, Running, Succeeded, Failed,
    };
    using StorageReformatCallback =
        fxe::FixedFunction<StorageReformatStatus(StorageReformatAction), 32>;

    using SettingsValidateAndSetCallback =
        fxe::FixedFunction<SettingsSetResult(SettingsStore& settings,
                                             std::string_view key,
                                             JsonVariantConst value),
                           64>;

    // \brief Reset a registered setting through the owning mutation service.
    // \note Empty-valued paste lines use this callback instead of editing the
    //       settings document directly.
    using SettingsResetCallback =
        fxe::FixedFunction<SettingsSetResult(SettingsStore& settings,
                                             std::string_view key),
                           64>;

    using SettingsDescriptorLookupCallback =
        fxe::FixedFunction<const SettingDescriptor*(std::string_view key), 64>;

    // \brief Events delivered while streaming pasted input.
    enum class PasteEvent : uint8_t {
        Line = 0,
        LineOverflow,
        Finish,
    };

    // \brief Streaming paste callback.
    // \note Line data is mutable, null-terminated, and valid only during the
    //       callback. LineOverflow has no data. Finish is delivered once after
    //       Ctrl+D, including after line errors.
    // \returns false to mark the paste failed; later events are still delivered.
    using PasteHandler = fxe::FixedFunction<bool(PasteEvent event, char* line, size_t len), 64>;

    explicit Console(IConsoleTransport& transport);
    Console(IConsoleTransport& transport, const Config& cfg);

    void SetSettingsStore(fxe::SettingsStore* settings);

    void SetSettingsKeyEnumCallback(SettingsKeyEnumCallback cb) { m_settingsKeyEnumCb = std::move(cb); }

    void SetSettingsWritePolicyCallback(SettingsWritePolicyCallback cb) {
        m_settingsWritePolicyCb = std::move(cb);
    }

    void SetSettingsDiagnosticAccessCallback(SettingsDiagnosticAccessCallback cb) {
        m_settingsDiagnosticAccessCb = std::move(cb);
    }
    [[nodiscard]] bool CanAccessDiagnosticSetting(std::string_view key) const {
        return !m_settingsDiagnosticAccessCb || m_settingsDiagnosticAccessCb(key);
    }

    void SetSettingsMutationCallback(SettingsMutationCallback cb) {
        m_settingsMutationCb = std::move(cb);
    }

    void SetSettingsClearCallback(SettingsClearCallback cb) { m_settingsClearCb = std::move(cb); }
    void SetSettingsLoadCallback(SettingsLoadCallback cb) { m_settingsLoadCb = std::move(cb); }
    void SetSettingsStorageAdmissionCallback(SettingsStorageAdmissionCallback cb) {
        m_settingsStorageAdmissionCb = std::move(cb);
    }
    void SetStorageReformatCallback(StorageReformatCallback cb) {
        m_storageReformatCb = std::move(cb);
    }

    void SetSettingsValidateAndSetCallback(SettingsValidateAndSetCallback cb) {
        m_settingsValidateAndSetCb = std::move(cb);
    }

    void SetSettingsResetCallback(SettingsResetCallback cb) {
        m_settingsResetCb = std::move(cb);
    }

    void SetSettingsDescriptorLookupCallback(SettingsDescriptorLookupCallback cb) {
        m_settingsDescriptorLookupCb = std::move(cb);
    }

    [[nodiscard]] size_t EnumerateExtraSettingKeys(const char* prefix,
                                                   const char** outMatches,
                                                   const size_t outMatchesCap) const {
        if (!m_settingsKeyEnumCb || !outMatches || outMatchesCap == 0) {
            return 0;
        }
        return m_settingsKeyEnumCb(prefix, outMatches, outMatchesCap);
    }

    [[nodiscard]] bool ShouldForceSaveNowForSettingKey(const std::string_view key) const {
        if (!m_settingsWritePolicyCb) {
            return false;
        }

        bool forceSaveNow = false;
        const bool handled = m_settingsWritePolicyCb(key, &forceSaveNow);
        return handled ? forceSaveNow : false;
    }

    void NotifyBeforeSettingsMutation(const std::string_view key) const {
        if (m_settingsMutationCb) {
            m_settingsMutationCb(key);
        }
    }

    [[nodiscard]] bool HasSettingsValidateAndSetCallback() const {
        return static_cast<bool>(m_settingsValidateAndSetCb);
    }

    [[nodiscard]] bool HasSettingsResetCallback() const {
        return static_cast<bool>(m_settingsResetCb);
    }

    [[nodiscard]] bool HasSettingsDescriptorLookupCallback() const {
        return static_cast<bool>(m_settingsDescriptorLookupCb);
    }

    [[nodiscard]] const SettingDescriptor* LookupSettingDescriptor(const std::string_view key) const {
        if (!m_settingsDescriptorLookupCb) {
            return nullptr;
        }

        return m_settingsDescriptorLookupCb(key);
    }

    [[nodiscard]] SettingsSetResult ValidateAndSetSettingValue(SettingsStore& settings,
                                                               const std::string_view key,
                                                               const JsonVariantConst value) const {
        if (!m_settingsValidateAndSetCb) {
            return SettingsSetResult::Ok;
        }

        return m_settingsValidateAndSetCb(settings, key, value);
    }

    [[nodiscard]] SettingsSetResult ResetSettingValue(
        SettingsStore& settings,
        const std::string_view key) const {
        if (!m_settingsResetCb) {
            return SettingsSetResult::StorageUnavailable;
        }

        return m_settingsResetCb(settings, key);
    }

    void SetCommandLineCallback(CommandLineCallback cb) { m_cmdLineCb = std::move(cb); }
    void SetEnteredLineCallback(EnteredLineCallback cb) { m_enteredLineCb = std::move(cb); }
    void SetUnknownCommandHandler(UnknownCommandHandler handler, void* context = nullptr) {
        m_unknownCommandHandler = handler;
        m_unknownCommandContext = context;
    }

    fxe::SettingsStore* GetSettingsStore() const { return m_settings; }

    // \brief Begin a line-streaming paste mode (Ctrl+D to finish).
    // Aggregate paste size is unbounded. Each line must fit within
    // INPUT_CAPACITY, including its null terminator.
    bool StartPasteMode(PasteHandler handler);

    // Help and optional usage are non-owning, lifetime-long metadata. Help search
    // reads only these strings, never command or completion handlers.
    bool RegisterCommand(const char* name, const char* help, CommandHandler handler);

    bool RegisterCommand(const char* name,
                         const char* help,
                         CommandHandler handler,
                         CompletionHandler completion,
                         const char* usage = nullptr);

    // \brief Poll the transport and execute completed commands.
    void Update();

    // \brief Print output while keeping the prompt stable.
    void Print(const char* fmt, ...);
    void PrintLn(const char* fmt, ...);
    void PrintRaw(const char* data, size_t len);

    // \brief Print multiple lines without redrawing the prompt between each.
    //
    // This avoids prompt redraw artifacts in some serial monitors when a
    // command prints many lines (e.g., `set` dumping all settings).
    void BeginBulkOutput();
    void EndBulkOutput();

    void PrintWelcome();

  private:
    struct HistoryEntry {
        char line[INPUT_CAPACITY] = {};
        uint8_t len = 0;
    };

    struct Command {
        const char* name = nullptr;
        const char* help = nullptr;
        const char* usage = nullptr;
        CommandHandler handler;
        CompletionHandler completion;
    };

    void InstallBuiltins();
    void InstallBuiltinHelp();
    void InstallBuiltinLogs();
    void InstallBuiltinDebug();
    void InstallBuiltinSettings();
    void InstallBuiltinSet();
    bool StartSettingsPaste(SettingsStore& settings);

    void HandleChar(char c, ConsoleInputSource source);
    void DiscardRetiredRemoteInput();
    void HandleEscapeSequenceChar(const char c);

    void HandlePasteChar(const char c);
    void SubmitPasteLine();
    void FinishPasteMode();

    void OnEnter();
    void OnBackspace();
    void OnTab();

    void RedrawPrompt();
    void BeginOutput();

    void WriteRaw(const char* data);
    void WriteRaw(const char* data, size_t len);

    bool ExecuteLine(const char* line,
                     ConsoleInputSource source = ConsoleInputSource::Unknown,
                     uint64_t remoteGeneration = 0U);
    Args SplitArgsInPlace(char* line) const;

    void HistoryPrev();
    void HistoryNext();
    void SetInputFromHistory();

    void PushHistoryLine(const char* line, size_t len);
    [[nodiscard]] bool HistoryEqualsLast(const char* line, size_t len) const;
    [[nodiscard]] size_t HistoryGetLine(size_t indexFromOldest,
                                        char* out,
                                        size_t outCap) const;

    size_t FindCompletions(const char* prefix, const char** outMatches, size_t outMatchesCap) const;
    static bool StartsWith(const char* s, const char* prefix);

    IConsoleTransport& m_transport;
    fxe::SettingsStore* m_settings = nullptr;

    Command m_commands[MAX_COMMANDS] = {};
    size_t m_commandCount = 0;

    const char* m_prompt = "> ";
    char m_input[INPUT_CAPACITY] = {};
    size_t m_inputLen = 0;
    bool m_inputRejected = false;
    ConsoleInputSource m_inputSource = ConsoleInputSource::Unknown;
    ConsoleInputSource m_commandInputSource = ConsoleInputSource::Unknown;
    bool m_inputSourceObserved = false;
    uint64_t m_inputRemoteGeneration = 0U;
    uint64_t m_commandRemoteInputGeneration = 0U;
    size_t m_lastRenderLen = 0;

    uint8_t m_bulkOutputDepth = 0;

    bool m_inEscape = false;
    bool m_inCsi = false;

    // Coalesce CRLF from some terminals/serial monitors.
    bool m_lastWasCR = false;

    // Line editing.
    size_t m_cursorPos = 0; // 0..m_inputLen

    // Minimal CSI parameter tracking (e.g., ESC [ 3 ~ for Delete).
    char m_csiLastParamChar = '\0';

    // Retain only the five most recent commands. Fixed entries keep the
    // implementation bounded and preserve the full input-line capacity.
    HistoryEntry m_historyEntries[HISTORY_CAPACITY] = {};

    size_t m_historyCount = 0;
    size_t m_historyOldestIndex = 0;
    size_t m_historyIndex = 0; // 0..m_historyCount (m_historyCount means scratch)
    char m_historyScratch[INPUT_CAPACITY] = {};

    bool m_pasteModeActive = false;
    bool m_pasteLineOverflow = false;
    bool m_pasteHandlerFailed = false;
    bool m_pasteInEscape = false;
    bool m_pasteInCsi = false;
    PasteHandler m_pasteHandler = nullptr;
    bool m_settingsPasteRebootPending = false;
    uint32_t m_settingsPasteRebootDelayMs = 0U;
    fxe::ElapsedTime m_settingsPasteRebootTime;

    char m_execLine[INPUT_CAPACITY] = {};

    CommandLineCallback m_cmdLineCb = nullptr;
    EnteredLineCallback m_enteredLineCb = nullptr;
    SettingsKeyEnumCallback m_settingsKeyEnumCb = nullptr;
    SettingsWritePolicyCallback m_settingsWritePolicyCb = nullptr;
    SettingsDiagnosticAccessCallback m_settingsDiagnosticAccessCb = nullptr;
    SettingsMutationCallback m_settingsMutationCb = nullptr;
    SettingsClearCallback m_settingsClearCb = nullptr;
    SettingsLoadCallback m_settingsLoadCb = nullptr;
    SettingsStorageAdmissionCallback m_settingsStorageAdmissionCb = nullptr;
    StorageReformatCallback m_storageReformatCb = nullptr;
    SettingsValidateAndSetCallback m_settingsValidateAndSetCb = nullptr;
    SettingsResetCallback m_settingsResetCb = nullptr;
    SettingsDescriptorLookupCallback m_settingsDescriptorLookupCb = nullptr;
    UnknownCommandHandler m_unknownCommandHandler = nullptr;
    void* m_unknownCommandContext = nullptr;
};

} // namespace fxe
