#!/usr/bin/env python3
"""Serial, balanced rounds: one color-swapped pair per wait, then next opening."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import chess.pgn
from run_match import pair_interval, elo_from_score, score_for_a

WAITS = (1000, 3000, 10000, 30000, 90000)

def summarize(directory, rounds):
    summary = {}
    for wait in WAITS:
        pairs = []
        for index in range(rounds):
            path = directory / f"round_{index+1:03d}_{wait}ms.pgn"
            if not (path.with_suffix(".done.json")).exists(): continue
            with path.open() as stream:
                games = [chess.pgn.read_game(stream), chess.pgn.read_game(stream)]
            if any(game is None or game.headers["Result"] == "*" or
                   game.headers.get("Termination", "").startswith("engine forfeit") for game in games):
                raise ValueError("invalid completed match evidence")
            scores = [score_for_a(game.headers["Result"], game.headers["White"] == "A") for game in games]
            pairs.append(sum(scores)/2)
        if not pairs: continue
        mean = sum(pairs)/len(pairs); low, high = pair_interval(pairs)
        summary[str(wait)] = dict(pairs=len(pairs), score=mean,
            elo_difference=None if mean in (0,1) else elo_from_score(mean),
            score_interval95=[low,high],
            note="host/internal Stockfish anchor; fixed-sample interval only; no finite estimate for all wins/losses")
    return summary

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine",required=True)
    parser.add_argument("--stockfish",default="/opt/homebrew/bin/stockfish")
    parser.add_argument("--stockfish-elo",type=int,default=1320)
    parser.add_argument("--stockfish-movetime-ms",type=int,default=1000)
    parser.add_argument("--openings",required=True)
    parser.add_argument("--rounds",type=int,default=1)
    parser.add_argument("--output",required=True)
    parser.add_argument("--max-plies",type=int,default=400)
    parser.add_argument("--pause-file", help="Shared move-boundary pause request file passed to each match")
    args=parser.parse_args()
    with open(args.openings) as stream: openings=json.load(stream)
    if args.rounds<1 or args.rounds>len(openings): parser.error("provide a distinct opening per requested round")
    directory=Path(args.output);directory.mkdir(parents=True,exist_ok=True)
    plan=dict(engine=args.engine,stockfish=args.stockfish,stockfish_elo=args.stockfish_elo,
              stockfish_movetime_ms=args.stockfish_movetime_ms,openings=openings,
              max_plies=args.max_plies,waits=WAITS)
    plan_path=directory/"plan.json"
    if plan_path.exists():
        if json.loads(plan_path.read_text())!=json.loads(json.dumps(plan)):
            parser.error("resume plan differs; use a new output directory")
    else: plan_path.write_text(json.dumps(plan,indent=2)+"\n")
    for index in range(args.rounds):
        opening_path=directory/f"opening_{index+1:03d}.json"
        opening_path.write_text(json.dumps([openings[index]])+"\n")
        # Rotate condition order so later conditions do not always encounter
        # warmer hardware. Each round still completes every wait condition.
        ordered=WAITS[index%len(WAITS):]+WAITS[:index%len(WAITS)]
        for wait in ordered:
            pgn=directory/f"round_{index+1:03d}_{wait}ms.pgn"
            done=pgn.with_suffix(".done.json")
            if done.exists(): continue
            if pgn.exists(): parser.error(f"partial evidence at {pgn}; inspect before resuming, never overwrite")
            command=[sys.executable,str(Path(__file__).with_name("run_match.py")),
                     "--engine-a",args.engine,"--uci-a","ContinuousHint=true",
                     "--a-movetime-ms",str(wait),"--engine-b",args.stockfish,
                     "--uci-b","Threads=1","--uci-b","Hash=64",
                     "--uci-b","UCI_LimitStrength=true","--uci-b",f"UCI_Elo={args.stockfish_elo}",
                     "--b-movetime-ms",str(args.stockfish_movetime_ms),
                     "--openings",str(opening_path),"--games","2","--max-plies",str(args.max_plies),"--pgn",str(pgn)]
            if args.pause_file: command.extend(["--pause-file", str(Path(args.pause_file).resolve())])
            with pgn.with_suffix(".log").open("x") as log:
                outcome=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
            if outcome.returncode: raise RuntimeError(f"match failed; inspect {pgn.with_suffix('.log')}")
            done.write_text(json.dumps(dict(command=command,returncode=0))+"\n")
        summary=summarize(directory,index+1)
        (directory/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
        print(json.dumps(dict(completed_round=index+1,summary=summary)),flush=True)
    return 0

if __name__ == "__main__": raise SystemExit(main())
