"""First-result provenance and reproducible legal-play inputs, without a long campaign."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import chess

MODULE = Path(__file__).resolve().parents[1] / "hint_reliability.py"
spec = importlib.util.spec_from_file_location("hint_reliability", MODULE)
screen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screen)

REPORTED = "2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1"


class CorpusTest(unittest.TestCase):
    def test_seeded_deduplicated_positions_replay_legally(self):
        rows, metadata = screen.make_corpus(20261026, 100)
        repeated, repeated_metadata = screen.make_corpus(20261026, 100)
        self.assertEqual(rows, repeated)
        self.assertEqual(metadata["sha256"], repeated_metadata["sha256"])
        self.assertEqual(metadata["categories"], dict(opening=24, middlegame=50, endgame=26))
        identities = set()
        for row in rows:
            board = chess.Board()
            for text in row["moves"]:
                move = chess.Move.from_uci(text)
                self.assertIn(move, board.legal_moves)
                board.push(move)
            self.assertEqual(board.fen(), row["fen"])
            self.assertIsNone(board.outcome())
            identities.add(" ".join(board.fen(en_passant="legal").split()[:4]))
        self.assertEqual(len(identities), len(rows))


class FirstHintTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get("LITCH_UCI_PATH", "build/uci/litch_uci")).resolve()
        if not cls.binary.is_file():
            raise RuntimeError("Build litch_uci and set LITCH_UCI_PATH")

    def sample(self, fen, cap):
        engine = screen.HintEngine(self.binary)
        try:
            return engine.first(fen, cap, 15)
        finally:
            engine.close()

    def test_reported_fen_first_result_and_insufficient_budget(self):
        result = self.sample(REPORTED, 1000000)
        self.assertIn(chess.Move.from_uci(result["move"]), chess.Board(REPORTED).legal_moves)
        self.assertEqual((result["first_depth"], result["root_kind"], result["source"]), (1, 3, 0))
        self.assertTrue(result["score_valid"])
        self.assertEqual(result["nodes"], result["first_nodes"])
        self.assertEqual(result["completed_roots"], 1)
        self.assertEqual(result["time_cap_ms"], 90000)
        limited = self.sample(REPORTED, 1)
        self.assertEqual((limited["move"], limited["nodes"], limited["first_depth"]), ("0000", 1, 0))
        # Replay the actual first result through screen admission without a campaign.
        for invalid in ({}, {"first_depth": 0}, {"score_valid": False}, {"source": 2},
                        {"root_kind": 2}, {"move": "a1a8"}):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "screen.jsonl"
                with mock.patch.object(screen, "make_corpus", return_value=([{"fen": REPORTED}], {})), \
                        mock.patch.object(screen, "HintEngine") as engine, \
                        mock.patch.object(sys, "argv", ["hint_reliability.py", "--engine", str(self.binary),
                                                       "--output", str(output), "--count", "20"]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    engine.return_value.first.side_effect = [
                        {"move": "0000"}, {"move": "0000"}, {**result, **invalid}]
                    self.assertEqual(screen.main(), 2 if invalid else 0)
                    engine.return_value.close.assert_called_once()
                summary = json.loads(output.read_text().splitlines()[-1])
                self.assertEqual(summary["completed"], 1)
                self.assertEqual(bool(summary["failure"]), bool(invalid))

    def test_actual_terminal_positions_have_no_fabricated_move(self):
        for fen in ("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
                    "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"):
            self.assertEqual(self.sample(fen, 1000)["move"], "0000")

    def test_repeated_probe_does_not_time_a_replayed_cached_candidate(self):
        result = subprocess.run(
            [str(self.binary)], input=f"position fen {REPORTED}\nhintfirst 1000000\n"
            "hintfirst 1000000\nquit\n", text=True, capture_output=True, timeout=15, check=True)
        samples = [json.loads(line.removeprefix("info string "))
                   for line in result.stdout.splitlines() if line.startswith('info string {"hint_first"')]
        self.assertEqual(len(samples), 2)
        self.assertEqual(samples[0]["first_nodes"], samples[1]["first_nodes"])
        self.assertEqual(samples[0]["nodes"], samples[1]["nodes"])
        self.assertEqual(samples[1]["completed_roots"], 1)

    def test_invalid_probe_caps_are_bounded_and_do_not_crash(self):
        result = subprocess.run(
            [str(self.binary)], input="hintfirst 0\nhintfirst 16000001\nhintfirst x\nquit\n",
            text=True, capture_output=True, timeout=5, check=True)
        self.assertEqual(result.stdout.count("bestmove 0000"), 3)
        self.assertNotIn('"hint_first":true', result.stdout)


if __name__ == "__main__":
    unittest.main()
