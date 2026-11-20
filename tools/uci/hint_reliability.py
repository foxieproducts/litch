#!/usr/bin/env python3
"""Bounded legal-play screen of the engine's first completed scored Full root.

Host time is diagnostic, never an ESP32-S3 latency measurement. A node-cap miss
is a work-cost counterexample, not proof that the production 90s window fails.
The synchronous probe bypasses the human Hint worker and its 500 ms reveal gate.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import random
import selectors
import subprocess
import time

import chess


def make_corpus(seed: int, count: int) -> tuple[list[dict], dict]:
    rng = random.Random(seed)
    quotas = {"opening": count * 24 // 100, "middlegame": count // 2}
    quotas["endgame"] = count - sum(quotas.values())
    counts: Counter = Counter()
    terminals: Counter = Counter()
    positions = {}
    for game in range(2000):
        board = chess.Board()
        moves = []
        for ply in range(1, 361):
            legal = sorted(board.legal_moves, key=lambda move: move.uci())
            if not legal:
                break
            captures = [move for move in legal if board.is_capture(move)]
            preferred = (game % 3 != 0 and captures and rng.random() < 0.72)
            move = rng.choice(captures if preferred else legal)
            moves.append(move.uci())
            board.push(move)
            outcome = board.outcome(claim_draw=False)
            if outcome:
                terminals[outcome.termination.name] += 1
                break
            category = ("opening" if ply <= 20 else
                        "endgame" if len(board.piece_map()) <= 10 else "middlegame")
            # Rule identity: placement, turn, effective castling and legal EP.
            # Move counters do not create another distinct sampled position.
            key = " ".join(board.fen(en_passant="legal").split()[:4])
            if counts[category] >= quotas[category] or key in positions:
                continue
            positions[key] = {
                "fen": board.fen(), "moves": moves[:], "game": game, "ply": ply,
                "category": category, "checked": board.is_check(),
                "captures": sum(board.is_capture(move) for move in board.legal_moves),
            }
            counts[category] += 1
        if all(counts[key] >= quota for key, quota in quotas.items()):
            break
    if len(positions) != count:
        raise RuntimeError(f"Bounded corpus generation exhausted: {dict(counts)}")
    rows = list(positions.values())
    rng.shuffle(rows)
    identity = "".join(json.dumps(row, separators=(",", ":")) + "\n" for row in rows)
    return rows, {
        "seed": seed, "count": count, "categories": dict(counts),
        "checked": sum(row["checked"] for row in rows),
        "capture_capable": sum(row["captures"] > 0 for row in rows),
        "terminal_exclusions": dict(terminals),
        "sha256": hashlib.sha256(identity.encode()).hexdigest(),
        "python_chess": chess.__version__,
    }


class HintEngine:
    """One sequential process; each imported FEN starts a fresh analysis context."""
    def __init__(self, binary: Path):
        self.process = subprocess.Popen(
            [str(binary.resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def first(self, fen: str, cap: int, timeout: float) -> dict:
        self.process.stdin.write(f"position fen {fen}\nhintfirst {cap}\n")
        self.process.stdin.flush()
        end = time.monotonic() + timeout
        sample = None
        while time.monotonic() < end:
            # hintfirst emits two adjacent lines. Once the sample is read,
            # readline consumes the already-buffered bestmove without select.
            if sample is None and not self.selector.select(max(0, end - time.monotonic())):
                break
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("Hint engine exited before its result")
            if line.startswith('info string {"hint_first"'):
                sample = json.loads(line.removeprefix("info string "))
            elif line.startswith("bestmove "):
                if sample is None:
                    raise RuntimeError("Missing first-result provenance")
                if line.split()[1] != sample["move"]:
                    raise RuntimeError("Hint sample and result disagree")
                return sample
        raise TimeoutError("Host screen deadline reached; not an S3 timing result")

    def close(self):
        self.selector.close()
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()


def quantiles(values: list[int]) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {}
    return {name: ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]
            for name, fraction in (("median", 0.5), ("p90", 0.9),
                                   ("p99", 0.99), ("max", 1.0))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New temporary JSONL result")
    parser.add_argument("--seed", type=int, default=20261026)
    parser.add_argument("--count", type=int, default=5000)
    parser.add_argument("--node-cap", type=int, default=8000000)
    parser.add_argument("--total-nodes", type=int, default=600000000)
    parser.add_argument("--wall-seconds", type=int, default=600)
    args = parser.parse_args()
    if not 20 <= args.count <= 10000 or not 1 <= args.node_cap <= 16000000:
        parser.error("count must be 20..10000; node cap must be 1..16000000")
    if args.total_nodes < args.node_cap or not 1 <= args.wall_seconds <= 1800:
        parser.error("total nodes must cover one position; wall limit must be 1..1800s")
    rows, metadata = make_corpus(args.seed, args.count)
    metadata.update(kind="corpus", node_cap=args.node_cap, total_node_cap=args.total_nodes,
                    wall_cap_s=args.wall_seconds,
                    scope="imported FEN first completed Full root; no Book/history/human reveal/physical timing acceptance")
    engine = HintEngine(args.engine)
    results = []
    nodes = 0
    failure = None
    start = time.monotonic()
    try:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(json.dumps(metadata) + "\n")
            print(json.dumps(metadata), flush=True)
            # Actual no-legal-move controls are expected unavailable, distinct
            # from a nonterminal result exhausting the diagnostic node budget.
            for fen in ("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
                        "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"):
                sample = engine.first(fen, args.node_cap, 10)
                if sample["move"] != "0000":
                    raise RuntimeError("Terminal position fabricated a Hint")
                output.write(json.dumps(dict(kind="terminal", fen=fen, **sample)) + "\n")
            for index, row in enumerate(rows):
                remaining = args.wall_seconds - (time.monotonic() - start)
                if remaining <= 0 or nodes + args.node_cap > args.total_nodes:
                    failure = "declared screen budget exhausted"
                    break
                sample = engine.first(row["fen"], args.node_cap, min(95, remaining))
                nodes += sample["nodes"]
                board = chess.Board(row["fen"])
                move = chess.Move.from_uci(sample["move"]) if sample["move"] != "0000" else None
                legal = move is not None and move in board.legal_moves
                provenance = (sample["first_depth"] >= 1 and sample["root_kind"] == 3
                              and sample["source"] == 0 and sample["score_valid"])
                record = dict(kind="position", index=index, **row, **sample,
                              legal=legal, provenance_valid=provenance)
                output.write(json.dumps(record, separators=(",", ":")) + "\n")
                output.flush()
                results.append(record)
                if not legal or not provenance:
                    failure = ("probe node cap before first completed Full root" if move is None and
                               sample["nodes"] >= args.node_cap else "missing/illegal/unanalysed result")
                    print(json.dumps(dict(counterexample=failure, **record)), flush=True)
                    break
                if (index + 1) % 500 == 0:
                    print(f"completed={index + 1} nodes={nodes}", flush=True)
            summary = dict(kind="summary", completed=len(results), planned=len(rows),
                           failure=failure, nodes=nodes,
                           host_wall_s=round(time.monotonic() - start, 3),
                           first_nodes=quantiles([r["first_nodes"] for r in results if r["legal"]]),
                           host_us=quantiles([r["host_us"] for r in results if r["legal"]]))
            output.write(json.dumps(summary) + "\n")
            print(json.dumps(summary), flush=True)
    finally:
        engine.close()
    return 2 if failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
