#!/usr/bin/env python3
"""Package a locally verified system APFS and its matching munch RAM loader."""
import argparse
import hashlib
import json
import shutil
import subprocess
import zipfile
from pathlib import Path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def package(config_path, boot_path, source_report, output):
    here = Path(__file__).resolve().parent
    config = json.loads(config_path.read_text())
    source = json.loads(source_report.read_text())
    boot = json.loads(Path(str(boot_path) + '.json').read_text())
    if (config['version'], config['build']) != ('16.5', '20F66'):
        raise ValueError('Unsupported system build')
    if source['build'] != '20F66' or len(source['members']) != 5:
        raise ValueError('Full source-member integrity report required')
    if digest(source_report) != config['source_report_sha256']:
        raise ValueError('Source integrity report differs from the boot configuration')
    image = Path(config['system']['path'])
    if image.stat().st_size != config['system']['size'] or digest(image) != config['system']['sha256']:
        raise ValueError('System APFS changed after boot configuration was generated')
    if (boot['mode'] != 'ios165-read-only-ramboot' or boot['checkpoint'] != 58 or
            boot['image_sha256'] != digest(boot_path) or boot_path.stat().st_size > 120 * 1024 * 1024):
        raise ValueError('Expected the matching small RAM boot image')
    if image.stat().st_size % 4096:
        raise ValueError('System APFS must be 4096-byte aligned')
    output.mkdir(parents=True, exist_ok=False)
    installer = (here / 'install-iosroot.sh.in').read_text()
    for key, value in {'SYSTEM_SIZE': config['system']['size'],
                       'SYSTEM_SHA256': config['system']['sha256'],
                       'SYSTEM_BLOCKS': config['system']['size'] // 4096}.items():
        installer = installer.replace('@' + key + '@', str(value))
    if '@SYSTEM_' in installer:
        raise ValueError('Unexpanded installer template')
    (output / 'install.sh').write_text(installer)
    subprocess.run(['bash', '-n', str(output / 'install.sh')], check=True)
    # Local hard links avoid another multi-GB copy before making the ZIP.
    try:
        (output / 'system.apfs').hardlink_to(image)
    except OSError:
        shutil.copy2(image, output / 'system.apfs')
    shutil.copy2(boot_path, output / boot_path.name)
    shutil.copy2(source_report, output / 'source-integrity.json')
    (output / 'README.txt').write_text('''POCO F4 (munch) iOS 16.5 20F66 experimental RAM boot

This package replaces the previous contents of the dedicated iosroot partition.
It does not target Android boot, system or userdata partitions.
The installer requires root in Android or recovery, exact iosroot -> /dev/block/sda35,
an unmounted target, correct image size/SHA256 and sufficient partition capacity.
After writing, it reads exactly the image length back and verifies SHA256 again.

Copy/extract this directory to /sdcard/Download/munch-ios165 on the phone.
In Termux:
  su
  cd /sdcard/Download/munch-ios165
  sh install.sh --check
  sh install.sh --apply
Do not continue to boot if either step fails.

On Windows, put the included munch-ios165-ramboot.img in Downloads, enter the
phone's ordinary fastboot mode, then use platform-tools fastboot boot with that
small image. No fastboot flash command is used.
The RAM loader reads only the pinned system APFS bytes from iosroot, checks CRC,
and supplies the system and the small helper image as RAM disks to XNU.
The session returns after about ten minutes; no persistent iOS data is configured.

This is the first iOS-userland attempt on this port. The RAM hello program and
early XNU/BSD checkpoints have passed; a working iOS interface is not yet proven.
GPU, touch, Wi-Fi, audio and modem are not enabled for this experiment.
''')
    report = {'version': '16.5', 'build': '20F66', 'system': config['system'],
              'root': config['root'], 'boot_image_sha256': digest(boot_path),
              'boot_image_size': boot_path.stat().st_size,
              'source_report_sha256': digest(source_report), 'hardware_tested': False}
    # Paths describe the builder's local files; omit them from the portable report.
    for key in ('system', 'root'):
        report[key] = {k: v for k, v in report[key].items() if k != 'path'}
    (output / 'package.json').write_text(json.dumps(report, indent=2) + '\n')
    files = sorted(p for p in output.iterdir() if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in files))
    archive = output.parent / 'Install-Munch-iOS165.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as zipout:
        for path in sorted(output.iterdir()):
            zipout.write(path, 'munch-ios165/' + path.name)
    with zipfile.ZipFile(archive) as zipin:
        bad = zipin.testzip()
        if bad:
            raise ValueError('Package ZIP CRC failure: ' + bad)
    print(f'Package verified: {archive} ({archive.stat().st_size} bytes)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--boot', type=Path, required=True)
    parser.add_argument('--source-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.config, args.boot, args.source_report, args.output)
