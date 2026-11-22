#include "hint_benchmark.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include "modules/async_think_worker.hpp"
#include "modules/classic_search_config.hpp"

namespace {
std::string CandidateMove(const modules::ThinkCandidate& candidate) {
    if (candidate.fromSquare >= 64U || candidate.toSquare >= 64U) return "0000";
    std::string move;
    move += static_cast<char>('a' + candidate.fromSquare % 8U);
    move += static_cast<char>('8' - candidate.fromSquare / 8U);
    move += static_cast<char>('a' + candidate.toSquare % 8U);
    move += static_cast<char>('8' - candidate.toSquare / 8U);
    if (candidate.promotion != '\0') move += candidate.promotion;
    return move;
}
}

void RunHintBenchmark(modules::LitchMaxEngine& engine,
                      std::vector<uint32_t> waitsMs, const uint32_t seed) {
    for (auto& wait : waitsMs) wait = std::max(500U, std::min(wait, 90000U));
    std::sort(waitsMs.begin(), waitsMs.end());
    waitsMs.erase(std::unique(waitsMs.begin(), waitsMs.end()), waitsMs.end());
    if (waitsMs.empty()) waitsMs.push_back(90000U);
    modules::AsyncThinkWorker worker;
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxThinkMs = 90000U;
    config.rngSeed = seed;
    config.candidateObserver = worker.PrepareCandidateObserver();
    config.progressObserver = worker.PrepareProgressObserver(true);
    const bool started = engine.StartThink(config);
    if (!started || !worker.Start(engine)) {
        if (started) {
            engine.CancelThink();
            uint8_t progress = 0U;
            modules::UciMove ignored;
            (void)engine.PollThinkProgress(progress, ignored);
        }
        std::cout << "info string hint_start_failed\nbestmove 0000\n" << std::flush;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&]() {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    };
    uint32_t finishedAt = 0U;
    bool finished = false;
    std::string move = "0000";
    for (const auto wait : waitsMs) {
        while (elapsed() < wait) {
            if (!worker.IsActive() && !finished) {
                finished = true;
                finishedAt = elapsed();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const uint32_t sampledAt = elapsed();
        if (!worker.IsActive() && !finished) {
            finished = true;
            finishedAt = sampledAt;
        }
        modules::ThinkCandidate candidate;
        const bool available = worker.TryGetPublishedCandidate(candidate);
        move = available ? CandidateMove(candidate) : "0000";
        const uint32_t nodes = worker.PublishedNodeCount();
        std::cout << "info string {\"hint_sample\":true,\"wait_ms\":" << wait
                  << ",\"sample_ms\":" << sampledAt
                  << ",\"calculation_ms_observed\":" << (finished ? finishedAt : sampledAt)
                  << ",\"nodes_observed\":" << nodes
                  << ",\"completed_root_depth\":"
                  << static_cast<unsigned>(candidate.completedDepth)
                  << ",\"search_active\":" << (finished ? "false" : "true")
                  << ",\"move\":\"" << move << "\"}\n" << std::flush;
    }
    std::cout << "info time " << (finished ? finishedAt : elapsed())
              << " nodes " << worker.PublishedNodeCount()
              << " pv " << move << '\n';
    // The requested candidate is emitted before cancellation/cleanup, matching
    // an immediate user request. Later input waits only for engine-lane reuse.
    std::cout << "bestmove " << move << '\n' << std::flush;
    while (!worker.CancelAndJoin(engine)) std::this_thread::yield();
}

void RunHintNodeBenchmark(modules::LitchMaxEngine& engine,
                          std::vector<uint32_t> nodeBudgets,
                          const uint32_t seed) {
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    if (!engine.GetFen(fen, sizeof(fen)) || !engine.SetFen(fen)) {
        std::cout << "info string hint_position_failed\nbestmove 0000\n" << std::flush;
        return;
    }
    nodeBudgets.erase(std::remove(nodeBudgets.begin(), nodeBudgets.end(), 0U),
                      nodeBudgets.end());
    std::sort(nodeBudgets.begin(), nodeBudgets.end());
    nodeBudgets.erase(std::unique(nodeBudgets.begin(), nodeBudgets.end()),
                      nodeBudgets.end());
    if (nodeBudgets.empty()) nodeBudgets = {1024U, 4096U, 16384U, 65536U, 262144U};
    struct Sample {
        uint32_t budget = 0U;
        uint32_t observedNodes = 0U;
        uint8_t completedDepth = 0U;
        modules::ThinkCandidate candidate;
    };
    struct Probe {
        std::vector<Sample> samples;
        size_t next = 0U;
        modules::ThinkCandidate latest;
        uint8_t depth = 0U;

        void Observe(const uint32_t nodes) {
            while (next < samples.size() && nodes >= samples[next].budget) {
                auto& sample = samples[next++];
                sample.observedNodes = nodes;
                sample.completedDepth = depth;
                sample.candidate = latest;
            }
        }
    } probe;
    for (const auto budget : nodeBudgets) {
        Sample sample;
        sample.budget = budget;
        probe.samples.push_back(sample);
    }
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxNodes = nodeBudgets.back();
    config.rngSeed = seed;
    // Completed depth travels with the immutable candidate in both the
    // synchronous work probe and the asynchronous wall-time mailbox.
    config.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
        auto& observation = *static_cast<Probe*>(context);
        observation.latest = candidate;
        observation.depth = candidate.completedDepth;
    }, &probe};
    config.progressObserver = {[](void* context, const uint32_t nodes) {
        static_cast<Probe*>(context)->Observe(nodes);
    }, &probe};
    if (!engine.StartThink(config)) {
        std::cout << "info string hint_start_failed\nbestmove 0000\n" << std::flush;
        return;
    }
    uint8_t progress = 0U;
    modules::UciMove best;
    const auto state = engine.PollThinkProgress(progress, best);
    modules::ThinkStats stats;
    engine.GetLastThinkStats(stats);
    probe.Observe(stats.nodeCount);
    // Natural terminal/depth exhaustion may finish before later requested
    // budgets. Retain that completed candidate and report actual work.
    while (probe.next < probe.samples.size()) {
        auto& sample = probe.samples[probe.next++];
        sample.observedNodes = stats.nodeCount;
        sample.completedDepth = probe.depth;
        sample.candidate = probe.latest;
    }
    for (const auto& sample : probe.samples) {
        std::cout << "info string {\"hint_node_sample\":true,\"node_budget\":"
                  << sample.budget << ",\"nodes_observed\":" << sample.observedNodes
                  << ",\"completed_root_depth\":" << static_cast<unsigned>(sample.completedDepth)
                  << ",\"move\":\"" << CandidateMove(sample.candidate) << "\"}\n";
    }
    std::cout << "info string hint_terminal_state=" << static_cast<unsigned>(state)
              << "\nbestmove " << CandidateMove(probe.latest) << '\n' << std::flush;
}

