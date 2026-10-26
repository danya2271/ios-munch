#!/bin/sh
# Configure the Munch CDC-ECM debug link and collect its UDP console log.
set -eu

MAC=${MUNCH_USB_MAC:-02:00:5e:77:ee:01}
LOGFILE=${1:-}

command -v ip >/dev/null 2>&1 || { echo 'munch-usbnet: ip is required' >&2; exit 1; }

echo "waiting for Munch USB Ethernet ($MAC)..." >&2
while :; do
	ifname=$(ip -o link | awk -v mac="$MAC" 'tolower($0) ~ tolower(mac) {split($2, a, ":"); sub(/@.*/, "", a[1]); print a[1]; exit}')
	[ -n "$ifname" ] && break
	sleep 1
done

if command -v nmcli >/dev/null 2>&1; then
	nmcli dev set "$ifname" managed no 2>/dev/null || true
fi

sudo ip addr replace 192.168.7.1/24 dev "$ifname"
sudo ip link set "$ifname" up
echo "$ifname up: 192.168.7.1 <-> Munch 192.168.7.2; UDP log on 5140" >&2

if command -v nc >/dev/null 2>&1; then
	if [ -n "$LOGFILE" ]; then
		nc -u -l -k 5140 | tee -a "$LOGFILE"
	else
		nc -u -l -k 5140
	fi
else
	command -v socat >/dev/null 2>&1 || { echo 'munch-usbnet: nc or socat is required' >&2; exit 1; }
	if [ -n "$LOGFILE" ]; then
		socat -u UDP-RECV:5140,reuseaddr - | tee -a "$LOGFILE"
	else
		socat -u UDP-RECV:5140,reuseaddr -
	fi
fi
