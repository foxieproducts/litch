"""Compile only the S3 Classic scorer at its measured optimization level."""
from pathlib import Path

Import("env")

if env.BoardConfig().get("build.mcu") != "esp32s3":
    raise RuntimeError("Litch-Max evaluator optimization requires the S3 target")

scorer = Path(env.subst("$PROJECT_DIR")) / "lib/litch-max/src/classic_chess_evaluation.cpp"


def OptimizeEvaluation(buildEnv, node):
    if Path(node.srcnode().get_abspath()) != scorer:
        return node
    flags = [flag for flag in buildEnv.get("CCFLAGS", []) if not str(flag).startswith("-O")]
    return buildEnv.Object(node, CCFLAGS=flags + ["-O3"])


env.AddBuildMiddleware(OptimizeEvaluation)
