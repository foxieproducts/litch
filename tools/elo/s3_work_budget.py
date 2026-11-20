"""Measured S3 work caps for host matches; this is a fixed-node model, not board Elo."""
import hashlib
import json
from pathlib import Path

NODE_CAPS = (0, 180000, 180000, 180000, 210000, 225000, 300000, 360000, 540000, 810000, 1200000, 1800000)
DEPTH_CAPS = (0, 3, 3, 4, 6, 7, 8, 9, 11, 12, 14, 16)
TIME_CAPS = (0, 900, 900, 900, 2000, 4000, 7000, 10000, 15000, 25000, 40000, 60000)
COMPLEX_CASES = (1, 2, 3, 6)
MAX_WORKSPACE_BYTES = 14 * 1024
STATIC_NOVICE_POLICY = "classic-static-novice-v2"
FAST_CLASSIC_POLICY = "classic-fast-deadlines-v3"
FAST_TIME_CAPS = tuple(t * 7 // 10 if 3 <= level <= 10 else t
                       for level, t in enumerate(TIME_CAPS))
COMPOSITE_RESULT = "PASS_COMPOSITE_WORK_CALIBRATION_GAME_PRESERVED"
NOVICE_NODE_CAPS = (0, 256, 4096) + NODE_CAPS[3:]
NOVICE_DEPTH_CAPS = (0, 1, 2) + DEPTH_CAPS[3:]

def profile_caps(receipt):
    policy = receipt.get("cpu_policy", "classic-v1")
    if policy not in ("classic-v1", STATIC_NOVICE_POLICY, FAST_CLASSIC_POLICY):
        raise ValueError("unknown CPU policy in S3 capture")
    return (NOVICE_NODE_CAPS, NOVICE_DEPTH_CAPS) if policy != "classic-v1" else (NODE_CAPS, DEPTH_CAPS)


def profile_times(receipt):
    profile_caps(receipt)  # Validate the CPU policy before selecting its time limits.
    return FAST_TIME_CAPS if receipt.get("cpu_policy") == FAST_CLASSIC_POLICY else TIME_CAPS


# Production opening ramp, checked against actual UCI diagnostics on every move.
OPENING_PROFILES = ((0, 0, 0), (1, 1, 1), (2, 2, 2), (3, 3, 3),
                    (3, 3, 4), (3, 4, 5), (3, 4, 6), (3, 5, 7),
                    (3, 5, 8), (3, 6, 9), (3, 6, 10), (3, 7, 11))


class CalibrationInvalid(RuntimeError):
    def __init__(self, reason, evidence=None):
        super().__init__(reason)
        self.evidence = {"reason": reason, "diagnostic": evidence}


def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def measured_caps(receipt):
    if not isinstance(receipt, dict) or receipt.get("result") not in ("PASS_FINITE_COMPUTE_CAPTURE_GAME_PRESERVED", COMPOSITE_RESULT):
        raise ValueError("S3 capture did not complete successfully")
    if receipt.get("result") == COMPOSITE_RESULT and receipt.get("cpu_policy") != STATIC_NOVICE_POLICY:
        raise ValueError("composite S3 calibration requires an explicit current CPU policy")
    node_caps, depth_caps = profile_caps(receipt)
    time_caps = profile_times(receipt)
    runs = receipt.get("runs", [])
    if len(runs) != 66 or {(j["case"], j["level"]) for j in runs} != {(c, l) for c in range(1, 7) for l in range(1, 12)}:
        raise ValueError("S3 calibration requires all six positions at all eleven levels")
    for job in runs:
        level, final = job["level"], job["final"]
        profile = final["profiles"]["requested"]
        if job["result"] != "DONE" or not job.get("boot_continuity") or final["cancel"]:
            raise ValueError("S3 sample is incomplete or canceled")
        if (profile["level"], profile["node_ceiling"], profile["depth_ceiling"], profile["time_ceiling_ms"]) != (level, node_caps[level], depth_caps[level], time_caps[level]):
            raise ValueError("S3 sample uses another production profile")
        nodes = final["final_nodes"]
        if type(nodes) is not int or not 0 < nodes <= node_caps[level] or not final["final_nodes_valid"]:
            raise ValueError("S3 sample has no valid work count")
    caps = {}
    for level in range(1, 12):
        panel = [j["final"] for j in runs if j["level"] == level and j["case"] in COMPLEX_CASES]
        if receipt.get("cpu_policy") in (STATIC_NOVICE_POLICY, FAST_CLASSIC_POLICY) and level <= 2:
            # A completed shallow horizon is not a throughput ceiling. Retain
            # its explicit production cap, reduced by any measured deadline.
            caps[str(level)] = min([node_caps[level]] +
                [f["final_nodes"] for f in panel if f.get("stop") == "deadline"])
        else:
            caps[str(level)] = min(f["final_nodes"] for f in panel)
    return caps


