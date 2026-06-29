#!/usr/bin/env bash
# Build, flash, and measure one firmware end-to-end.
#   ./run.sh httpc /dev/ttyUSB0           # auto-detect IP from boot log
#   ./run.sh esp   /dev/ttyUSB0 192.168.1.50  # or pass IP explicitly
set -euo pipefail
FW=$1; PORT=${2:-/dev/ttyUSB0}; IP=${3:-}
DIR=$(cd "$(dirname "$0")" && pwd)
case "$FW" in
  httpc) PROJ=$DIR/firmware-httpc; LBL=http-c ;;
  esp)   PROJ=$DIR/firmware-esp;   LBL=esp_http_server ;;
  *) echo "usage: run.sh {httpc|esp} [serial] [ip]"; exit 1 ;;
esac
[ -f "$HOME/esp/esp-idf/export.sh" ] && . "$HOME/esp/esp-idf/export.sh"
cd "$PROJ"
idf.py set-target esp32
idf.py build
idf.py size | tee "$DIR/size-$FW.txt"
idf.py -p "$PORT" flash
if [ -z "$IP" ]; then
  echo "Flashed. Reading serial to discover device IP (timeout 60s)..."
  IP=$(timeout 60 idf.py -p "$PORT" monitor 2>/dev/null \
       | grep -m1 -oE 'sta ip: [0-9.]+|IPv4 address: [0-9.]+' | grep -oE '[0-9.]+') || true
fi
[ -z "$IP" ] && { echo "Could not detect IP. Re-run with: ./run.sh $FW $PORT <ip>"; exit 1; }
echo "Device IP: $IP"
python3 "$DIR/measure.py" "$IP" --label "$LBL" --out "$DIR/results-$FW.json"
