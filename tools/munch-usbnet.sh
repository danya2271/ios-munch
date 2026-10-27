#!/bin/sh
# Configure the Munch CDC-ECM debug link and collect its UDP console log.
set -eu

MAC=${MUNCH_USB_MAC:-02:00:5e:77:ee:01}
LOGFILE=${1:-}
TIMEOUT=${MUNCH_USB_TIMEOUT:-120}

command -v ip >/dev/null 2>&1 || { echo 'munch-usbnet: ip is required' >&2; exit 1; }

case $TIMEOUT in
	''|*[!0-9]*) echo 'munch-usbnet: MUNCH_USB_TIMEOUT must be seconds (0 means no timeout)' >&2; exit 1 ;;
esac

if [ -n "$LOGFILE" ]; then
	mkdir -p "$(dirname "$LOGFILE")"
	: >> "$LOGFILE"
	printf 'munch-usbnet: waiting for USB Ethernet %s; phone logs are not available until it enumerates\n' "$MAC" | tee -a "$LOGFILE" >&2
else
	echo "waiting for Munch USB Ethernet ($MAC)..." >&2
fi

elapsed=0
while :; do
	ifname=$(ip -o link | awk -v mac="$MAC" 'tolower($0) ~ tolower(mac) {split($2, a, ":"); sub(/@.*/, "", a[1]); print a[1]; exit}')
	[ -n "$ifname" ] && break
	if [ "$TIMEOUT" -ne 0 ] && [ "$elapsed" -ge "$TIMEOUT" ]; then
		message="munch-usbnet: timed out after ${elapsed}s; no interface with MAC $MAC"
		if [ -n "$LOGFILE" ]; then
			printf '%s\n' "$message" | tee -a "$LOGFILE" >&2
		else
			printf '%s\n' "$message" >&2
		fi
		echo 'Host links:' >&2
		ip -br link >&2 || true
		if command -v lsusb >/dev/null 2>&1; then
			echo 'USB devices:' >&2
			lsusb -nn >&2 || true
		fi
		echo "For USB connect/disconnect events, run \`sudo dmesg -w\` in another terminal before booting." >&2
		exit 1
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done

if command -v nmcli >/dev/null 2>&1; then
	nmcli dev set "$ifname" managed no 2>/dev/null || true
fi

sudo ip addr replace 192.168.7.1/24 dev "$ifname"
sudo ip link set "$ifname" up
echo "$ifname up: 192.168.7.1 <-> Munch 192.168.7.2; UDP log on 5140" >&2

listen_udp() {
	port=5140
	if command -v socat >/dev/null 2>&1; then
		socat -u "udp-recv:$port,reuseaddr" -
	elif command -v ncat >/dev/null 2>&1; then
		ncat -l -u -k "$port"
	elif command -v nc >/dev/null 2>&1; then
		if nc -h 2>&1 | grep -q -- '-k'; then
			nc -u -l -k "$port"
		else
			while :; do
				nc -u -l -p "$port" 2>/dev/null || nc -u -l "$port" 2>/dev/null || return 1
			done
		fi
	else
		echo 'munch-usbnet: nc, ncat, or socat is required' >&2
		return 1
	fi
}

if [ -n "$LOGFILE" ]; then
	listen_udp | tee -a "$LOGFILE"
else
	listen_udp
fi