void RunHintFirstBenchmark(modules::LitchMaxEngine& engine,
                           const uint32_t maxNodes, const uint32_t seed) {
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    if (!engine.GetFen(fen, sizeof(fen)) || !engine.SetFen(fen)) {
        std::cout << "info string hint_position_failed\nbestmove 0000\n" << std::flush;
        return;
    }
    struct Probe {
        modules::ThinkCandidate first;
        uint8_t completedDepth = 0U;
        uint32_t roots = 0U;
    } probe;
    modules::ThinkConfig config;
    config.continuousAnalysis = true;
    config.maxThinkMs = 90000U;
    config.maxNodes = maxNodes;
    // Earlier iterations match an unrestricted Hint. No later refinement is
    // needed to measure the first result; do not spend a full window per FEN.
    config.maxDepth = modules::CLASSIC_MIN_HINT_PLIES;
    config.rngSeed = seed;
    config.candidateObserver = {
        [](void* context, const modules::ThinkCandidate& candidate) {
            auto& probe = *static_cast<Probe*>(context);
            if (!probe.first.IsAvailable()) probe.first = candidate;
        }, &probe,
        [](void* context, const modules::ThinkCandidate& candidate) {
            auto& probe = *static_cast<Probe*>(context);
            probe.completedDepth = candidate.completedDepth;
            ++probe.roots;
        }};
    const auto start = std::chrono::steady_clock::now();
    uint8_t progress = 0U;
    modules::UciMove best;
    const bool started = engine.StartThink(config);
    const auto state = started ? engine.PollThinkProgress(progress, best)
                               : modules::ThinkState::Error;
    modules::ThinkStats stats;
    engine.GetLastThinkStats(stats);
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "info string {\"hint_first\":true,\"started\":" << (started ? "true" : "false")
              << ",\"state\":" << static_cast<unsigned>(state)
              << ",\"node_cap\":" << maxNodes
              << ",\"time_cap_ms\":" << config.maxThinkMs
              << ",\"first_nodes\":" << probe.first.completedNodes
              << ",\"first_depth\":" << static_cast<unsigned>(probe.first.completedDepth)
              << ",\"root_kind\":" << static_cast<unsigned>(probe.first.rootKind)
              << ",\"source\":" << static_cast<unsigned>(probe.first.source)
              << ",\"score\":" << probe.first.score
              << ",\"score_valid\":" << (probe.first.scoreValid ? "true" : "false")
              << ",\"completed_depth\":" << static_cast<unsigned>(probe.completedDepth)
              << ",\"completed_roots\":" << probe.roots
              << ",\"nodes\":" << stats.nodeCount
              << ",\"host_us\":" << elapsedUs
              << ",\"move\":\"" << CandidateMove(probe.first) << "\"}\n"
              << "bestmove " << CandidateMove(probe.first) << '\n' << std::flush;
}
