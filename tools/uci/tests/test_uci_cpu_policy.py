"""Exercise the real UCI executable with the board's optional CPU policy."""

import json
import os
from pathlib import Path
import subprocess
import unittest

import chess


NODE_CAPS = (0, 256, 4096, 180000, 210000, 225000,
             300000, 360000, 540000, 810000, 1200000, 1800000)
DEPTH_CAPS = (0, 1, 2, 4, 6, 7, 8, 9, 11, 12, 14, 16)
THINK_MS_CAPS = (0, 900, 900, 630, 1400, 2800,
                 4900, 7000, 10500, 17500, 28000, 60000)
RAMPS = {
    1: (1, 1, 1, 1), 2: (2, 2, 2, 2), 3: (3, 3, 3, 3),
    4: (3, 3, 4, 4), 5: (3, 4, 5, 5), 6: (3, 4, 6, 6),
    7: (3, 5, 7, 7), 8: (3, 5, 8, 8), 9: (3, 6, 9, 9),
    10: (3, 6, 10, 10), 11: (3, 7, 11, 11),
}
OPENING = 'e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4'.split()
MATE_FENS = (
    '7k/5Q2/6K1/8/8/8/8/8 w - - 0 1',
    '8/8/8/8/8/6k1/5q2/7K b - - 0 1',
)


class CpuPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get('LITCH_UCI_PATH', 'build/uci/litch_uci'))
        if not cls.binary.is_file():
            raise RuntimeError('Build litch_uci and set LITCH_UCI_PATH to its executable')

    def run_uci(self, commands):
        result = subprocess.run(
            [str(self.binary.resolve())],
            input='\n'.join(['uci', *commands, 'quit']) + '\n',
            text=True, capture_output=True, timeout=30, check=True)
        samples = []
        pending = None
        for line in result.stdout.splitlines():
            if line.startswith('info string {'):
                data = json.loads(line.removeprefix('info string '))
                if data.get('cpu_sample'):
                    pending = data
            elif line.startswith('bestmove '):
                samples.append({'bestmove': line.split()[1], 'info': pending})
                pending = None
        self.assertEqual(len(samples), sum(c.startswith('go') for c in commands), result.stdout)
        return samples

    @staticmethod
    def options(level=11, seed=5, policy=True):
        return [f'setoption name Difficulty value {level}',
                f'setoption name Seed value {seed}',
                f'setoption name LitchCpuPolicy value {str(policy).lower()}']

    def assert_legal(self, sample, board):
        self.assertNotEqual(sample['bestmove'], '0000')
        self.assertIn(chess.Move.from_uci(sample['bestmove']), board.legal_moves)

    def test_all_full_profiles_and_both_sides(self):
        # An immediate mate keeps this inexpensive while running all real caps.
        for level in range(1, 12):
            for fen in MATE_FENS:
                with self.subTest(level=level, fen=fen):
                    sample, = self.run_uci(self.options(level) + [f'position fen {fen}', 'go'])
                    info = sample['info']
                    self.assert_legal(sample, chess.Board(fen))
                    self.assertEqual((info['selected_level'], info['profile_level']), (level, level))
                    self.assertEqual(info['max_nodes'], NODE_CAPS[level])
                    self.assertEqual(info['max_depth'], DEPTH_CAPS[level])
                    self.assertEqual(info['max_think_ms'], THINK_MS_CAPS[level])
                    self.assertEqual(info['first_root_node_reserve'], min(16384, NODE_CAPS[level]))
                    self.assertEqual(info['seed'], 5)
                    self.assertTrue(info['cpu_policy'])
                    self.assertFalse(info['novice_substitution'])
                    self.assertFalse(info['policy_rejected'])
                    self.assertEqual(sample['bestmove'], info['searched_move'])

    def test_all_opening_ramps_for_white_and_black(self):
        for level, expected in RAMPS.items():
            for black in (False, True):
                commands = self.options(level)
                boards = []
                for turn in range(4):
                    prefix = OPENING[:2 * turn + int(black)]
                    commands += ['position startpos moves ' + ' '.join(prefix), 'go depth 1']
                    board = chess.Board()
                    for move in prefix:
                        board.push_uci(move)
                    boards.append(board)
                samples = self.run_uci(commands)
                for sample, board, profile in zip(samples, boards, expected):
                    self.assert_legal(sample, board)
                    self.assertEqual(sample['info']['profile_level'], profile)
                    self.assertEqual(sample['info']['max_nodes'], NODE_CAPS[profile])
                    self.assertEqual(sample['info']['max_think_ms'],
                                     THINK_MS_CAPS[level])
                    self.assertTrue(sample['info']['opening_history'])
                    self.assertEqual(sample['info']['opening_plies'], len(board.move_stack))
                    self.assertEqual(sample['info']['reversible_halfmoves'], board.halfmove_clock)

    def test_opt_in_preserves_full_profile_diagnostics(self):
        sample, = self.run_uci(self.options(policy=False) + ['position startpos', 'go depth 1'])
        self.assertEqual(sample['info']['profile_level'], 11)
        self.assertFalse(sample['info']['opening_history'])
        self.assertEqual(sample['info']['reversible_halfmoves'], 65535)
        self.assertEqual(sample['info']['max_nodes'], NODE_CAPS[11])
        self.assertFalse(sample['info']['cpu_policy'])
        self.assertFalse(sample['info']['novice_substitution'])

    def test_novice_static_horizons_keep_best_move_for_both_sides(self):
        for level, seed in ((1, 2), (2, 3)):
            for black in (False, True):
                # Imported FEN keeps the controlled seed independent of replay.
                board = chess.Board()
                if black:
                    board.push_uci('e2e4')
                sample, = self.run_uci(self.options(level, seed) + [
                    'position fen ' + board.fen(), 'go'])
                self.assert_legal(sample, board)
                self.assertFalse(sample['info']['novice_substitution'])
                self.assertTrue(sample['info']['static_leaf_only'])
                self.assertTrue(sample['info']['preliminary_only'])
                self.assertEqual(sample['info']['completed_root_depth'], level)
                self.assertFalse(sample['info']['policy_rejected'])
                self.assertEqual(sample['bestmove'], sample['info']['searched_move'])

    def test_newgame_resets_opening_stage_and_seed_without_position(self):
        initial, late, reset = self.run_uci(self.options() + [
            'position startpos', 'go depth 1',
            'position startpos moves ' + ' '.join(OPENING), 'go depth 1',
            'ucinewgame', 'go depth 1'])
        self.assertEqual(initial, reset)
        self.assertEqual(late['info']['profile_level'], 11)
        self.assertNotEqual(late['info']['seed'], initial['info']['seed'])

    def test_seed_option_order_and_repeated_position_are_equivalent(self):
        for prefix in (OPENING[:3], OPENING[:6]):
            position = 'position startpos moves ' + ' '.join(prefix)
            before, repeated = self.run_uci(self.options(seed=17) + [
                position, 'go depth 1', position, 'go depth 1'])
            after, = self.run_uci(self.options(seed=1) + [
                position, 'setoption name Seed value 17', 'go depth 1'])
            self.assertEqual(before, repeated)
            self.assertEqual(before, after)
            self.assertNotEqual(before['info']['seed'], 17)

    def test_promotion_replay_hashes_canonical_committed_text(self):
        for fen, move in (
            ('7k/P7/8/8/8/8/8/7K w - - 0 1', 'a7a8'),
            ('7k/8/8/8/8/8/p7/7K b - - 0 1', 'a2a1'),
        ):
            for suffix in ('q', 'n'):
                spellings = [move + suffix, move + suffix.upper()]
                if suffix == 'q':
                    spellings.append(move)  # Existing engine defaults to Queen.
                samples = [self.run_uci(self.options(level=3) + [
                    f'position fen {fen} moves {spelling}', 'go depth 1'])[0]
                    for spelling in spellings]
                for sample in samples[1:]:
                    self.assertEqual(sample, samples[0])

    def test_imported_initial_fen_uses_full_profile(self):
        sample, = self.run_uci(self.options() + [
            'position fen ' + chess.STARTING_FEN, 'go depth 1'])
        self.assertEqual(sample['info']['profile_level'], 11)

    def test_small_cap_completes_the_escaped_first_root(self):
        fen = '4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31'
        sample, = self.run_uci(self.options(level=4, seed=1) + [
            'position fen ' + fen, 'go nodes 11200 depth 6'])
        self.assert_legal(sample, chess.Board(fen))
        self.assertEqual(sample['bestmove'], 'e8d8')
        self.assertEqual(sample['info']['searched_move'], 'e8d8')
        self.assertEqual(sample['info']['max_nodes'], 11200)
        self.assertEqual(sample['info']['first_root_node_reserve'], 11200)
        self.assertEqual(sample['info']['nodes'], 11200)
        self.assertTrue(sample['info']['preliminary_only'])
        self.assertEqual(sample['info']['completed_root_depth'], 2)
        self.assertFalse(sample['info']['novice_substitution'])

    def test_explicit_work_cap_retains_seed_or_completed_root(self):
        fen = '4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31'
        for cap, preliminary, depth, source, root in (
                (1, False, 0, 2, 5),
                (11200, True, 2, 0, 2),
                (16384, False, None, 0, 3)):
            with self.subTest(cap=cap):
                sample, = self.run_uci(self.options(level=4, seed=1) + [
                    'position fen ' + fen, f'go nodes {cap} depth 6'])
                self.assert_legal(sample, chess.Board(fen))
                self.assertEqual(sample['info']['nodes'], cap)
                self.assertEqual(sample['info']['first_root_node_reserve'], min(cap, 16384))
                self.assertEqual(sample['info']['preliminary_only'], preliminary)
                if depth is None:
                    self.assertGreaterEqual(sample['info']['completed_root_depth'], 1)
                else:
                    self.assertEqual(sample['info']['completed_root_depth'], depth)
                self.assertEqual(sample['info']['decision_source'], source)
                self.assertEqual(sample['info']['root_kind'], root)

    def test_reply_aware_roots_recover_both_level_seven_forfeits(self):
        cases = (
            ('rn5r/1pp1kNb1/6pp/1B3b2/N1QPn3/4P3/PPK2P1P/R1B4q w - - 4 19', 'c4c7'),
            ('r1bq1rk1/1pp4p/2n2p1p/2bNp3/2Pp4/Pp1P1NPP/3QPPB1/1R3RK1 b - - 1 15', 'a8a3'),
        )
        for fen, expected in cases:
            with self.subTest(fen=fen):
                sample, = self.run_uci(self.options(level=7, seed=1) + [
                    'position fen ' + fen, 'go nodes 19200 depth 9'])
                self.assert_legal(sample, chess.Board(fen))
                self.assertEqual(sample['bestmove'], expected)
                self.assertEqual(sample['info']['searched_move'], expected)
                self.assertEqual(sample['info']['nodes'], 19200)
                self.assertEqual(sample['info']['completed_root_depth'], 2)
                self.assertTrue(sample['info']['preliminary_only'])
                self.assertFalse(sample['info']['novice_substitution'])
                self.assertFalse(sample['info']['policy_rejected'])

    def test_unavailable_analysis_is_not_novice_substitution(self):
        # A terminal position has no usable move; reserve work cannot fabricate one.
        sample, = self.run_uci(self.options(level=1, seed=2) + [
            'position fen k7/1Q6/2K5/8/8/8/8/8 b - - 0 1', 'go nodes 1 depth 1'])
        self.assertEqual(sample['bestmove'], '0000')
        self.assertEqual(sample['info']['searched_move'], '')
        self.assertFalse(sample['info']['novice_substitution'])
        self.assertFalse(sample['info']['policy_rejected'])


if __name__ == '__main__':
    unittest.main()
