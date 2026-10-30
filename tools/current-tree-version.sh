#!/bin/sh
# The release tag, v<yyyy>.<mm>.<dd>-<6 hex>, from a commit's UTC date (default HEAD).
# Adapted from VibeOffice's tools/current-tree-version.sh. Prints only; no files change.
set -eu
root=$(CDPATH= cd "$(dirname "$0")/.." && pwd -P)
cd "$root"
if ! command -v git >/dev/null 2>&1 ||
   [ "$(git rev-parse --show-toplevel 2>/dev/null || true)" != "$root" ]; then
  echo v0.0.0-dev
  exit 0
fi
commit=$(git rev-parse --verify "${1:-HEAD}^{commit}")
day=$(TZ=UTC git log -1 --format=%cd --date=format-local:%Y.%m.%d "$commit")
hash=$(printf '%s' "$commit" | cut -c1-6)
echo "v$day-$hash"
