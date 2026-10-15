#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Independent CTR metadata/hash reader over synthetic shared NX/CTR host files."""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def check(blob, expected):
    h = struct.unpack_from('<10I', blob)
    assert h[0] == 40 and h[9] <= len(blob)
    seen_dirs, seen_files, contents, dir_names, file_names = set(), set(), {}, {}, {}

    def walk(at, parent, prefix):
        assert at not in seen_dirs and at % 4 == 0 and at + 24 <= h[4]
        seen_dirs.add(at)
        p, sibling, child, file, chain, length = struct.unpack_from('<6I', blob, h[3] + at)
        assert p == parent and at + 24 + length <= h[4] and length % 2 == 0
        name = blob[h[3] + at + 24 : h[3] + at + 24 + length].decode('utf-16le')
        dir_names[at] = (p, name, chain)
        here = prefix + name + '/' if at else ''
        while file != 0xFFFFFFFF:
            assert file not in seen_files and file % 4 == 0 and file + 32 <= h[8]
            seen_files.add(file)
            p, nxt, data, size, chain, length = struct.unpack_from('<IIQQII', blob, h[7] + file)
            assert p == at and file + 32 + length <= h[8] and length % 2 == 0
            name = blob[h[7] + file + 32 : h[7] + file + 32 + length].decode('utf-16le')
            assert h[9] + data + size <= len(blob) and here + name not in contents
            contents[here + name] = blob[h[9] + data : h[9] + data + size]
            file_names[file] = (p, name, chain)
            file = nxt
        while child != 0xFFFFFFFF:
            child = walk(child, at, here)
        return sibling

    assert walk(0, 0, '') == 0xFFFFFFFF
    assert contents == expected
    for offset, size, records in [(h[1], h[2], dir_names), (h[5], h[6], file_names)]:
        found = set()
        for bucket, at in enumerate(struct.unpack_from('<' + 'I' * (size // 4), blob, offset)):
            while at != 0xFFFFFFFF:
                assert at not in found and at in records
                found.add(at)
                parent, name, nxt = records[at]
                digest = parent ^ 123456789
                raw = name.encode('utf-16le')
                for (unit,) in struct.iter_unpack('<H', raw):
                    digest = (((digest >> 5) | (digest << 27)) ^ unit) & 0xFFFFFFFF
                assert digest % (size // 4) == bucket
                at = nxt
        assert found == records.keys()


def main():
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='snx-ctr-fs-') as temp:
        root = Path(temp) / 'title'
        expected = {
            'hello.txt': b'hello',
            'dir/é😀.bin': b'DATA',
            'empty': b'',
            'dir/sub/big.bin': bytes(i % 251 for i in range(8201)),
        }
        for name, contents in expected.items():
            path = root / 'romfs' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        for mode in ['shared', *(['fallback'] if os.name == 'posix' else [])]:
            output = Path(temp) / f'{mode}.bin'
            subprocess.run([probe, str(root), str(output), mode], check=True)
            check(output.read_bytes(), expected)
        missing = Path(temp) / 'missing-romfs'
        missing.mkdir()
        subprocess.run([probe, str(missing), str(Path(temp) / 'unused'), 'absent'], check=True)
    print('CTR metadata, UTF-16/surrogates, hash chains, contents, shared and fallback IO independently checked')


if __name__ == '__main__':
    main()
