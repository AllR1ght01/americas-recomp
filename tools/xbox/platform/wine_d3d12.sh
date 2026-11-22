#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"

missing=""
for tool in wine wineboot x86_64-w64-mingw32-g++ timeout python3; do
  command -v "$tool" >/dev/null || missing+=" $tool"
done

unset RADV_DEBUG MESA_SHADER_CACHE_DISABLE || true
export LC_ALL=C TZ=UTC WINEDEBUG=-all
export WINEPREFIX="$work/prefix"
export WINEDLLOVERRIDES="d3d12_xs=n"

wine_version=$(wine --version 2>/dev/null || true)
gpu=$(vulkaninfo --summary 2>/dev/null | sed -n 's/^[[:space:]]*deviceName[[:space:]]*=[[:space:]]*//p' | head -1 || true)
driver=$(vulkaninfo --summary 2>/dev/null | sed -n 's/^[[:space:]]*driverInfo[[:space:]]*=[[:space:]]*//p' | head -1 || true)
if [ -z "$gpu" ]; then
  gpu=$(glxinfo -B 2>/dev/null | sed -n 's/^ *Device: //p' | head -1 || true)
fi
if [ -z "$gpu" ]; then
  gpu=$(lspci 2>/dev/null | sed -n 's/.*VGA compatible controller: //p' | head -1 || true)
fi

write_report() {
  REPORT_PATH="$work/report.json" RESULT="$1" SKIPPED="$2" WINE_VERSION="$wine_version" \
  KERNEL="$(uname -r)" GPU="$gpu" DRIVER="$driver" ADAPTER="${adapter:-}" \
  FEATURE_LEVEL="${feature_level:-}" CLEAR="${clear:-}" EXPECTED="${expected:-}" DEVICE="${device:-}" \
  python3 - <<'PY'
import json, os
keys = ["RESULT", "SKIPPED", "WINE_VERSION", "KERNEL", "GPU", "DRIVER", "ADAPTER",
        "FEATURE_LEVEL", "CLEAR", "EXPECTED", "DEVICE"]
json.dump({key.lower(): os.environ.get(key, "") for key in keys}, open(os.environ["REPORT_PATH"], "w"), indent=2)
PY
}

if [ -n "$missing" ]; then
  echo "skipped: missing$missing"
  echo "RESULT: SKIP"
  write_report skip "missing$missing"
  echo "report: $work/report.json"
  exit 0
fi

if [ ! -d "$WINEPREFIX" ]; then
  if ! wineboot -u >/dev/null 2>&1; then
    echo "skipped: wineboot failed"
    echo "RESULT: SKIP"
    write_report skip "wineboot failed"
    echo "report: $work/report.json"
    exit 0
  fi
fi

x86_64-w64-mingw32-g++ -shared -O2 -std=c++17 "$here/d3d12_xs.cpp" "$here/d3d12_xs_device.cpp" \
  -o "$work/d3d12_xs.dll" -static
x86_64-w64-mingw32-g++ -O2 -std=c++17 "$here/d3d12_xs_test.cpp" \
  -o "$work/d3d12_xs_test.exe" -static -ldxgi

set +e
out=$(cd "$work" && timeout 300 wine d3d12_xs_test.exe 2>/dev/null | tr -d '\r')
rc=$?
set -e
printf '%s\n' "$out"

adapter=$(sed -n 's/^adapter: //p' <<<"$out" | head -1)
feature_level=$(sed -n 's/^feature level: //p' <<<"$out" | head -1)
device=$(sed -n 's/^device: //p' <<<"$out" | head -1)
clear=$(sed -n 's/^clear: \([^ ]*\) expected .*/\1/p' <<<"$out" | head -1)
expected=$(sed -n 's/^clear: .* expected //p' <<<"$out" | head -1)
skipped=$(sed -n 's/^skipped: //p' <<<"$out" | head -1)

if grep -q '^RESULT: PASS' <<<"$out"; then
  result=pass
elif grep -q '^RESULT: SKIP' <<<"$out"; then
  result=skip
else
  result=fail
fi

write_report "$result" "$skipped"
echo "report: $work/report.json"

if [ "$result" = fail ]; then
  echo "FAIL: synthetic D3D12 clear did not complete under Wine" >&2
  exit 1
fi
echo "${result^^}: $work"
