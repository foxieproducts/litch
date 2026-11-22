import base64
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import chess
import chess.engine
import chess.pgn

sys.path.insert(0, str(Path(__file__).parents[1]))
import run_match as match
import s3_work_budget as budget


class WorkBudgetTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / 'uci'
        self.binary.write_bytes(b'pinned test executable')
        self.receipt_path = self.root / 'receipt.json'
        runs = []
        for case in range(1, 7):
            for level in range(1, 12):
                runs.append(dict(case=case, level=level, result='DONE', boot_continuity=True,
                    final=dict(cancel=False, final_nodes=1000 * level + case,
                        final_nodes_valid=True, profiles=dict(requested=dict(level=level,
                            node_ceiling=budget.NODE_CAPS[level], depth_ceiling=budget.DEPTH_CAPS[level],
                            time_ceiling_ms=budget.TIME_CAPS[level])))))
        self.receipt = dict(result='PASS_FINITE_COMPUTE_CAPTURE_GAME_PRESERVED', runs=runs,
                            target='test', expected_image='test-image', candidate={})
        self.receipt_path.write_text(json.dumps(self.receipt))
        self.model_path = self.root / 'model.json'
        budget.write_model(self.receipt_path, self.binary, self.model_path)
        self.work = budget.S3WorkBudget(self.model_path, 11, self.binary)

    def diagnostic(self, profile=11, nodes=None):
        cap = min(self.work.nodes, budget.NODE_CAPS[profile])
        return dict(cpu_sample=True, cpu_policy=True, selected_level=11, profile_level=profile,
                    max_nodes=cap, max_depth=budget.DEPTH_CAPS[profile], max_think_ms=60000,
                    first_root_node_reserve=min(16384, cap), policy_rejected=False, seed=5,
                    completed_root_depth=2, preliminary_only=False, novice_substitution=False, searched_move='e2e4',
                    decision_source=0, root_kind=3,
                    workspace_bytes=12232, reversible_halfmoves=65535, opening_history=False, opening_plies=0,
                    nodes=cap if nodes is None else nodes)

    def test_complete_panel_and_fixed_minimum_rule(self):
        self.assertEqual(budget.measured_caps(self.receipt), {str(l): 1000*l+1 for l in range(1, 12)})
        for mutation in ('missing', 'duplicate', 'canceled', 'wrong-profile', 'bool-nodes'):
            receipt = copy.deepcopy(self.receipt)
            if mutation == 'missing': receipt['runs'].pop()
            if mutation == 'duplicate': receipt['runs'][-1] = receipt['runs'][0]
            if mutation == 'canceled': receipt['runs'][0]['final']['cancel'] = True
            if mutation == 'wrong-profile': receipt['runs'][0]['final']['profiles']['requested']['time_ceiling_ms'] = 1
            if mutation == 'bool-nodes': receipt['runs'][0]['final']['final_nodes'] = True
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): budget.measured_caps(receipt)

        revised = copy.deepcopy(self.receipt)
        revised['cpu_policy'] = budget.STATIC_NOVICE_POLICY
        revised['result'] = budget.COMPOSITE_RESULT
        for job in revised['runs']:
            level, final = job['level'], job['final']
            final['profiles']['requested']['node_ceiling'] = budget.NOVICE_NODE_CAPS[level]
            final['profiles']['requested']['depth_ceiling'] = budget.NOVICE_DEPTH_CAPS[level]
            final['final_nodes'] = min(final['final_nodes'], budget.NOVICE_NODE_CAPS[level])
            final['stop'] = 'exhausted'
        caps = budget.measured_caps(revised)
        self.assertEqual((caps['1'], caps['2']), (256, 4096))
        # Deadline-limited work still reduces the host cap; an early completed
        # static horizon does not impose its smaller position-specific count.
        job = next(j for j in revised['runs'] if j['case'] == 2 and j['level'] == 2)
        job['final'].update(stop='deadline', final_nodes=3200)
        self.assertEqual(budget.measured_caps(revised)['2'], 3200)
        self.receipt_path.write_text(json.dumps(revised))
        path = self.root / 'static-model.json'
        budget.write_model(self.receipt_path, self.binary, path)
        work = budget.S3WorkBudget(path, 1, self.binary)
        sample = self.diagnostic(profile=1, nodes=100)
        sample.update(selected_level=1, max_nodes=256, max_depth=1, max_think_ms=900,
                      first_root_node_reserve=256, static_leaf_only=True,
                      completed_root_depth=1, preliminary_only=True, root_kind=2)
        work.validate({'string': json.dumps(sample)})
        sample['novice_substitution'] = True
        with self.assertRaises(budget.CalibrationInvalid):
            work.validate({'string': json.dumps(sample)})
        sample['novice_substitution'] = False
        sample.update(preliminary_only=False, root_kind=3)
        with self.assertRaises(budget.CalibrationInvalid):
            work.validate({'string': json.dumps(sample)})

    def test_model_receipt_and_executable_are_pinned(self):
        for target in (self.binary, self.receipt_path, self.model_path):
            original = target.read_bytes()
            if target == self.model_path:
                model = json.loads(original); model['node_caps']['11'] += 1
                target.write_text(json.dumps(model))
            else: target.write_bytes(original + b' ')
            with self.subTest(target=target), self.assertRaises(ValueError):
                budget.S3WorkBudget(self.model_path, 11, self.binary)
            target.write_bytes(original)

    def test_provenance_and_required_diagnostics_cannot_be_dropped(self):
        model = json.loads(self.model_path.read_text()); model['target'] = 'another-board'
        self.model_path.write_text(json.dumps(model))
        with self.assertRaises(ValueError): budget.S3WorkBudget(self.model_path, 11, self.binary)
        for key in ('seed', 'searched_move', 'novice_substitution', 'completed_root_depth', 'preliminary_only'):
            sample = self.diagnostic(); del sample[key]
            with self.subTest(key=key), self.assertRaises(budget.CalibrationInvalid):
                self.work.validate({'string': json.dumps(sample)})

    def test_early_exhaustion_is_valid_but_reserve_overrun_is_not(self):
        for nodes in (0, 171, self.work.nodes):
            self.assertEqual(self.work.validate({'string': json.dumps(self.diagnostic(nodes=nodes))})['observed_nodes'], nodes)
        with self.assertRaises(budget.CalibrationInvalid):
            self.work.validate({'string': json.dumps(self.diagnostic(nodes=self.work.nodes+1))})

    def test_diagnostic_policy_types_and_missing_data_fail_closed(self):
        for raw in (None, 'bad-json', '[]', '{}', 'null'):
            with self.subTest(raw=raw), self.assertRaises(budget.CalibrationInvalid):
                self.work.validate({'string': raw})
        for key, value in dict(cpu_policy=False, selected_level=10, max_depth=99,
                               max_think_ms=900, max_nodes=1800000, nodes=True,
                               profile_level=12, policy_rejected=True, first_root_node_reserve=1,
                               seed=-1, completed_root_depth=None, preliminary_only=None, novice_substitution=None, searched_move=None).items():
            sample = self.diagnostic(); sample[key] = value
            with self.subTest(key=key), self.assertRaises(budget.CalibrationInvalid):
                self.work.validate({'string': json.dumps(sample)})

    def test_opening_ramp_uses_history_and_selected_level_time(self):
        board = chess.Board()
        for profile, moves in ((3, []), (7, ['e2e4', 'e7e5']), (11, ['g1f3', 'b8c6'])):
            for move in moves: board.push_uci(move)
            sample = self.diagnostic(profile); sample['searched_move'] = next(iter(board.legal_moves)).uci()
            sample.update(reversible_halfmoves=board.halfmove_clock, opening_history=True,
                          opening_plies=len(board.move_stack))
            self.work.validate({'string': json.dumps(sample)}, board)
        with self.assertRaises(budget.CalibrationInvalid):
            self.work.validate({'string': json.dumps(self.diagnostic(3))}, board)
        sample = self.diagnostic(); sample['searched_move'] = next(iter(board.legal_moves)).uci()
        self.work.validate({'string': json.dumps(sample)}, chess.Board(board.fen()))

    def test_overrun_preserves_partial_pgn_and_is_never_a_forfeit(self):
        sample = self.diagnostic(profile=3, nodes=self.work.nodes+1)
        for calibrated_a in (True, False):
            a, b = mock.Mock(), mock.Mock()
            playing, idle = (a, b) if calibrated_a else (b, a)
            playing.play.return_value = chess.engine.PlayResult(chess.Move.from_uci('e2e4'), None,
                                                                 info={'string': json.dumps(sample)})
            result, game = match.play_one_game(a, b, chess.engine.Limit(nodes=self.work.nodes),
                chess.engine.Limit(nodes=self.work.nodes), '', [], calibrated_a, 10,
                work_budget=self.work if calibrated_a else None,
                work_budget_b=None if calibrated_a else self.work)
            self.assertEqual(result, '*'); self.assertEqual(len(list(game.mainline())), 0)
            self.assertTrue(game.headers['Termination'].startswith('calibration invalid:'))
            saved = chess.pgn.read_game(io.StringIO(str(game)))
            evidence = json.loads(base64.urlsafe_b64decode(saved.comment.split()[1]))
            self.assertEqual(evidence['diagnostic'], sample)
            idle.play.assert_not_called()

    def cli(self, *extra):
        return ['run_match.py', '--engine-a', str(self.binary), '--uci-a', 'LitchCpuPolicy=true',
                '--uci-a', 'Difficulty=11', '--a-s3-work', str(self.model_path),
                '--engine-b', 'stockfish', '--b-movetime-ms', '1000', '--games', '2',
                '--pgn', str(self.root / 'result.pgn'), *extra]

    def test_conflicting_limits_rejected_before_starting_engines(self):
        for flag in ('--nodes', '--depth', '--movetime-ms', '--a-nodes', '--a-depth', '--a-movetime-ms'):
            with self.subTest(flag=flag), mock.patch.object(sys, 'argv', self.cli(flag, '1')), \
                 mock.patch.object(chess.engine.SimpleEngine, 'popen_uci') as start, \
                 mock.patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit) as exit:
                match.main()
            self.assertEqual(exit.exception.code, 2); start.assert_not_called()

    def test_cli_aborts_no_score_and_preserves_b_budget(self):
        engines = [mock.MagicMock(), mock.MagicMock()]
        for engine in engines: engine.__enter__.return_value = engine; engine.id = {}
        game = chess.pgn.Game(); game.headers.update(Result='*', Termination='calibration invalid: test')
        with mock.patch.object(sys, 'argv', self.cli()), \
             mock.patch.object(chess.engine.SimpleEngine, 'popen_uci', side_effect=engines), \
             mock.patch.object(match, 'play_one_game', return_value=('*', game)) as play, \
             mock.patch('sys.stderr', io.StringIO()) as err:
            self.assertEqual(match.main(), 2)
        self.assertIn('NO SCORE', err.getvalue()); play.assert_called_once()
        self.assertEqual(play.call_args.args[2].nodes, self.work.nodes)
        self.assertIsNone(play.call_args.args[2].time)
        self.assertEqual(play.call_args.args[3].time, 1)
        self.assertIsNone(play.call_args.args[3].nodes)
        with (self.root / 'result.pgn').open() as stream:
            self.assertEqual(chess.pgn.read_game(stream).headers['Result'], '*')


if __name__ == '__main__': unittest.main()
