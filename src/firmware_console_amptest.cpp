#ifdef ARDUINO

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"

#include "hardware/hardware.hpp"

namespace firmware_internal::console_commands {

bool AmptestCommand(Console& console, const Console::Args& args) {
    if (!hardware::HW_IS_S3) {
        console.PrintLn("amptest: supported on ESP32-S3 hardware only");
        return false;
    }

    auto& sys = GetSystem();
    if (sys.comptest.IsActive()) {
        console.PrintLn("amptest: stop comptest first"); return false;
    }
    auto& session = sys.amptest;
    auto& verify = sys.amptestVerify;
    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();
    const bool selectsRaw = sub == "start" || sub == "restart" ||
        (sub.empty() && !verify.IsActive());
    const bool startsRaw = selectsRaw && (!session.IsActive() || sub == "restart");
    const bool startsVerify = sub == "verify" &&
        (!args.Has(2U) || args.Equals(2U, "start") || args.Equals(2U, "restart"));
    if ((startsRaw || startsVerify) &&
        (sys.factoryBootOnly || sys.rebootPersistencePending ||
         sys.ble.IsOtaInProgress() || sys.hardwareTest.IsActive() ||
         sys.sleep.GetState() != firmware::SleepState::Awake ||
         sys.gamePersistence.Litchcraft().IsActive() ||
         sys.gamePersistence.IsEngineLaneActive() ||
         sys.gamePersistence.GetGameClock().IsRunning())) {
        console.PrintLn("amptest: finish the active game, test, update or save before starting");
        return false;
    }

    if (sub.empty()) {
        if (session.IsActive()) {
            return session.HandleEnteredLine(console, {});
        }
        if (verify.IsActive()) {
            return verify.HandleEnteredLine(console, {});
        }
    }

    if (sub == "verify") {
        const std::string_view verifySub = args.Has(2) ? args.Get(2) : std::string_view();

        if (verifySub.empty() || verifySub == "start") {
            if (session.IsActive()) {
                session.Stop(console);
            }
            if (verify.IsActive()) {
                verify.PrintStatus(console);
                return true;
            }

            verify.Start(console);
            return verify.IsActive();
        }

        if (verifySub == "status") {
            verify.PrintStatus(console);
            return true;
        }

        if (verifySub == "stop") {
            verify.Stop(console);
            return true;
        }

        if (verifySub == "restart") {
            if (session.IsActive()) {
                session.Stop(console);
            }
            if (verify.IsActive()) {
                verify.Stop(console);
            }
            verify.Start(console);
            return verify.IsActive();
        }

        console.PrintLn("usage: amptest verify [start|status|stop|restart]");
        return false;
    }

    if (sub.empty() || sub == "start") {
        if (verify.IsActive()) {
            verify.Stop(console);
        }
        if (session.IsActive()) {
            session.PrintStatus(console);
            return true;
        }

        session.Start(console);
        return session.IsActive();
    }

    if (sub == "status") {
        session.PrintStatus(console);
        return true;
    }

    if (sub == "stop") {
        session.Stop(console);
        return true;
    }

    if (sub == "restart") {
        if (verify.IsActive()) {
            verify.Stop(console);
        }
        if (session.IsActive()) {
            session.Stop(console);
        }
        session.Start(console);
        return session.IsActive();
    }

    console.PrintLn("usage: amptest [start|status|stop|restart|verify [start|status|stop|restart]]");
    return false;
}

} // namespace firmware_internal::console_commands

#endif
