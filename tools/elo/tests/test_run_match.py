import argparse
import base64
import io
import json
import chess.pgn
import importlib.util
import sys
from pathlib import Path
import unittest
from unittest import mock
import tempfile
import chess
import chess.engine

sys.path.insert(0, str(Path(__file__).parents[1]))
spec = importlib.util.spec_from_file_location("run_match", Path(__file__).parents[1]/"run_match.py")
match = importlib.util.module_from_spec(spec)
spec.loader.exec_module(match)

class Engine:
    def __init__(self, invalid=False): self.invalid=invalid; self.limits=[]
    def play(self, board, limit, **kwargs):
        self.limits.append(limit)
        return chess.engine.PlayResult(chess.Move.null() if self.invalid else next(iter(board.legal_moves)), None)

class MatchTest(unittest.TestCase):
    def play_selected_move(self, uci, fen, history, a_white, max_plies):
        board = chess.Board(fen) if fen else chess.Board()
        for move in history: board.push_uci(move)
        a, b = mock.Mock(), mock.Mock()
        playing, idle = (a, b) if board.turn == a_white else (b, a)
        playing.play.return_value = chess.engine.PlayResult(chess.Move.from_uci(uci), None)
        result, game = match.play_one_game(a, b,
            chess.engine.Limit(nodes=1), chess.engine.Limit(nodes=2),
            fen, history, a_white, max_plies)
        playing.play.assert_called_once()
        idle.play.assert_not_called()
        return result, game

    def test_prospective_repetition_does_not_skip_selected_escape(self):
        history = "e2e4 e7e5 g1f3 b8c6 f3g1 c6b8 g1f3 b8c6 f3g1".split()
        board = chess.Board()
        for uci in history: board.push_uci(uci)
        self.assertTrue(board.can_claim_threefold_repetition())
        self.assertFalse(board.is_repetition(3))
        for a_white in (False, True):
            with self.subTest(a_white=a_white):
                result, game = self.play_selected_move("c6d4", "", history, a_white, 1)
                self.assertEqual(result, "*")
                self.assertEqual(game.end().move.uci(), "c6d4")
                self.assertFalse(game.end().board().can_claim_threefold_repetition())

    def test_actual_repetition_requires_the_selected_repeating_move(self):
        history = "e2e4 e7e5 g1f3 b8c6 f3g1 c6b8 g1f3 b8c6 f3g1".split()
        for a_white in (False, True):
            for max_plies in (1, 3):
                with self.subTest(a_white=a_white, max_plies=max_plies):
                    result, game = self.play_selected_move("c6b8", "", history, a_white, max_plies)
                    self.assertEqual(result, "1/2-1/2")
                    self.assertTrue(game.end().board().is_repetition(3))
                    self.assertEqual(game.end().move.uci(), "c6b8")

    def test_prospective_fifty_moves_allows_selected_pawn_reset(self):
        fen = "7k/8/8/8/8/8/P7/K7 w - - 99 51"
        board = chess.Board(fen)
        self.assertTrue(board.can_claim_fifty_moves())
        self.assertFalse(board.is_fifty_moves())
        for a_white in (False, True):
            with self.subTest(a_white=a_white):
                result, game = self.play_selected_move("a2a3", fen, [], a_white, 1)
                self.assertEqual(result, "*")
                self.assertEqual(game.end().board().halfmove_clock, 0)
                self.assertEqual(game.end().move.uci(), "a2a3")

    def test_actual_fifty_moves_follows_selected_quiet_move(self):
        fen = "7k/8/8/8/8/8/R7/K7 w - - 99 51"
        for a_white in (False, True):
            for max_plies in (1, 3):
                with self.subTest(a_white=a_white, max_plies=max_plies):
                    result, game = self.play_selected_move("a2b2", fen, [], a_white, max_plies)
                    self.assertEqual(result, "1/2-1/2")
                    self.assertEqual(game.end().board().halfmove_clock, 100)
                    self.assertEqual(game.end().move.uci(), "a2b2")

    def test_mate_on_hundredth_quiet_ply_wins(self):
        fen = "7k/8/5KQ1/8/8/8/8/8 w - - 99 51"
        self.assertTrue(chess.Board(fen).can_claim_fifty_moves())
        for a_white in (False, True):
            for max_plies in (1, 3):
                with self.subTest(a_white=a_white, max_plies=max_plies):
                    result, game = self.play_selected_move("g6g7", fen, [], a_white, max_plies)
                    self.assertEqual(result, "1-0")
                    self.assertTrue(game.end().board().is_checkmate())
                    self.assertEqual(game.end().board().halfmove_clock, 100)

    def test_current_fifty_move_draw_does_not_query_either_engine(self):
        a, b = mock.Mock(), mock.Mock()
        result, game = match.play_one_game(a, b,
            chess.engine.Limit(nodes=1), chess.engine.Limit(nodes=2),
            "7k/8/8/8/8/8/R7/K7 w - - 100 51", [], True, 3)
        self.assertEqual(result, "1/2-1/2")
        a.play.assert_not_called()
        b.play.assert_not_called()

    def test_pause_ack_follows_both_ready_barriers_and_preserves_game(self):
        limits = [chess.engine.Limit(time=1), chess.engine.Limit(time=3)]
        class ScriptedEngine(Engine):
            def play(self, board, limit, **kwargs):
                self.limits.append(limit)
                move = ("f2f3", "e7e5", "g2g4", "d8h4")[board.ply()]
                return chess.engine.PlayResult(chess.Move.from_uci(move), None)
        baseline_result, baseline = match.play_one_game(
            ScriptedEngine(), ScriptedEngine(), *limits, "", [], True, 4)
        with tempfile.TemporaryDirectory() as directory:
            request = Path(directory) / "pause"
            ack = Path(str(request) + ".paused")
            events = []
            now = [10.0]
            expected_fen = []
            class PausingEngine(ScriptedEngine):
                def __init__(self, name): super().__init__(); self.name = name
                def play(self, board, limit, **kwargs):
                    self_outer.assertFalse(ack.exists())
                    result = super().play(board, limit, **kwargs)
                    if board.ply() == 0:
                        request.touch()
                        next_board = board.copy(); next_board.push(result.move)
                        expected_fen.append(next_board.fen())
                    return result
                def ping(self):
                    self_outer.assertFalse(ack.exists())
                    events.append(self.name)
            self_outer = self
            a, b = PausingEngine("a"), PausingEngine("b")
            def wait(seconds):
                self.assertEqual(events, ["a", "b"])
                self.assertEqual(seconds, .25)
                payload = json.loads(ack.read_text())
                self.assertEqual(payload["ply"], 1)
                self.assertEqual(payload["fen"], expected_fen[0])
                # No second search has started while lane ownership is yielded.
                self.assertEqual(a.limits, [limits[0]])
                self.assertEqual(b.limits, [])
                now[0] += 7.5
                request.unlink()
            with mock.patch.object(match.time, "sleep", side_effect=wait), \
                 mock.patch.object(match.time, "monotonic", side_effect=lambda: now[0]):
                result, game = match.play_one_game(a, b, *limits, "", [], True, 4, request)
            self.assertFalse(ack.exists())
            self.assertEqual(result, "0-1")
            self.assertEqual(result, baseline_result)
            self.assertEqual(list(game.mainline_moves()), list(baseline.mainline_moves()))
            self.assertEqual(game.headers["Termination"], baseline.headers["Termination"])
            self.assertEqual(game.headers["InterMovePauseSeconds"], "7.500000")
            self.assertEqual(a.limits, [limits[0]] * 2)
            self.assertEqual(b.limits, [limits[1]] * 2)
            for node in game.mainline():
                decoded = json.loads(base64.urlsafe_b64decode(node.comment.split()[1]))
                self.assertEqual(decoded["host_elapsed_s"], 0.0)
            self.assertNotIn("InterMovePauseSeconds", baseline.headers)

    def test_stale_ack_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            request = Path(directory) / "pause"; request.touch()
            ack = Path(str(request) + ".paused"); ack.write_text("prior owner")
            a, b = mock.Mock(), mock.Mock()
            with self.assertRaises(FileExistsError):
                match.pause_at_move_boundary(request, a, b, chess.Board())
            self.assertEqual(ack.read_text(), "prior owner")
            a.ping.assert_called_once(); b.ping.assert_called_once()

    def test_pgn_roundtrip_preserves_nested_move_diagnostics(self):
        diagnostic = {"time": 1.0, "nodes": 1234,
                      "string": '{"hint_sample":true,"move":"e2e4"}'}
        class ReportingEngine(Engine):
            def play(self, board, limit, **kwargs):
                result = super().play(board, limit, **kwargs)
                result.info = diagnostic.copy()
                return result
        with mock.patch.object(match.time, "monotonic", side_effect=[10.0, 10.125]):
            _, game = match.play_one_game(ReportingEngine(), Engine(),
                chess.engine.Limit(time=1), chess.engine.Limit(time=1), "", [], True, 1)
        restored = chess.pgn.read_game(io.StringIO(str(game)))
        comment = next(iter(restored.mainline())).comment
        self.assertTrue(comment.startswith("litch_info_b64 "), comment)
        decoded = json.loads(base64.urlsafe_b64decode(comment.split()[1]))
        self.assertEqual(decoded, dict(diagnostic, host_elapsed_s=.125))

    def test_forfeit_is_actual_color_for_both_assignments(self):
        for a_white in (False,True):
            for white_turn in (False,True):
                a,b=Engine(True),Engine(True)
                fen=chess.STARTING_FEN.replace(" w "," b ") if not white_turn else ""
                result,_=match.play_one_game(a,b,chess.engine.Limit(nodes=1),chess.engine.Limit(nodes=2),fen,[],a_white,20)
                self.assertEqual(result,"0-1" if white_turn else "1-0")
    def test_ply_limit_is_not_draw(self):
        result,game=match.play_one_game(Engine(),Engine(),chess.engine.Limit(nodes=1),chess.engine.Limit(nodes=2),"",[],True,1)
        self.assertEqual(result,"*")
        self.assertIn("unfinished",game.headers["Termination"])
    def test_time_has_no_default_node_cap(self):
        args=argparse.Namespace(nodes=None,depth=None,movetime_ms=None,a_nodes=None,a_depth=None,a_movetime_ms=1000)
        limit=match.make_limit(args,"a")
        self.assertEqual(limit.time,1)
        self.assertIsNone(limit.nodes)
    def test_separate_limits_and_pair_interval(self):
        a,b=Engine(),Engine()
        la,lb=chess.engine.Limit(nodes=1),chess.engine.Limit(nodes=2)
        validators = [mock.Mock(), mock.Mock()]
        validators[0].validate.return_value = {"side": "a"}
        validators[1].validate.return_value = {"side": "b"}
        _, game = match.play_one_game(a,b,la,lb,"",[],False,2,
                                     work_budget=validators[0], work_budget_b=validators[1])
        self.assertEqual(a.limits,[la]);self.assertEqual(b.limits,[lb])
        for validator in validators: validator.validate.assert_called_once()
        diagnostics = [json.loads(base64.urlsafe_b64decode(n.comment.split()[1])) for n in game.mainline()]
        self.assertEqual([d["s3_work"]["side"] for d in diagnostics], ["b", "a"])
        self.assertEqual(match.pair_interval([.5]),(0.,1.))
        self.assertEqual(match.elo_from_score(.5),0.)

if __name__ == "__main__": unittest.main()
