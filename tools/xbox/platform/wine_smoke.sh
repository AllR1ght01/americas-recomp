#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
game=${1:?usage: wine_smoke.sh <package dir> [work dir]}
work=${2:-$(mktemp -d)}

for tool in wine wineboot x86_64-w64-mingw32-g++ python3; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done

export LC_ALL=C TZ=UTC WINEDEBUG=-all
export WINEPREFIX="$work/prefix"
export WINEDLLOVERRIDES="xmem,d3d12_xs=n"
export MESA_SHADER_CACHE_MAX_SIZE=1G
unset RADV_DEBUG MESA_SHADER_CACHE_DISABLE || true

mkdir -p "$work"
if [ ! -d "$WINEPREFIX" ]; then
  wineboot -u >/dev/null 2>&1
fi

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

out=$(cd "$work" && timeout 300 wine bink_test.exe "$video" 2>/dev/null)
printf '%s\n' "$out"
if ! grep -q "DoFrame errors: 0" <<<"$out"; then
  echo "FAIL: Bink2Scarlett.dll did not decode cleanly under Wine" >&2
  exit 1
fi

gpu=$(glxinfo -B 2>/dev/null | sed -n 's/^ *Device: //p' | head -1)
if [ -z "$gpu" ]; then
  gpu=$(lspci 2>/dev/null | sed -n 's/.*VGA compatible controller: //p' | head -1)
fi
opened=$(sed -n 's/^opened: //p' <<<"$out" | head -1)
frames=$(sed -n 's/.*, \([0-9]*\) frames/\1/p' <<<"$out" | head -1)
size=$(sed -n 's/^opened: \([0-9]*x[0-9]*\).*/\1/p' <<<"$out" | head -1)

REPORT_PATH="$work/report.json" PACKAGE="$game" VIDEO="$(basename "$video")" \
WINE="$(wine --version 2>/dev/null)" KERNEL="$(uname -r)" GPU="$gpu" \
OPENED="$opened" FRAMES="$frames" SIZE="$size" RESULT="pass" \
python3 - <<'PY'
import json, os
keys = ["PACKAGE", "VIDEO", "WINE", "KERNEL", "GPU", "OPENED", "FRAMES", "SIZE", "RESULT"]
json.dump({key: os.environ.get(key, "") for key in keys}, open(os.environ["REPORT_PATH"], "w"), indent=2)
PY

echo "PASS: $work"
echo "report: $work/report.json"
