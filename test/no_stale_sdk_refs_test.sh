#!/usr/bin/env bash
# Guards the migration contract: no tracked, live build file may reference a tree
# the migration removes (brightsign-oe/, the repo-root toolkit/, or the in-repo
# ./sdk). Docs and this test's own matches are excluded. Catches orphaned helpers
# and test harnesses left pointing at deleted directories.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
fail=0

# Live build files to scan (tracked; excludes docs and the superpowers workspace).
mapfile -t FILES < <(git ls-files -- \
  '*.sh' '*.cmake' 'CMakeLists.txt' 'test/CMakeLists.txt' 'Makefile' 'package' 'build_test.sh' \
  | grep -vE '^(docs/|\.superpowers/)')

scan() { # pattern label
  local pat="$1" label="$2" hit
  # Exclude explanatory comments that merely name the removed trees.
  hit="$(grep -nE "$pat" "${FILES[@]}" 2>/dev/null \
        | grep -vE 'no longer|retired compile-models|now in the shared cache' || true)"
  if [ -n "$hit" ]; then
    echo "  FAIL $label:"; echo "$hit" | sed 's/^/      /'; fail=1
  else
    echo "  ok   $label"
  fi
}

# brightsign-oe tree (removed).
scan 'brightsign-oe' "no brightsign-oe refs"
# repo-root toolkit dir used as a path prefix (removed; now in the cache).
scan '(\.\./|\$\{CMAKE_SOURCE_DIR\}/\.\./)toolkit/' "no repo-root toolkit/ refs"
# in-repo ./sdk as the SDK source (removed; SDK now from the shared cache). The
# cache's own <cache>/sdk/environment-setup path is legitimate and not matched.
scan '\$\(pwd\)/sdk|-d \./sdk' "no in-repo ./sdk SDK-source refs"

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