def write_model(receipt_path, binary, output):
    receipt = json.loads(Path(receipt_path).read_text())
    model = {"schema": "litch.s3-fixed-work.v1", "receipt": str(Path(receipt_path).resolve()),
             "receipt_sha256": file_sha(receipt_path), "uci_sha256": file_sha(binary),
             "target": receipt["target"], "image": receipt["expected_image"],
             "cpu_policy": receipt.get("cpu_policy", "classic-v1"),
             "candidate": receipt["candidate"], "node_caps": measured_caps(receipt),
             "method": "Full-search levels3-11: minimum observed nodes across fixed complex cases1,2,3,6 at the receipt's temporal policy. Static novice levels1/2: explicit production caps256/4096 reduced by any complex-panel deadline observations; early static-horizon exhaustion does not measure throughput. Chosen before outcomes.",
             "limits": "Fixed-node host model. Position-dependent S3 cost varies; no universal board deadline or actual-board Elo claim."}
    with Path(output).open("x") as stream:
        json.dump(model, stream, indent=2); stream.write("\n")
    return model


class S3WorkBudget:
    def __init__(self, path, level, binary):
        self.sha256 = file_sha(path)
        self.model = json.loads(Path(path).read_text())
        if not isinstance(self.model, dict) or self.model.get("schema") != "litch.s3-fixed-work.v1" or type(level) is not int or level not in range(1, 12):
            raise ValueError("invalid S3 work model or selected difficulty")
        receipt_path = self.model["receipt"]
        if file_sha(receipt_path) != self.model["receipt_sha256"] or file_sha(binary) != self.model["uci_sha256"]:
            raise ValueError("S3 capture or UCI binary differs from the calibration")
        receipt = json.loads(Path(receipt_path).read_text())
        expected = measured_caps(receipt)
        if any(self.model.get(key) != receipt.get(source) for key, source in
               (("target", "target"), ("image", "expected_image"), ("candidate", "candidate"))):
            raise ValueError("S3 model provenance differs from its pinned capture")
        if self.model["node_caps"] != expected:
            raise ValueError("S3 node caps differ from the measured fixed-panel rule")
        self.node_caps, self.depth_caps = profile_caps(receipt)
        self.time_caps = profile_times(receipt)
        self.policy = receipt.get("cpu_policy", "classic-v1")
        if self.model.get("cpu_policy", self.policy) != self.policy:
            raise ValueError("S3 model CPU policy differs from its pinned capture")
        self.static_novice = self.policy in (STATIC_NOVICE_POLICY, FAST_CLASSIC_POLICY)
        self.level, self.nodes = level, expected[str(level)]

    def validate(self, info, board=None):
        raw = info.get("string")
        try:
            sample = json.loads(raw)
        except (TypeError, ValueError):
            raise CalibrationInvalid("missing or malformed Litch CPU work diagnostic", raw)
        if not isinstance(sample, dict):
            raise CalibrationInvalid("malformed Litch CPU work diagnostic", raw)
        for key in ("seed", "completed_root_depth"):
            if type(sample.get(key)) is not int or sample[key] < 0:
                raise CalibrationInvalid("missing or invalid CPU selector/search diagnostic", sample)
        if sample["seed"] > 0xffffffff or any(type(sample.get(k)) is not bool for k in
                                             ("novice_substitution", "preliminary_only")):
            raise CalibrationInvalid("missing or invalid CPU selector/search diagnostic", sample)
        if not isinstance(sample.get("searched_move"), str):
            raise CalibrationInvalid("missing searched CPU move", sample)
        source, root = sample.get("decision_source"), sample.get("root_kind")
        provenance_valid = (source, root) == (2, 5) if sample["completed_root_depth"] == 0 else (
            source == 0 and root in (2, 3) and sample["preliminary_only"] == (root == 2))
        if type(source) is not int or type(root) is not int or not provenance_valid:
            raise CalibrationInvalid("CPU decision provenance does not match its completed depth", sample)
        workspace = sample.get("workspace_bytes")
        if type(workspace) is not int or not 0 < workspace <= MAX_WORKSPACE_BYTES:
            raise CalibrationInvalid("CPU workspace exceeds the no-PSRAM search budget", sample)
        if board is not None:
            import chess
            try:
                searched_move = chess.Move.from_uci(sample["searched_move"])
            except ValueError:
                raise CalibrationInvalid("malformed searched CPU move", sample)
            if searched_move not in board.legal_moves:
                raise CalibrationInvalid("searched CPU move is illegal at the requested position", sample)
            canonical_history = board.root().fen() == chess.STARTING_FEN
            clock = 0 if canonical_history else 65535
            replay = board.root()
            for move in board.move_stack:
                if replay.is_capture(move) or replay.piece_type_at(move.from_square) == chess.PAWN:
                    clock = 0
                elif clock != 65535:
                    clock += 1
                replay.push(move)
            book_history = canonical_history and len(board.move_stack) <= 16 and not any(
                move.promotion for move in board.move_stack)
            history = {"reversible_halfmoves": clock, "opening_history": book_history,
                       "opening_plies": len(board.move_stack) if book_history else 0}
            if any(type(sample.get(k)) is not type(v) or sample.get(k) != v for k, v in history.items()):
                raise CalibrationInvalid("CPU game knowledge differs from retained production history", sample)
        profile = sample.get("profile_level")
        if type(profile) is not int or profile not in range(1, self.level + 1):
            raise CalibrationInvalid("invalid effective CPU profile", sample)
        if board is not None:
            import chess
            opening_turn = min(len(board.move_stack) // 2, 2) if board.root().fen() == chess.STARTING_FEN else 2
            if profile != OPENING_PROFILES[self.level][opening_turn]:
                raise CalibrationInvalid("opening ramp differs from the production CPU policy", sample)
        if self.static_novice:
            if sample["novice_substitution"]:
                raise CalibrationInvalid("current CPU policy deliberately substituted a move", sample)
            static_leaf = profile <= 2
            if sample.get("static_leaf_only") is not static_leaf or (
                    static_leaf and sample["completed_root_depth"] != 0 and
                    (root != 2 or sample["completed_root_depth"] != profile)):
                raise CalibrationInvalid("CPU static novice horizon differs from the measured policy", sample)
        cap = min(self.nodes, self.node_caps[profile])
        expected = {"cpu_sample": True, "cpu_policy": True, "selected_level": self.level,
                    "max_nodes": cap, "max_depth": self.depth_caps[profile], "first_root_node_reserve": min(16384, cap),
                    "max_think_ms": self.time_caps[self.level], "policy_rejected": False}
        if self.policy == FAST_CLASSIC_POLICY:
            expected["cpu_policy_id"] = FAST_CLASSIC_POLICY
        if any(type(sample.get(k)) is not type(v) or sample.get(k) != v for k, v in expected.items()):
            raise CalibrationInvalid("CPU policy or effective limits differ from the calibrated request", sample)
        nodes = sample.get("nodes")
        if type(nodes) is not int or not 0 <= nodes <= cap:
            raise CalibrationInvalid("search exceeded the measured S3 work cap", sample)
        return {"calibration_sha256": self.sha256, "selected_level": self.level,
                "requested_nodes": self.nodes, "effective_node_cap": cap, "observed_nodes": nodes,
                "model": "S3-calibrated fixed-node host model"}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--uci", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(write_model(args.receipt, args.uci, args.output)))
    except (KeyError, ValueError, OSError) as error:
        parser.error(str(error))
