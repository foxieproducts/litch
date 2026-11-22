#!/usr/bin/env python3
"""Freeze 64 rated human positions from a bounded official Maia CSV prefix.

Requires tools/elo/requirements.txt. Network reads retain an ignored raw prefix;
--replay-prefix plus --source-manifest regenerates the fixture without network.
The fixture is a move-quality comparator, not a measured playing Elo.
"""
import argparse
import bz2
import codecs
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import signal
import time
import urllib.request
import urllib.error

import chess
import chess.pgn


SOURCE_URL = "https://csslab.cs.toronto.edu/data/chess/kdd/maia-chess-testing-set.csv.bz2"
MAX_BYTES = 16 * 1024 * 1024
MAX_SECONDS = 120
CHUNK_BYTES = 65536
POSITION_COUNT = 64
MAX_GAME_EXPORTS = 96
SELECTION = {
    "version": 2,
    "count": POSITION_COUNT,
    "ratings_inclusive": [1000, 1199],
    "types": ["Blitz", "Rapid", "Classical"],
    "move_ply": "20 + int(sha256(game_id UTF-8).hexdigest(), 16) % 20; CSV zero-based move_ply",
    "minimum_both_clocks_seconds": 30,
    "missing_csv_clocks": "recover only from official same-game PGN",
    "recovered_clocks": "min(active previous-turn clock, active after-move clock) and opponent last clock",
    "pgn_crosschecks": ["FEN", "actual move", "both ratings", "time control"],
    "unavailable_game": "record failure, skip in source order; stop on HTTP 429",
    "order": "first qualifying row per distinct game, in source order",
    "validity": "valid nonterminal standard FEN and legal recorded human move",
    "quality_selection": "No engine evaluation, result, or move-quality fields used",
}


class DeadlineExceeded(RuntimeError):
    """Do not confuse the whole-run deadline with one unavailable game."""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded_json(value):
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode()


def csv_lines(source, raw_output, receipt):
    """Decompress bounded chunks; a retained prefix need not end a bz2 stream."""
    decompressor = bz2.BZ2Decompressor()
    decoder = codecs.getincrementaldecoder("utf-8")()
    pending = ""
    hasher = hashlib.sha256()
    started = time.monotonic()
    while receipt["bytes"] < MAX_BYTES:
        if time.monotonic() - started >= MAX_SECONDS:
            raise TimeoutError("source read exceeded 120 seconds")
        chunk = source.read(min(CHUNK_BYTES, MAX_BYTES - receipt["bytes"]))
        if not chunk:
            break
        receipt["bytes"] += len(chunk)
        hasher.update(chunk)
        receipt["sha256"] = hasher.hexdigest()
        if raw_output is not None:
            raw_output.write(chunk)
            raw_output.flush()
        compressed = chunk
        while True:
            data = decompressor.decompress(compressed, max_length=1024 * 1024)
            compressed = b""
            pending += decoder.decode(data)
            lines = pending.split("\n")
            pending = lines.pop()
            for line in lines:
                yield line + "\n"
            if len(pending) > 65536:
                raise ValueError("CSV line exceeds 64 KiB")
            if decompressor.eof or decompressor.needs_input:
                break
        if decompressor.eof:
            if pending:
                yield pending
            break


