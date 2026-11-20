#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"
#include "hint_benchmark.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/classic_search_config.hpp"
#include "modules/chess_strings.hpp"
#include "modules/vs_cpu_difficulty.hpp"
#include "modules/vs_cpu_rng.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"

namespace {

std::vector<std::string> SplitTokens(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) {
        out.push_back(tok);
    }
    return out;
}

std::string JoinFenFromTokens(const std::vector<std::string>& toks, size_t startIdx) {
    // UCI: "position fen <fen fields...>" where fen is 6 space-separated fields.
    // We'll join exactly 6 tokens when available.
    if (toks.size() < startIdx + 6) {
        return std::string();
    }
    std::ostringstream oss;
    for (size_t i = 0; i < 6; ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << toks[startIdx + i];
    }
    return oss.str();
}

bool TryApplyReplayMove(modules::LitchMaxEngine& engine, const std::string& token,
                        std::string& canonical, uint16_t& reversibleHalfmoves) {
    litch_max::ParsedUciMove parsed;
    if (!litch_max::TryParseUciMove(token.c_str(), parsed)) return false;
    char fen[128]{};
    char piece = '\0';
    char captured = '\0';
    if (!engine.GetFen(fen, sizeof(fen)) ||
        !modules::TryGetFenPieceAtSquare(fen, parsed.fromSquare, piece)) return false;
    (void)modules::TryGetFenPieceAtSquare(fen, parsed.toSquare, captured);
    char promotion = parsed.promotionSuffixLower;
    if (!parsed.hasPromotion) {
        if ((piece == 'P' && parsed.toSquare < 8U) ||
            (piece == 'p' && parsed.toSquare >= 56U)) promotion = 'q';
    }
    modules::UciMove move;
    if (!litch_max::TryFormatUciMove(parsed.fromSquare, parsed.toSquare, promotion, move) ||
        !engine.ApplyUciMove(move.CStr())) return false;
    // Like the board, an imported clock is unknown until a retained pawn move
    // or capture establishes it. Do not trust the imported FEN's clock field.
    if (piece == 'p' || piece == 'P' || captured != '\0') reversibleHalfmoves = 0U;
    else if (reversibleHalfmoves != UINT16_MAX) ++reversibleHalfmoves;
    canonical = move.CStr();
    return true;
}

void FillReplayKnowledge(modules::ThinkConfig& config, const bool openingPrefix,
                         const std::vector<std::string>& moves,
                         const uint16_t reversibleHalfmoves) {
    config.reversibleHalfmoves = reversibleHalfmoves;
    if (!openingPrefix || moves.size() > modules::THINK_OPENING_MAX_PLIES) return;
    for (size_t ply = 0U; ply < moves.size(); ++ply) {
        litch_max::ParsedUciMove move;
        if (!litch_max::TryParseUciMove(moves[ply].c_str(), move) || move.hasPromotion) return;
        config.openingPrefix.moves[ply].fromSquare = move.fromSquare;
        config.openingPrefix.moves[ply].toSquare = move.toSquare;
    }
    config.openingPrefix.moveCount = static_cast<uint8_t>(moves.size());
    config.openingPrefix.fromCanonicalStart = true;
}

} // namespace

