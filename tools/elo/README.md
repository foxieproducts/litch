# Measuring chess engine strength

Build the [UCI runner](../uci/README.md) and install the Python packages in [requirements.txt](requirements.txt). These tools run Classic chess searches on the host computer. Host timings measure the host, and node counts describe search work. A comparison against Stockfish applies to Classic chess.

## Sample hints

Supply a JSON list of positions with an `id` and a FEN for each. The sampler records hint choices during one continuous search. Use the same positions and search budgets when comparing versions.

```sh
python3 tools/elo/sample_hints.py --engine build/uci/litch_uci \
  --positions positions.json --output hint_samples.jsonl
```

## Play paired matches

[run_match.py](run_match.py) runs UCI engines against each other and writes PGN. Use `python3 tools/elo/run_match.py --help` for engine options, openings, time controls and game count. [s3_work_budget.py](s3_work_budget.py) converts a matching board calibration into search budgets when comparing the host engine against work measured on an ESP32-S3.

Calibration receipts identify their CPU policy. `classic-fast-deadlines-v3`
uses the shorter Classic deadlines documented by the [UCI runner](../uci/README.md#board-cpu-policy-for-matches)
and requires a complete six-position, eleven-level capture. Its host diagnostics
must report the same `cpu_policy_id`. Historical `classic-v1` and
`classic-static-novice-v2` receipts retain their original limits and validation.
Models pin the receipt and UCI executable hashes; a new executable needs a
matching model. The minimum observed work across complex panel positions is a
fixed-node host comparison budget, not a universal board deadline or a product
node ceiling. Static novice horizons retain their explicit caps unless the
capture records an earlier deadline.