def recover_clocks(row, exports, directory, offline):
    if len(exports) >= MAX_GAME_EXPORTS:
        raise ValueError("game export bound reached before 64 qualifying positions")
    game = row["game_id"]
    if not game.isascii() or not game.isalnum():
        raise ValueError("invalid source game identifier")
    url = f"https://lichess.org/game/export/{game}?clocks=true&evals=false&opening=false"
    path = directory / f"{game}.pgn"
    receipt = {"game_id": game, "url": url}
    exports.append(receipt)
    try:
        if offline:
            raw = path.read_bytes()
        else:
            if len(exports) > 1:
                time.sleep(1)
            with urllib.request.urlopen(urllib.request.Request(
                    url, headers={"Accept": "application/x-chess-pgn"}), timeout=10) as response:
                raw = response.read(256 * 1024 + 1)
            if len(raw) > 256 * 1024:
                raise ValueError("game export exceeds 256 KiB")
            with path.open("xb") as output:
                output.write(raw)
        receipt.update(bytes=len(raw), sha256=digest(raw))
        game_record = chess.pgn.read_game(io.StringIO(raw.decode()))
        if game_record is None or game_record.errors:
            raise ValueError("invalid PGN")
        ply = int(row["move_ply"])
        nodes = list(game_record.mainline())
        if len(nodes) <= ply:
            raise ValueError("PGN missing selected ply")
        node = nodes[ply]
        if (node.parent.board().fen() != chess.Board(row["board"]).fen() or
                node.move.uci() != row["move"] or
                int(game_record.headers["WhiteElo"]) != int(row["white_elo"]) or
                int(game_record.headers["BlackElo"]) != int(row["black_elo"]) or
                game_record.headers["TimeControl"] != row["time_control"]):
            raise ValueError("PGN disagrees with selected CSV position")
        before, after, opponent = node.parent.parent.clock(), node.clock(), node.parent.clock()
        if any(value is None or not math.isfinite(value) for value in (before, after, opponent)):
            raise ValueError("PGN lacks required clocks")
        receipt.update(status="verified", active_clock_before=before,
                       active_clock_after=after, opponent_clock=opponent)
        return min(before, after), opponent, digest(raw)
    except urllib.error.HTTPError as error:
        receipt.update(status="failed", error=f"HTTP {error.code}")
        if error.code == 429:
            receipt["retry_after"] = error.headers.get("Retry-After")
            raise RuntimeError("Lichess rate limited; stop without retrying") from error
    except (OSError, ValueError, KeyError) as error:
        receipt.update(status="failed", error=str(error))
    return None


def select_positions(lines, exports, directory, offline):
    positions = []
    games = set()
    for row_number, row in enumerate(csv.DictReader(lines), 1):
        game = row["game_id"]
        if not game or game in games or row["type"] not in SELECTION["types"]:
            continue
        white, black = int(row["white_elo"]), int(row["black_elo"])
        if not (1000 <= white <= 1199 and 1000 <= black <= 1199):
            continue
        ply = int(row["move_ply"])
        if ply != 20 + int(digest(game.encode()), 16) % 20:
            continue
        # Missing clocks cannot establish the pre-registered time-trouble filter.
        clock = float(row["clock"] or "nan")
        opponent_clock = float(row["opp_clock"] or "nan")
        clock_source = "csv"
        pgn_sha256 = None
        if not all(math.isfinite(value) for value in (clock, opponent_clock)):
            recovered = recover_clocks(row, exports, directory, offline)
            if recovered is None:
                continue
            clock, opponent_clock, pgn_sha256 = recovered
            clock_source = "official same-game PGN; conservative before/after minimum"
        if not all(math.isfinite(value) and value >= 30 for value in (clock, opponent_clock)):
            continue
        board = chess.Board(row["board"])
        move = chess.Move.from_uci(row["move"])
        if not board.is_valid() or board.is_game_over() or move not in board.legal_moves:
            continue
        positions.append({
            "id": f"maia_{game}_ply{ply}",
            "fen": board.fen(),
            "human_move": move.uci(),
            "game_id": game,
            "source_row": row_number,
            "white_rating": white,
            "black_rating": black,
            "time_control_type": row["type"],
            "time_control": row["time_control"],
            "move_ply": ply,
            "active_clock_seconds": clock,
            "opponent_clock_seconds": opponent_clock,
            "clock_source": clock_source,
            "source_pgn_sha256": pgn_sha256,
        })
        games.add(game)
        if len(positions) == POSITION_COUNT:
            return positions
    raise ValueError(f"bounded prefix yielded {len(positions)}/{POSITION_COUNT} positions")


