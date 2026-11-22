#include "modules/game_console_commands.hpp"

#include <litch_max/fen_constants.hpp>
#include "modules/game_persistence_coordinator.hpp"

namespace modules {

bool FenCommand(GameConsoleCommandContext& ctx,
                 const Console::Args& args,
                 const size_t firstFenArgIndex) {
    if (ctx.console == nullptr || ctx.gamePersistence == nullptr) {
        return false;
    }
    if (args.overflow || args.argc > Console::MAX_ARGS) {
        ctx.console->PrintLn("FAIL");
        return false;
    }
    if (!args.Has(firstFenArgIndex)) {
        const char* currentFen = ctx.gamePersistence->GetAuthoritativeFen();
        if (currentFen == nullptr || currentFen[0] == '\0') {
            ctx.console->PrintLn("(unavailable)");
            return false;
        }
        ctx.console->PrintLn("%s", currentFen);
        return true;
    }

    FenString fen;
    for (size_t index = firstFenArgIndex; index < args.argc; ++index) {
        const std::string_view field = args.Get(index);
        for (const unsigned char character : field) {
            if (character < 32U || character > 126U) {
                ctx.console->PrintLn("FAIL");
                return false;
            }
        }
        if ((index != firstFenArgIndex && !fen.PushBack(' ')) ||
            !fen.Append(field)) {
            ctx.console->PrintLn("FAIL");
            return false;
        }
    }
    const bool accepted = ctx.gamePersistence->LoadExternalFen(fen.CStr());
    ctx.console->PrintLn(accepted ? "OK" : "FAIL");
    return accepted;
}

} // namespace modules
