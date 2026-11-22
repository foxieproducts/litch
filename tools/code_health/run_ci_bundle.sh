#!/usr/bin/env bash

# Run every code-health evidence lane and preserve partial results for CI review.
set -u

repo_root="$(git rev-parse --show-toplevel)" || exit 2
cd "$repo_root" || exit 2

base_ref="${CODE_HEALTH_BASE_REF:?set CODE_HEALTH_BASE_REF to a merge-base commit or ref}"
revision="${GITHUB_SHA:-}"
if [[ -z "$revision" ]]; then
  revision="$(git rev-parse HEAD)" || exit 2
fi

output_root="build/code_health/$revision"
status_file="$output_root/lane-status.tsv"
mkdir -p "$output_root"
printf 'lane\tstatus\texit_code\telapsed_seconds\n' > "$status_file"

base_commit="$(git rev-parse --verify "${base_ref}^{commit}" 2>/dev/null || true)"
{
  printf 'revision=%s\n' "$revision"
  printf 'base_ref=%s\n' "$base_ref"
  printf 'base_commit=%s\n' "${base_commit:-unresolved}"
} > "$output_root/context.txt"

overall=0

run_lane() {
  local lane="$1"
  local rc=0
  local started_at=$SECONDS
  shift

  if "$@"; then
    printf '%s\tpassed\t0\t%s\n' "$lane" "$((SECONDS - started_at))" >> "$status_file"
  else
    rc=$?
    printf '%s\tfailed\t%s\t%s\n' "$lane" "$rc" "$((SECONDS - started_at))" >> "$status_file"
    printf 'code-health lane %s failed (exit %s)\n' "$lane" "$rc" >&2
    overall=1
  fi
  return 0
}

produce_complexity() {
  mkdir -p "$output_root/analysis"
  lizard --version > "$output_root/analysis/lizard.version.txt" 2>&1 || return $?
  # Lizard otherwise counts both sides of mutually exclusive preprocessor
  # branches and emits duplicate signatures that cannot be compared stably.
  lizard -Ecpre -l cpp --csv \
    src include lib/foxie_embedded/src lib/foxie_embedded/include \
    > "$output_root/analysis/complexity.csv"
}

run_lane native-coverage \
  ./litch code-health native-coverage \
  --backend gcovr --output-dir "$output_root/native"
run_lane flutter-coverage \
  ./litch code-health flutter-coverage \
  --output-dir "$output_root/flutter"
run_lane complexity produce_complexity
run_lane crap \
  ./litch code-health crap \
  --coverage "$output_root/native/coverage.info" \
  --coverage "$output_root/flutter/coverage.info" \
  --complexity "$output_root/analysis/complexity.csv" \
  --complexity-version "$output_root/analysis/lizard.version.txt" \
  --changed-from "$base_ref" \
  --output-dir "$output_root/crap"
run_lane static-analysis \
  ./litch code-health analyze \
  --changed-from "$base_ref" --tool cppcheck --tool dart \
  --report-only --output-dir "$output_root/analysis/static"
run_lane firmware-size \
  ./litch code-health size-snapshot \
  --build --role current --label "$revision" \
  --profile-label repository-default \
  --output-dir "$output_root/size"
run_lane runtime-headroom \
  ./litch code-health runtime-headroom \
  --report-only --output-dir "$output_root/support"

exit "$overall"
