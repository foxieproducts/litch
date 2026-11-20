#!/usr/bin/env python3
"""Collect one uninterrupted production hint search, then Stockfish regret."""
import argparse
import json
import shlex
import subprocess
import chess
import chess.engine


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", required=True)
    ap.add_argument("--positions", required=True, help="JSON list of {id,fen}")
    ap.add_argument("--stockfish", default="/opt/homebrew/bin/stockfish")
    ap.add_argument("--reference-nodes", type=int, default=1000000)
    ap.add_argument("--output", required=True, help="New JSONL output; raw samples flushed immediately")
    ap.add_argument("--waits-ms", default="1000,3000,10000,30000,90000")
    ap.add_argument("--node-budgets", help="Sample one continuous search by observed nodes instead of host time")
    args = ap.parse_args()
    waits = [int(value) for value in args.waits_ms.split(",")]
    if not waits or any(value < 500 or value > 90000 for value in waits):
        ap.error("waits must be in500..90000ms")
    nodes = [int(value) for value in args.node_budgets.split(",")] if args.node_budgets else None
    if nodes is not None and (not nodes or any(value < 1 or value > 0xFFFFFFFF for value in nodes)):
        ap.error("node budgets must be in 1..4294967295")
    budgets = nodes if nodes is not None else waits
    command = "hintbenchnodes" if nodes is not None else "hintbench"
    sample_prefix = 'info string {"hint_node_sample"' if nodes is not None else 'info string {"hint_sample"'
    budget_key = "node_budget" if nodes is not None else "wait_ms"
    with open(args.positions, encoding="utf-8") as source: positions = json.load(source)
    with open(args.output, "x", encoding="utf-8") as output:
        for position in positions:
            board = chess.Board(position["fen"])
            if not board.is_valid() or board.is_game_over(): raise ValueError("position must be valid and nonterminal")
            samples = []
            # Engine dies/restarts per position: no retained hash state from
            # another benchmark position; all wait points share one search.
            with subprocess.Popen(shlex.split(args.engine), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True) as engine:
                engine.stdin.write("position fen " + board.fen() + "\n" + command + " " +
                                   " ".join(map(str, budgets)) + "\nquit\n")
                engine.stdin.flush()
                for line in engine.stdout:
                    if not line.startswith(sample_prefix): continue
                    sample = json.loads(line[len("info string "):])
                    sample.update(kind="hint", position=position.get("id", board.fen()), fen=board.fen(),
                                  platform="host", engine_command=args.engine)
                    output.write(json.dumps(sample)+"\n"); output.flush()
                    samples.append(sample)
                if engine.wait() != 0: raise RuntimeError("hint engine failed")
            if len(samples) != len(set(budgets)): raise RuntimeError("missing hint snapshots")
            # Reference grading is sequential, after hint engine quiescence.
            with chess.engine.SimpleEngine.popen_uci(shlex.split(args.stockfish)) as reference:
                reference.configure({"Threads": 1, "Hash": 64, "UCI_LimitStrength": False})
                budget = chess.engine.Limit(nodes=args.reference_nodes)
                for sample in samples:
                    move = chess.Move.from_uci(sample["move"])
                    if move not in board.legal_moves: raise ValueError("hint sample missing or illegal")
                    reference.configure({"Clear Hash": None})
                    best = reference.analyse(board, budget)
                    reference.configure({"Clear Hash": None})
                    chosen = reference.analyse(board, budget, root_moves=[move])
                    best_cp = best["score"].pov(board.turn).score(mate_score=100000)
                    chosen_cp = chosen["score"].pov(board.turn).score(mate_score=100000)
                    record = dict(kind="reference", position=sample["position"],
                                  move=sample["move"], reference=reference.id.get("name"),
                                  reference_nodes_per_search=args.reference_nodes,
                                  best_score=str(best["score"].pov(board.turn)),
                                  hint_score=str(chosen["score"].pov(board.turn)),
                                  cp_regret_estimate=best_cp-chosen_cp,
                                  note="finite-search estimate; mate scores use100000 sentinel; not Elo")
                    record[budget_key] = sample[budget_key]
                    output.write(json.dumps(record)+"\n"); output.flush()
    return 0

if __name__ == "__main__": raise SystemExit(main())
