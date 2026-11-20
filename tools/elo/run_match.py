#!/usr/bin/env python3
"""Paired-opening UCI matches; host results are not physical-board ratings."""
import argparse
import base64
import json
import math
import os
import shlex
import sys
import time
from pathlib import Path
from datetime import datetime, timezone

import chess
import chess.engine
import chess.pgn
from s3_work_budget import CalibrationInvalid, S3WorkBudget


def elo_from_score(score):
    if score <= 0: return -math.inf
    if score >= 1: return math.inf
    return 400 * math.log10(score / (1 - score))


def pair_interval(scores):
    """Distribution-free 95% Hoeffding interval on independent paired scores.

    Conservative at small n, valid for fractional draws, keeps paired games
    together. Fixed openings limit interpretation to the supplied corpus.
    """
    if not scores: return (0.0, 1.0)
    mean = sum(scores) / len(scores)
    radius = math.sqrt(math.log(40) / (2 * len(scores)))
    return max(0.0, mean-radius), min(1.0, mean+radius)


def pause_at_move_boundary(pause_file, engine_a, engine_b, board):
    """Acknowledge only after both UCI ready barriers drain hint cleanup."""
    if not pause_file or not Path(pause_file).exists(): return 0.0
    request = Path(pause_file)
    acknowledgement = Path(str(request) + ".paused")
    started = time.monotonic()
    engine_a.ping()
    engine_b.ping()
    # Exclusive creation refuses stale acknowledgements or another match owner.
    with acknowledgement.open("x", encoding="utf-8") as stream:
        json.dump(dict(pid=os.getpid(), ply=board.ply(), fen=board.fen()), stream)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    try:
        while request.exists(): time.sleep(0.25)
    finally:
        acknowledgement.unlink()
    return time.monotonic() - started


def played_outcome(board):
    """Match adjudication uses played positions, never hypothetical next moves."""
    outcome = board.outcome(claim_draw=False)
    if outcome is not None: return outcome  # Checkmate precedes move-count draws.
    if board.is_fifty_moves():
        return chess.Outcome(chess.Termination.FIFTY_MOVES, None)
    if board.is_repetition(3):
        return chess.Outcome(chess.Termination.THREEFOLD_REPETITION, None)
    return None


def play_one_game(engine_a, engine_b, limit_a, limit_b, start_fen,
                  moves_uci, a_is_white, max_plies, pause_file=None, work_budget=None, work_budget_b=None):
    board = chess.Board(start_fen) if start_fen else chess.Board()
    for move in moves_uci: board.push_uci(move)
    opening_plies = len(board.move_stack)
    move_diagnostics = []
    pause_seconds = 0.0
    game_token = object()  # python-chess sends ucinewgame independently per game.
    forced_result = None
    termination = "normal"
    calibration_failure = None
    for _ in range(max_plies):
        if played_outcome(board) is not None: break
        pause_seconds += pause_at_move_boundary(pause_file, engine_a, engine_b, board)
        white_to_move = board.turn == chess.WHITE
        side_is_a = white_to_move == a_is_white
        side_engine = engine_a if side_is_a else engine_b
        try:
            move_started = time.monotonic()
            result = side_engine.play(board, limit_a if side_is_a else limit_b,
                                      game=game_token, info=chess.engine.INFO_ALL)
            host_elapsed = time.monotonic() - move_started
            validator = work_budget if side_is_a else work_budget_b
            work = validator.validate(result.info, board) if validator is not None else None
            if result.resigned:
                forced_result = "0-1" if white_to_move else "1-0"
                termination = "resignation"
                break
            if result.move is None or result.move not in board.legal_moves:
                raise chess.engine.EngineError("missing, null or illegal move")
            board.push(result.move)
            diagnostic = {key: result.info[key] for key in ("time", "nodes", "string") if key in result.info}
            diagnostic["host_elapsed_s"] = host_elapsed
            if work is not None: diagnostic["s3_work"] = work
            move_diagnostics.append(diagnostic)
        except CalibrationInvalid as exc:
            forced_result = "*"
            termination = "calibration invalid: " + str(exc)
            calibration_failure = exc.evidence
            break
        except (chess.engine.EngineError, chess.IllegalMoveError, ValueError) as exc:
            # PGN result always denotes White/Black, irrespective of A/B.
            forced_result = "0-1" if white_to_move else "1-0"
            termination = "engine forfeit: " + str(exc)
            print(termination, file=sys.stderr)
            break
    outcome = played_outcome(board)
    result = forced_result or (outcome.result() if outcome else "*")
    if result == "*" and calibration_failure is None: termination = "unfinished: ply limit"
    game = chess.pgn.Game.from_board(board)
    if calibration_failure is not None:
        payload = json.dumps(calibration_failure, sort_keys=True).encode("utf-8")
        game.comment = "litch_failure_b64 " + base64.urlsafe_b64encode(payload).decode("ascii")
    for node, diagnostic in zip(list(game.mainline())[opening_plies:], move_diagnostics):
        if diagnostic:
            # PGN exporters remove closing braces from comments, including
            # braces inside escaped JSON strings. Preserve exact diagnostics
            # through standard PGN export/read with a brace-free encoding.
            payload = json.dumps(diagnostic, sort_keys=True).encode("utf-8")
            node.comment = "litch_info_b64 " + base64.urlsafe_b64encode(payload).decode("ascii")
    game.headers.update(Date=datetime.now(timezone.utc).strftime("%Y.%m.%d"),
                        White="A" if a_is_white else "B",
                        Black="B" if a_is_white else "A", Result=result,
                        Termination=termination, DrawPolicy="played-threefold-or-fifty-moves")
    if pause_file is not None:
        game.headers["InterMovePauseSeconds"] = f"{pause_seconds:.6f}"
    return result, game