int main() {
    // UCI engines must not emit extra stdout noise.
    logger::SetMinLevel(logger::Level::ERROR);
    logger::SetOutputLineCallback([](const char*) {});

    modules::LitchMaxEngine engine;
    engine.Reset();

    uint32_t difficulty = 3;
    uint32_t rngSeed = 1;
    uint32_t debug = 0;
    bool positionValid = true;
    bool continuousHint = false;
    bool litchCpuPolicy = false;
    bool openingPrefix = true;
    std::vector<std::string> positionMoves;
    uint32_t positionSeed = 1U;
    uint16_t reversibleHalfmoves = 0U;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (debug != 0U) {
            std::cerr << "uci< " << line << "\n";
        }
        const auto toks = SplitTokens(line);
        if (toks.empty()) {
            continue;
        }

        const std::string& cmd = toks[0];
        if (cmd == "uci") {
            std::cout << "id name Litch-Max" << "\n";
            std::cout << "id author litch" << "\n";
            std::cout << "option name Difficulty type spin default 3 min 0 max 11" << "\n";
            std::cout << "option name Seed type spin default 1 min 1 max 2147483647" << "\n";
            std::cout << "option name Debug type spin default 0 min 0 max 1" << "\n";
            std::cout << "option name ContinuousHint type check default false\n";
            std::cout << "option name LitchCpuPolicy type check default false\n";
            std::cout << "uciok" << "\n";
            std::cout.flush();
            continue;
        }

        if (cmd == "isready") {
            std::cout << "readyok" << "\n";
            std::cout.flush();
            continue;
        }

        if (cmd == "setoption") {
            // Minimal: setoption name Difficulty value N
            // Minimal: setoption name Seed value N
            if (toks.size() >= 5 && toks[1] == "name") {
                const std::string& optName = toks[2];
                if (toks[3] == "value") {
                    if (optName == "ContinuousHint") {
                        continuousHint = toks[4] == "true" || toks[4] == "1";
                        continue;
                    }
                    if (optName == "LitchCpuPolicy") {
                        litchCpuPolicy = toks[4] == "true" || toks[4] == "1";
                        continue;
                    }
                    const uint32_t v = static_cast<uint32_t>(std::stoul(toks[4]));
                    if (optName == "Difficulty") {
                        difficulty = v;
                    } else if (optName == "Seed") {
                        rngSeed = v;
                        positionSeed = rngSeed;
                        for (const auto& move : positionMoves) {
                            positionSeed = modules::NextVsCpuSeedAfterMove(positionSeed, move.c_str());
                        }
                    } else if (optName == "Debug") {
                        debug = v;
                        if (debug != 0U) {
                            logger::SetMinLevel(logger::Level::DEBUG);
                            logger::SetOutputLineCallback([](const char* line) {
                                if (line == nullptr) {
                                    return;
                                }
                                std::cerr << line << "\n";
                            });
                        } else {
                            logger::SetMinLevel(logger::Level::ERROR);
                            logger::SetOutputLineCallback([](const char*) {});
                        }
                    }
                }
            }
            continue;
        }

        if (cmd == "ucinewgame") {
            engine.Reset();
            positionValid = true;
            openingPrefix = true;
            positionMoves.clear();
            positionSeed = rngSeed;
            reversibleHalfmoves = 0U;
            continue;
        }

        if (cmd == "position") {
            openingPrefix = false;
            positionMoves.clear();
            positionSeed = rngSeed;
            reversibleHalfmoves = UINT16_MAX;
            if (toks.size() >= 2 && toks[1] == "startpos") {
                engine.Reset();
                positionValid = true;
                openingPrefix = true;
                reversibleHalfmoves = 0U;
                size_t movesIdx = 2;
                if (movesIdx < toks.size() && toks[movesIdx] == "moves") {
                    ++movesIdx;
                }
                for (size_t i = movesIdx; i < toks.size(); ++i) {
                    std::string canonical;
                    if (!TryApplyReplayMove(engine, toks[i], canonical, reversibleHalfmoves)) {
                        positionValid = false;
                        if (debug != 0U) {
                            std::cerr << "uci: ApplyUciMove failed (" << toks[i] << ")\n";
                        }
                        break;
                    }
                    positionSeed = modules::NextVsCpuSeedAfterMove(positionSeed, canonical.c_str());
                    positionMoves.push_back(canonical);
                }
                continue;
            }

            if (toks.size() >= 3 && toks[1] == "fen") {
                const std::string fen = JoinFenFromTokens(toks, 2);
                if (!fen.empty()) {
                    if (!engine.SetFen(fen.c_str())) {
                        positionValid = false;
                        if (debug != 0U) {
                            std::cerr << "uci: SetFen failed\n";
                        }
                    } else {
                        positionValid = true;
                    }
                } else {
                    positionValid = false;
                    if (debug != 0U) {
                        std::cerr << "uci: JoinFenFromTokens failed\n";
                    }
                }

                size_t movesIdx = 2 + 6;
                if (movesIdx < toks.size() && toks[movesIdx] == "moves") {
                    ++movesIdx;
                }
                for (size_t i = movesIdx; i < toks.size(); ++i) {
                    std::string canonical;
                    if (!TryApplyReplayMove(engine, toks[i], canonical, reversibleHalfmoves)) {
                        positionValid = false;
                        if (debug != 0U) {
                            std::cerr << "uci: ApplyUciMove failed (" << toks[i] << ")\n";
                        }
                        break;
                    }
                    positionSeed = modules::NextVsCpuSeedAfterMove(positionSeed, canonical.c_str());
                    positionMoves.push_back(canonical);
                }
                continue;
            }

            continue;
        }

        if (cmd == "hintfirst") {
            uint32_t budget = 0U;
            const std::string token = toks.size() == 2U ? toks[1] : "";
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), budget);
            if (!positionValid || parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() || budget == 0U || budget > 16000000U) {
                std::cout << "info string hintfirst requires a valid position and 1..16000000 nodes\n"
                          << "bestmove 0000\n" << std::flush;
            } else RunHintFirstBenchmark(engine, budget, rngSeed);
            continue;
        }

        if (cmd == "hintbench" || cmd == "hintbenchnodes") {
            std::vector<uint32_t> waits;
            for (size_t i = 1U; i < toks.size(); ++i) waits.push_back(std::stoul(toks[i]));
            if (positionValid && cmd == "hintbenchnodes") RunHintNodeBenchmark(engine, waits, rngSeed);
            else if (positionValid) RunHintBenchmark(engine, waits, rngSeed);
            else std::cout << "bestmove 0000\n" << std::flush;
            continue;
        }

        if (cmd == "go") {
            if (!positionValid) {
                if (debug != 0U) {
                    std::cerr << "uci: position invalid => bestmove 0000\n";
                }
                std::cout << "bestmove 0000" << "\n";
                std::cout.flush();
                continue;
            }

            if (continuousHint) {
                uint32_t waitMs = 0U;
                for (size_t i = 1U; i + 1U < toks.size(); ++i)
                    if (toks[i] == "movetime") waitMs = std::stoul(toks[i + 1U]);
                if (waitMs == 0U) {
                    std::cout << "info string ContinuousHint requires go movetime\n"
                              << "bestmove 0000\n" << std::flush;
                } else RunHintBenchmark(engine, {waitMs}, rngSeed);
                continue;
            }

            modules::ThinkConfig cfg = litchCpuPolicy
                ? modules::MakeThinkConfigForVsCpuTurn(
                      difficulty, openingPrefix ? static_cast<uint32_t>(positionMoves.size() / 2U) : 2U, positionSeed)
                : modules::MakeThinkConfigForVsCpuLevel(difficulty, rngSeed);
            if (litchCpuPolicy) FillReplayKnowledge(cfg, openingPrefix, positionMoves, reversibleHalfmoves);

            bool haveNodes = false;
            for (size_t i = 1; i + 1 < toks.size(); ++i) {
                if (toks[i] == "nodes") {
                    const uint32_t limitNodes = static_cast<uint32_t>(std::stoul(toks[i + 1]));
                    if (cfg.maxNodes == 0U) {
                        cfg.maxNodes = limitNodes;
                    } else {
                        cfg.maxNodes = std::min(cfg.maxNodes, limitNodes);
                    }
                    haveNodes = true;
                }
                if (toks[i] == "depth") {
                    const uint32_t limitDepth = static_cast<uint32_t>(std::stoul(toks[i + 1]));
                    if (cfg.maxDepth == 0U) {
                        cfg.maxDepth = limitDepth;
                    } else {
                        cfg.maxDepth = std::min(cfg.maxDepth, limitDepth);
                    }
                }
                if (toks[i] == "movetime") {
                    cfg.maxThinkMs = static_cast<uint32_t>(std::stoul(toks[i + 1]));
                }
            }

            // A host work-model ceiling must also bound the first-root reserve.
            // Normal board profiles retain their unchanged production allowance.
            if (haveNodes) cfg.firstRootNodeReserve = std::min<uint32_t>(cfg.firstRootNodeReserve, cfg.maxNodes);

            modules::ThinkCandidate completedCandidate;
            cfg.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
                *static_cast<modules::ThinkCandidate*>(context) = candidate;
            }, &completedCandidate};
            modules::ThinkCandidate immediate;
            if (litchCpuPolicy && engine.GetImmediateThinkCandidate(immediate)) {
                // The board captures this before worker admission too.
                cfg.candidateObserver.Publish(immediate);
            }
            modules::UciMove best;
            const bool started = engine.StartThink(cfg);
            if (!started) {
                if (debug != 0U) {
                    std::cerr << "uci: StartThink failed\n";
                }
            }

            uint8_t progress = 0;
            while (started) {
                const modules::ThinkState st = engine.PollThinkProgress(progress, best);
                if (st == modules::ThinkState::Done) {
                    break;
                }
                if (st == modules::ThinkState::Canceled || st == modules::ThinkState::Error) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const bool seedFallback = litchCpuPolicy && best.Empty() && immediate.IsAvailable();
            if (seedFallback) {
                (void)litch_max::TryFormatUciMove(immediate.fromSquare, immediate.toSquare,
                                               immediate.promotion, best);
                completedCandidate = immediate;
            }
            const modules::UciMove searched = best;
            bool policyRejected = false;
            if (litchCpuPolicy && !best.Empty()) {
                litch_max::ParsedUciMove parsed;
                modules::UciMove selected;
                if (!modules::game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
                        engine, best, parsed, selected)) {
                    policyRejected = true;
                    best.Clear();
                } else {
                    best = selected;
                }
            }
            modules::ThinkStats stats;
            if (started) engine.GetLastThinkStats(stats);
            modules::ClassicThinkResult classicResult;
            if (started) engine.GetLastClassicThinkResult(classicResult);
            std::cout << "info string {\"cpu_sample\":true,\"nodes\":" << stats.nodeCount
                      << ",\"workspace_bytes\":" << litch_max::classic_chess::SearchWorkspace::RequiredBytes()
                      << ",\"decision_source\":" << static_cast<unsigned>(completedCandidate.source)
                      << ",\"root_kind\":" << static_cast<unsigned>(completedCandidate.rootKind)
                      << ",\"search_started\":" << (started ? "true" : "false")
                      << ",\"seed_fallback\":" << (seedFallback ? "true" : "false")
                      << ",\"completed_root_depth\":" << static_cast<unsigned>(completedCandidate.completedDepth)
                      << ",\"preliminary_only\":" << (classicResult.preliminaryOnly ? "true" : "false")
                      << ",\"best_score\":" << classicResult.bestScore
                      << ",\"best_score_valid\":" << (classicResult.bestScoreValid ? "true" : "false")
                      << ",\"static_leaf_only\":" << (modules::ResolveClassicSearchConfig(cfg).staticLeafOnly ? "true" : "false")
                      << ",\"cpu_policy\":" << (litchCpuPolicy ? "true" : "false")
                      << ",\"cpu_policy_id\":\"" << modules::CLASSIC_CPU_POLICY_ID << "\""
                      << ",\"selected_level\":" << difficulty
                      << ",\"profile_level\":" << static_cast<unsigned>(cfg.difficulty)
                      << ",\"max_nodes\":" << cfg.maxNodes
                      << ",\"first_root_node_reserve\":" << cfg.firstRootNodeReserve
                      << ",\"max_depth\":" << cfg.maxDepth
                      << ",\"max_think_ms\":" << cfg.maxThinkMs
                      << ",\"seed\":" << cfg.rngSeed
                      << ",\"reversible_halfmoves\":" << cfg.reversibleHalfmoves
                      << ",\"opening_history\":" << (cfg.openingPrefix.fromCanonicalStart ? "true" : "false")
                      << ",\"opening_plies\":" << static_cast<unsigned>(cfg.openingPrefix.moveCount)
                      << ",\"searched_move\":\"" << searched.CStr() << "\""
                      << ",\"novice_substitution\":" << (litchCpuPolicy && !best.Empty() && !best.Equals(searched.View()) ? "true" : "false")
                      << ",\"policy_rejected\":" << (policyRejected ? "true" : "false")
                      << "}\n";
            if (!best.Empty()) {
                std::cout << "bestmove " << best.CStr() << "\n";
            } else {
                if (debug != 0U) {
                    std::cerr << "uci: best empty => bestmove 0000\n";
                }
                std::cout << "bestmove 0000" << "\n";
            }
            std::cout.flush();
            continue;
        }

        if (cmd == "stop") {
            engine.CancelThink();
            continue;
        }

        if (cmd == "quit") {
            break;
        }
    }

    return 0;
}
