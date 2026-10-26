#!/usr/bin/env python3
"""Resume upstream iOS 16.5 downloads using bounded, validated HTTP ranges."""
import importlib.util
import os
import sys
import urllib.request
from pathlib import Path

source = Path(__file__).resolve().parents[3] / 'sysimage/fetch-from-ipsw.py'
spec = importlib.util.spec_from_file_location('ipsw_fetch', source)
upstream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upstream)


def checked_fetch(start, end):
    request = urllib.request.Request(upstream.URL, headers={
        'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'})
    response = urllib.request.urlopen(request, timeout=20)
    expected = f'bytes {start}-{end}/{upstream.SIZE}'
    if response.status != 206 or response.headers.get('Content-Range') != expected:
        response.close()
        raise ValueError('Server did not return the requested IPSW byte range')
    return response


original_download = upstream.download

def bounded_download(data, method, size, output):
    if method != 0:
        return original_download(data, method, size, output)
    have = os.path.getsize(output) if os.path.exists(output) else 0
    if have > size:
        raise ValueError('Existing partial file is larger than the IPSW member')
    with open(output, 'ab') as out:
        while have < size:
            stop = min(have + 4 * 1024 * 1024, size)
            with checked_fetch(data + have, data + stop - 1) as response:
                remaining = stop - have
                while remaining:
                    chunk = response.read(min(262144, remaining))
                    if not chunk:
                        raise OSError('Truncated IPSW range')
                    out.write(chunk)
                    have += len(chunk)
                    remaining -= len(chunk)
                out.flush()
            print(f'{Path(output).name}: {have}/{size} bytes ({have * 100 / size:.1f}%)', flush=True)
    return have


upstream.fetch = checked_fetch
upstream.download = bounded_download
if __name__ == '__main__':
    upstream.main()
