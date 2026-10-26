#!/usr/bin/env bash
# Build the diagnostic raw FD only. Does not package or boot a phone image.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mu=$(cd "${MU_ROOT:-$here/../../../Mu-Silicium}" && pwd)
python=${UEFI_PYTHON:-python3}
target=${BUILD_TARGET:-RELEASE}
storage=${MUNCH_STORAGE:-0}
case "$storage" in 0|1) ;; *) echo "MUNCH_STORAGE must be 0 or 1" >&2; exit 1 ;; esac
case "$target" in
    RELEASE|DEBUG) ;;
    *) echo 'BUILD_TARGET must be RELEASE or DEBUG.' >&2; exit 1 ;;
esac
MU_ROOT="$mu" bash "$here/apply.sh"
"$python" - "$mu" <<'PY'
import hashlib, json, sys
from pathlib import Path
mu = Path(sys.argv[1])
firmware = mu / 'Binaries/munch'
manifest = json.loads((firmware / 'manifest.json').read_text())
expected = '1535264ea3b12c3aa5d06bd697fdc4df210c8089a63672e92a5d10cfcef86f2e'
if manifest['xbl_sha256'] != expected:
    raise SystemExit('Unreviewed F4 XBL revision; inspect its map and drivers first.')
for module in manifest['modules']:
    for section in module['sections']:
        data = (firmware / section['file']).read_bytes()
        if hashlib.sha256(data).hexdigest() != section['sha256']:
            raise SystemExit('Firmware module differs from extraction manifest.')
PY
if [ "${PREPARE:-0}" = 1 ]; then
    "$python" "$mu/Platforms/Xiaomi/munchPkg/DeviceBuild.py" --update -t "$target"
fi
"$python" "$mu/Platforms/Xiaomi/munchPkg/DeviceBuild.py" \
    TARGET="$target" ENABLE_SECUREBOOT=0 FD_BASE=0x9fc00000 \
    FD_SIZE=0x300000 FD_BLOCKS=0x300 DEVICE_MODEL=0 MUNCH_STORAGE="$storage"
fd="$mu/Build/munchPkg/${target}_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ "$(wc -c < "$fd")" -eq 3145728 ] || { echo 'Unexpected FD size.' >&2; exit 1; }
"$python" - "$fd" "$target" "$storage" <<'PY'
import hashlib, json, sys
from pathlib import Path
fd = Path(sys.argv[1])
metadata = {'target': 'munch', 'fd_sha256': hashlib.sha256(fd.read_bytes()).hexdigest(),
            'build_target': sys.argv[2],
            'diagnostic_revision': 3 if sys.argv[3] == '1' else 2,
            'storage_drivers': sys.argv[3] == '1',
            'mu_base': '42fe563791e51bf5500c2e470ed294ca0c94b97c',
            'xbl_sha256': '1535264ea3b12c3aa5d06bd697fdc4df210c8089a63672e92a5d10cfcef86f2e'}
fd.with_suffix(fd.suffix + '.munch.json').write_text(json.dumps(metadata, indent=2) + '\n')
PY
echo "Built diagnostic raw FD: $fd"
echo 'No boot image was packaged. Device boot and XNU handoff remain unverified.'