def timeout_handler(signum, frame):
    raise DeadlineExceeded("extraction exceeded 120 seconds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--raw-prefix", type=Path, help="New ignored file for bounded network bytes")
    source.add_argument("--source-prefix", type=Path, help="Existing CSV prefix; recover clocks online")
    source.add_argument("--replay-prefix", type=Path, help="Existing saved compressed prefix")
    parser.add_argument("--source-manifest", type=Path, help="Required with either prefix input")
    parser.add_argument("--game-exports", type=Path, required=True, help="New PGN directory, or saved directory for offline replay")
    parser.add_argument("--output", type=Path, required=True, help="New positions JSON file")
    parser.add_argument("--manifest", type=Path, required=True, help="New ignored evidence JSON file")
    args = parser.parse_args()
    input_prefix = args.replay_prefix or args.source_prefix
    if bool(input_prefix) != bool(args.source_manifest):
        parser.error("prefix inputs and --source-manifest must be supplied together")
    destinations = [args.output, args.manifest] + ([args.raw_prefix] if args.raw_prefix else [])
    if len({path.resolve() for path in destinations}) != len(destinations):
        parser.error("output paths must be distinct")
    for path in destinations:
        if path.exists():
            parser.error(f"refusing to overwrite {path}")
    if not hasattr(signal, "setitimer"):
        parser.error("a POSIX timer is required for the strict network/time bound")
    if not args.replay_prefix:
        args.game_exports.mkdir(parents=True, exist_ok=False)
    receipt = {"bytes": 0, "sha256": digest(b"")}
    manifest = {"source": SOURCE_URL, "selection": SELECTION, "prefix": receipt,
                "game_exports": [], "max_game_exports": MAX_GAME_EXPORTS,
                "max_source_bytes": MAX_BYTES, "max_seconds": MAX_SECONDS,
                "limitations": ["source-order prefix sample; not a random population sample",
                                "1000..1199 ratings are Lichess ratings, not FIDE Elo",
                                "move-quality proxy only; no measured board or human playing Elo"]}
    old_handler = signal.signal(signal.SIGALRM, timeout_handler)
    signal.setitimer(signal.ITIMER_REAL, MAX_SECONDS)
    try:
        if input_prefix:
            prior = json.loads(args.source_manifest.read_text())
            if prior["source"] != SOURCE_URL or (args.replay_prefix and prior["selection"] != SELECTION):
                raise ValueError("prefix source or replay selection differs")
            if input_prefix.stat().st_size > MAX_BYTES:
                raise ValueError("replay prefix exceeds byte bound")
            if digest(input_prefix.read_bytes()) != prior["prefix"]["sha256"]:
                raise ValueError("replay prefix digest differs")
            manifest["prefix"] = prior["prefix"]
            manifest["prefix_read"] = receipt
            manifest["source_headers"] = prior["source_headers"]
            if args.replay_prefix:
                for export in prior["game_exports"]:
                    if "sha256" in export and digest((args.game_exports / f"{export['game_id']}.pgn").read_bytes()) != export["sha256"]:
                        raise ValueError("replay PGN digest differs")
            with input_prefix.open("rb") as stream:
                positions = select_positions(csv_lines(stream, None, receipt),
                    manifest["game_exports"], args.game_exports, bool(args.replay_prefix))
        else:
            request = urllib.request.Request(SOURCE_URL, headers={"Range": f"bytes=0-{MAX_BYTES - 1}"})
            with urllib.request.urlopen(request, timeout=20) as stream, args.raw_prefix.open("xb") as raw:
                manifest["source_headers"] = {key: stream.headers.get(key) for key in
                    ["ETag", "Last-Modified", "Content-Length", "Content-Range", "Content-Type"]}
                positions = select_positions(csv_lines(stream, raw, receipt),
                    manifest["game_exports"], args.game_exports, False)
        fixture = encoded_json(positions)
        manifest.update(status="complete", position_count=len(positions), fixture_sha256=digest(fixture))
        with args.output.open("xb") as output:
            output.write(fixture)
    except Exception as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old_handler)
        with args.manifest.open("xb") as output:
            output.write(encoded_json(manifest))
    print(json.dumps({"positions": len(positions), "prefix_bytes": receipt["bytes"],
                      "fixture_sha256": manifest["fixture_sha256"]}))


if __name__ == "__main__":
    main()
