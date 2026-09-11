#!/usr/bin/env bash
set -euo pipefail

if cmp -s "$1" "$2"; then
  exit 0
fi

echo "MISMATCH:" >&2
echo "  $1: $(cat "$1")" >&2
echo "  $2: $(cat "$2")" >&2
exit 1
