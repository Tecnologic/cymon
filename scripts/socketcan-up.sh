#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Brings up SocketCAN interfaces of CANable 2.0 adapters with the candleLight
# (gs_usb) firmware in CAN FD mode. Needs root (sudo).
#
#   scripts/socketcan-up.sh                    # all CAN interfaces, 1 Mbit/s, no BRS
#   scripts/socketcan-up.sh can0 can1          # these interfaces
#   BITRATE=500000 DBITRATE=2000000 scripts/socketcan-up.sh can0
#   VCAN=1 scripts/socketcan-up.sh             # also a CAN FD vcan0 for the UNIMOC simulation
set -euo pipefail

bitrate="${BITRATE:-1000000}"
dbitrate="${DBITRATE:-${bitrate}}"   # equal rates: FD frames at the nominal rate

if [[ "${VCAN:-0}" == 1 ]]; then
  modprobe vcan
  ip link show vcan0 >/dev/null 2>&1 || ip link add vcan0 type vcan
  ip link set vcan0 mtu 72 up
  echo "vcan0: CAN FD (mtu 72)"
fi

ifaces=("$@")
if [[ ${#ifaces[@]} -eq 0 ]]; then
  for path in /sys/class/net/*; do
    name="$(basename "${path}")"
    [[ "$(cat "${path}/type")" == 280 && "${name}" != vcan* ]] && ifaces+=("${name}")
  done
fi

for iface in "${ifaces[@]}"; do
  ip link set "${iface}" down
  ip link set "${iface}" type can bitrate "${bitrate}" dbitrate "${dbitrate}" fd on restart-ms 100
  ip link set "${iface}" up
  echo "${iface}: ${bitrate} bit/s nominal, ${dbitrate} bit/s data, CAN FD"
done