def parse_uci_options(items):
    opts = {}
    for item in items:
        if "=" not in item: raise ValueError(f"invalid UCI option {item!r}")
        key, value = [part.strip() for part in item.split("=", 1)]
        if not key: raise ValueError("empty UCI option")
        if value.lower() in ("true", "false"): value = value.lower() == "true"
        else:
            try: value = int(value)
            except ValueError: pass
        opts[key] = value
    return opts


def make_limit(args, side):
    def selected(name):
        value = getattr(args, side + "_" + name)
        return getattr(args, name) if value is None else value
    nodes, depth, ms = [selected(name) for name in ("nodes", "depth", "movetime_ms")]
    values = {}
    if nodes is not None and nodes > 0: values["nodes"] = nodes
    if depth is not None and depth > 0: values["depth"] = depth
    if ms is not None and ms > 0: values["time"] = ms / 1000
    if not values: values["nodes"] = 50000
    return chess.engine.Limit(**values)


def score_for_a(result, a_is_white):
    if result == "*": return None
    if result == "1/2-1/2": return .5
    return float((result == "1-0") == a_is_white)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for side in ("a", "b"):
        ap.add_argument(f"--engine-{side}", required=True)
        ap.add_argument(f"--uci-{side}", action="append", default=[])
        ap.add_argument(f"--{side}-s3-work", help=f"Pinned measured S3 work model for {side.upper()}")
        for flag in ("nodes", "depth", "movetime-ms"):
            ap.add_argument(f"--{side}-{flag}", type=int)
    for flag in ("nodes", "depth", "movetime-ms"): ap.add_argument(f"--{flag}", type=int)
    ap.add_argument("--games", type=int, default=20)
    ap.add_argument("--start-fen", default="")
    ap.add_argument("--moves", default="")
    ap.add_argument("--openings", help="JSON list of {fen,moves}; each used twice with colors reversed")
    ap.add_argument("--max-plies", type=int, default=400)
    ap.add_argument("--pgn", required=True, help="New file; flushed after each game")
    ap.add_argument("--pause-file", help="Pause between moves while this file exists; ready acknowledgement at FILE.paused")
    args = ap.parse_args()
    if args.games < 2 or args.games % 2: ap.error("--games must be positive and even")
    if args.max_plies <= 0: ap.error("--max-plies must be positive")
    openings = [{"fen": args.start_fen, "moves": args.moves}]
    if args.openings:
        with open(args.openings, encoding="utf-8") as stream: openings = json.load(stream)
    if not openings: ap.error("opening corpus is empty")
    if args.games // 2 > len(openings):
        print("warning: repeated openings; interval only describes this repeated corpus", file=sys.stderr)
    opts_a, opts_b = parse_uci_options(args.uci_a), parse_uci_options(args.uci_b)
    budgets = {}
    for side, opts in (("a", opts_a), ("b", opts_b)):
        path = getattr(args, side + "_s3_work")
        if not path: continue
        if any(getattr(args, name) is not None for name in
               ("nodes", "depth", "movetime_ms", side + "_nodes", side + "_depth", side + "_movetime_ms")):
            ap.error(f"--{side}-s3-work cannot be combined with shared or {side.upper()}-specific search limits")
        command = shlex.split(getattr(args, "engine_" + side))
        if len(command) != 1 or opts.get("LitchCpuPolicy") is not True:
            ap.error(f"--{side}-s3-work requires a direct Litch UCI executable and LitchCpuPolicy=true")
        try:
            budgets[side] = S3WorkBudget(path, opts.get("Difficulty"), command[0])
        except (KeyError, ValueError, OSError) as error:
            ap.error(str(error))
    limit_a = chess.engine.Limit(nodes=budgets["a"].nodes) if "a" in budgets else make_limit(args, "a")
    limit_b = chess.engine.Limit(nodes=budgets["b"].nodes) if "b" in budgets else make_limit(args, "b")
    os.makedirs(os.path.dirname(args.pgn) or ".", exist_ok=True)
    scores, pairs = [], []
    unfinished = forfeits = 0
    # Exclusive create avoids accidentally overwriting prior evidence.
    with open(args.pgn, "x", encoding="utf-8") as pgn, \
         chess.engine.SimpleEngine.popen_uci(shlex.split(args.engine_a), timeout=120) as engine_a, \
         chess.engine.SimpleEngine.popen_uci(shlex.split(args.engine_b), timeout=120) as engine_b:
        engine_a.configure(opts_a); engine_b.configure(opts_b)
        for index in range(args.games):
            opening = openings[(index // 2) % len(openings)]
            moves = opening.get("moves", [])
            if isinstance(moves, str): moves = moves.split()
            a_is_white = index % 2 == 0
            result, game = play_one_game(engine_a, engine_b, limit_a, limit_b,
                                        opening.get("fen", ""), moves, a_is_white, args.max_plies, args.pause_file, budgets.get("a"), budgets.get("b"))
            game.headers.update(Round=str(index+1), OpeningId=str((index//2) % len(openings)),
                                EngineA=engine_a.id.get("name", args.engine_a),
                                EngineB=engine_b.id.get("name", args.engine_b),
                                OptionsA=json.dumps(opts_a), OptionsB=json.dumps(opts_b),
                                LimitA=str(limit_a), LimitB=str(limit_b))
            for side, budget in budgets.items():
                game.headers["SearchWorkModel" + side.upper()] = "S3-calibrated fixed-node host model"
                game.headers["S3CalibrationSHA256" + side.upper()] = budget.sha256
                game.headers["S3NodeCap" + side.upper()] = str(budget.nodes)
            if "a" in budgets:  # Preserve existing A-only receipts.
                game.headers["SearchWorkModel"] = "S3-calibrated fixed-node host model"
                game.headers["S3CalibrationSHA256"] = budgets["a"].sha256
                game.headers["S3NodeCap"] = str(budgets["a"].nodes)
            print(game, file=pgn, end="\n\n"); pgn.flush(); os.fsync(pgn.fileno())
            if game.headers["Termination"].startswith("calibration invalid:"):
                print("NO SCORE: " + game.headers["Termination"], file=sys.stderr, flush=True)
                return 2
            score = score_for_a(result, a_is_white); scores.append(score)
            unfinished += score is None
            forfeits += game.headers["Termination"].startswith("engine forfeit")
            if index % 2 and None not in scores[-2:]: pairs.append(sum(scores[-2:]) / 2)
            print(f"game {index+1}/{args.games}: {result}; unfinished={unfinished} forfeits={forfeits}", flush=True)
    # Dropping truncated games creates selection bias. Retain all PGNs but do
    # not publish a rating estimate until the intended match is fully resolved.
    if unfinished or forfeits:
        print(f"NO ELO ESTIMATE: unfinished={unfinished}, engine_forfeits={forfeits}")
        return 1
    mean = sum(pairs) / len(pairs)
    low, high = pair_interval(pairs)
    if mean in (0.0, 1.0):
        print(f"NO FINITE ELO ESTIMATE: pairs={len(pairs)} A_score={mean:.4f}; score95%=[{low:.4f},{high:.4f}]")
        return 0
    print(f"pairs={len(pairs)} A_score={mean:.4f} EloDiff(A-B)={elo_from_score(mean):.1f} "
          f"conservative95%=[{elo_from_score(low):.1f},{elo_from_score(high):.1f}]")
    return 0

if __name__ == "__main__": raise SystemExit(main())
