#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$ROOT/.tools/espressif}"
export IDF_PATH="${IDF_PATH:-$ROOT/.tools/esp-idf}"
for python_env in "$IDF_TOOLS_PATH"/python_env/idf6.0_py*_env; do
  if [[ -x "$python_env/bin/python" ]]; then
    export PATH="$python_env/bin:$PATH"
    break
  fi
done
if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo 'ESP-IDF v6.0.1 is missing. See README.md for setup.' >&2
  exit 1
fi
source "$IDF_PATH/export.sh" >/dev/null
if [[ "$(git -C "$IDF_PATH" rev-parse HEAD)" != 8c19b156084a0753687347cca1f5355782893533 ]]; then
  echo 'This project requires the pinned ESP-IDF v6.0.1 commit.' >&2
  exit 1
fi
if [[ ! -f "$ROOT/.build/sdk/esp32/CMakeLists.txt" ]]; then
  echo 'Run scripts/prepare_firmware.py first.' >&2
  exit 1
fi
for action in "$@"; do
  if [[ "$action" == flash || "$action" == app-flash ]]; then
    python - "$ROOT/.build/stopwatch/sdkconfig" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text() if path.exists() else ''
if ('CONFIG_MUSE_BRIDGE_URL="wss://' not in text or 'compile-check.invalid' in text
        or 'CONFIG_MUSE_BRIDGE_TOKEN=""' in text):
    raise SystemExit('Run scripts/configure.py and rebuild with the real Worker settings before flashing.')
PY
  fi
done
DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch;$ROOT/firmware/sdkconfig.bridge"
if [[ -f "$ROOT/.private/sdkconfig.local" ]]; then
  DEFAULTS="$DEFAULTS;$ROOT/.private/sdkconfig.local"
fi
cd "$ROOT/.build/sdk/esp32"
exec idf.py -B "$ROOT/.build/stopwatch" -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG="$ROOT/.build/stopwatch/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
