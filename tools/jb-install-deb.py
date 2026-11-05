#!/usr/bin/env python3
"""Install a downloaded Debian package into a Procursus prefix.

The normal preinstall path consumes the Procursus Packages index. This helper is for
fixed external packages whose archives are fetched directly from their publishers.
It accepts rootless packages (./var/jb/...) and legacy app packages (./Applications/...)
and records the package in dpkg's status database without running maintainer scripts.
"""
import argparse
import hashlib
import io
import lzma
import os
import re
import subprocess
import tarfile


def ar_members(path):
    result = subprocess.run(['ar', 't', path], check=True, capture_output=True, text=True)
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def ar_data(path, member):
    return subprocess.run(['ar', 'p', path, member], check=True, capture_output=True).stdout


def unpack_archive(data, member):
    if member.endswith('.zst'):
        return subprocess.run(['zstd', '-dc'], input=data, check=True, capture_output=True).stdout
    if member.endswith('.lzma'):
        return lzma.decompress(data, format=lzma.FORMAT_ALONE)
    return data


def archive_member(path, prefix):
    member = next((name for name in ar_members(path) if name.startswith(prefix)), None)
    if member is None:
        raise RuntimeError('%s: no %s member' % (path, prefix))
    return unpack_archive(ar_data(path, member), member)


def control_text(path):
    data = archive_member(path, 'control.tar')
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        member = next((item for item in archive.getmembers() if os.path.basename(item.name) == 'control'), None)
        if member is None:
            raise RuntimeError('%s: control archive has no control file' % path)
        return archive.extractfile(member).read().decode('utf-8', 'replace')


def field(stanza, name):
    match = re.search(r'(?m)^%s: (.*)$' % re.escape(name), stanza)
    return match.group(1).strip() if match else None


def safe_path(path):
    normalized = os.path.normpath(path.lstrip('./'))
    return normalized not in ('', '.') and not normalized.startswith('../') and '/..' not in normalized


def extract_data(deb, prefix, legacy_app):
    data = archive_member(deb, 'data.tar')
    installed = []
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for member in archive.getmembers():
            source = os.path.normpath(member.name.lstrip('./'))
            if not safe_path(source):
                continue
            if source.startswith('var/jb/'):
                target = source
            elif legacy_app and source.startswith('Applications/'):
                target = 'var/jb/' + source
            else:
                continue
            member.name = './' + target
            archive.extract(member, prefix, set_attrs=True, filter='fully_trusted')
            installed.append('/' + target)
    return installed


def rewrite_control(control, architecture):
    lines = [line for line in control.splitlines()
             if not line.startswith(('Filename:', 'Size:', 'MD5sum:', 'MD5Sum:', 'SHA1:', 'SHA256:',
                                     'SHA512:', 'Description-md5:'))]
    if architecture:
        lines = [line for line in lines if not line.startswith('Architecture:')]
        insert_at = next((i + 1 for i, line in enumerate(lines) if line.startswith('Version:')), len(lines))
        lines.insert(insert_at, 'Architecture: ' + architecture)
    return '\n'.join(lines).rstrip()


def write_status(prefix, control, architecture, installed):
    package = field(control, 'Package')
    version = field(control, 'Version')
    if not package or not version:
        raise RuntimeError('package control is missing Package or Version')
    status_path = os.path.join(prefix, 'var/jb/Library/dpkg/status')
    os.makedirs(os.path.dirname(status_path), exist_ok=True)
    old = open(status_path, encoding='utf-8', errors='replace').read() if os.path.exists(status_path) else ''
    stanzas = [stanza for stanza in old.split('\n\n') if stanza.strip()]
    entry = rewrite_control(control, architecture) + '\nStatus: install ok installed'
    replaced = False
    for index, stanza in enumerate(stanzas):
        if field(stanza, 'Package') == package:
            stanzas[index] = entry
            replaced = True
            break
    if not replaced:
        stanzas.append(entry)
    with open(status_path, 'w', encoding='utf-8') as output:
        output.write('\n\n'.join(stanzas) + '\n\n')
    info_dir = os.path.join(prefix, 'var/jb/Library/dpkg/info')
    os.makedirs(info_dir, exist_ok=True)
    with open(os.path.join(info_dir, package + '.list'), 'w', encoding='utf-8') as output:
        output.write('\n'.join(installed) + '\n')
    return package, version


def verify_sha256(path, expected):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    actual = digest.hexdigest()
    if actual != expected:
        raise RuntimeError('%s: SHA-256 mismatch: expected %s, got %s' % (path, expected, actual))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', required=True, help='directory containing var/jb')
    parser.add_argument('--deb', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--architecture', default=None,
                        help='override the package Architecture field')
    parser.add_argument('--legacy-app', action='store_true',
                        help='map ./Applications into var/jb/Applications')
    parser.add_argument('--chmod', action='append', default=[], metavar='PATH=MODE',
                        help='chmod a path below the prefix after extraction')
    args = parser.parse_args()

    verify_sha256(args.deb, args.sha256)
    installed = extract_data(args.deb, args.prefix, args.legacy_app)
    for change in args.chmod:
        path, mode = change.split('=', 1)
        os.chmod(os.path.join(args.prefix, path), int(mode, 8))
    package, version = write_status(args.prefix, control_text(args.deb), args.architecture, installed)
    print('jb-install-deb.py: %s %s installed' % (package, version))


if __name__ == '__main__':
    main()
