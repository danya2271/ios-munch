#!/bin/sh
# Deliberately disabled during the Munch RAM-only bring-up.
#
# The historical implementation read a kernel log ring from the tail of the
# phone's userdata partition. The current device policy forbids any userdata
# read, mount, backup or write. Keep this command as a visible fail-closed
# stub so an old workflow cannot access storage by accident.
set -eu

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	echo "$(basename "$0") is disabled for the Munch RAM-only workflow."
	echo "It never contacts ADB or reads userdata."
	echo "Parse an existing offline capture with:"
	echo "  python3 tools/munch-log.py AREA.bin OUT.txt"
	exit 0
fi

echo "Error: $(basename "$0") is disabled; userdata access is forbidden." >&2
exit 1
