#!/usr/bin/env bash
set -euo pipefail

digest_a="$1"
digest_b="$2"
comparator="$3"

out_a=$(mktemp)
out_b=$(mktemp)
diff_a=$(mktemp)
diff_b=$(mktemp)
trap 'rm -f "$out_a" "$out_b" "$diff_a" "$diff_b"' EXIT

"$digest_a" >"$out_a"
"$digest_b" >"$out_b"

"$comparator" "$out_a" "$out_b"
echo "OK: digest_dump_a and digest_dump_b agree"

echo "one" >"$diff_a"
echo "two" >"$diff_b"
if "$comparator" "$diff_a" "$diff_b"; then
  echo "FAIL: comparator did not reject mismatched files" >&2
  exit 1
fi
echo "OK: comparator rejects mismatched files"
