#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
game=${1:?usage: wine_smoke.sh <package dir> [work dir]}
work=${2:-$(mktemp -d)}

for tool in wine x86_64-w64-mingw32-g++ python3; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done

mkdir -p "$work"
x86_64-w64-mingw32-g++ -shared -O2 "$here/xmem.cpp" -o "$work/xmem.dll" -static
x86_64-w64-mingw32-g++ -O2 "$here/bink_test.cpp" -o "$work/bink_test.exe" -static

for dll in Bink2Scarlett.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll \
           msvcp140_atomic_wait.dll msvcp140_codecvt_ids.dll vcruntime140.dll \
           vcruntime140_1.dll concrt140.dll vccorlib140.dll; do
  if [ -f "$game/$dll" ]; then cp "$game/$dll" "$work/"; fi
done

python3 "$here/carve_bink.py" "$game/movies.rpf" "$work/video" 1 >/dev/null
video=$(find "$work/video" -name '*.bk2' -print -quit)
[ -n "$video" ] || { echo "no .bk2 carved from $game/movies.rpf" >&2; exit 1; }

out=$(cd "$work" && WINEDEBUG=-all timeout 300 wine bink_test.exe "$video" 2>/dev/null)
printf '%s\n' "$out"
if ! grep -q "DoFrame errors: 0" <<<"$out"; then
  echo "FAIL: Bink2Scarlett.dll did not decode cleanly under Wine" >&2
  exit 1
fi
echo "PASS: $work"
