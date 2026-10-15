#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Asset-free CIA -> extracted tree -> native ELF -> checked ARM/Thumb execution."""
import ctypes as C
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from ctr_fixtures import cia, ncch, put


def application(words=None, raw=None, text_size=None, ro=None):
    if raw is None:
        raw = struct.pack('<' + 'I' * len(words), *words)
    size = text_size if text_size is not None else len(raw)
    pages = (size + 4095) // 4096
    code = bytearray((pages + 2) * 4096)
    code[:len(raw)] = raw
    at = pages * 4096
    code[at:at + 16] = b'READONLY-DATA!!!'
    if ro is not None:
        code[at:at + len(ro)] = ro
    code[at + 4096:at + 4100] = b'DATA'

    def header(ex):
        put(ex, 0x14, '<II', pages, size)
        if size > 0x100000:
            put(ex, 0x20, '<I', 0x2000000)
            put(ex, 0x30, '<I', 0x3000000)
    return cia(application=ncch(code_blob=bytes(code), patch_exheader=header))


def descriptor_offset(blob):
    # Locate the exported descriptor in a little-endian ELF64 test artifact.
    assert blob[:6] == b'\x7fELF\x02\x01'
    eh = struct.unpack_from('<16sHHIQQQIHHHHHH', blob)
    sections = [struct.unpack_from('<IIQQQQIIQQ', blob, eh[6] + i * eh[11]) for i in range(eh[12])]
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        names = blob[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            symbol = struct.unpack_from('<IBBHQQ', blob, at)
            name = names[symbol[0]:].split(b'\0', 1)[0]
            if name == b'snx_ctr_module_v1':
                storage = sections[symbol[3]]
                return storage[4] + symbol[4] - storage[3]
    raise AssertionError('descriptor not exported')


class Descriptor(C.Structure):
    _fields_ = [('magic', C.c_uint64), ('fields', C.c_uint32 * 14), ('identity', C.c_uint8 * 32),
                ('arm', C.c_void_p), ('thumb', C.c_void_p)]


def main():
    driver, compiler, probe = [str(Path(p).resolve()) for p in sys.argv[1:4]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='snx-ctr-aot-') as temp:
        root = Path(temp)

        def run(blob, status=0, message='Guest ExitProcess', extra=()):
            nonlocal cases
            cases += 1
            source, output = root / f'input {cases};literal.cia', root / f'out {cases};literal'
            source.write_bytes(blob)
            p = subprocess.run([driver, *extra, '--output', str(output), str(source)],
                               text=True, capture_output=True, timeout=90)
            text = p.stdout + p.stderr
            assert p.returncode == status, (cases, p.returncode, text)
            assert message in text and 'AddressSanitizer' not in text and 'runtime error:' not in text, (cases, text)
            assert not list(root.rglob('.snx-*')), (cases, 'private staging leaked')
            if status:
                assert 'Guest ExitProcess' not in text
            return output, text

        basic, text = run(application([0xe3a0002a, 0xef000003]), message='Guest ExitProcess: r0=42 blocks=2')
        assert text.index('Extracting ') < text.index('Recompiling ARM11') < text.index('Running ARM11')
        assert (basic / 'romfs/hello.txt').read_bytes() == b'hello'
        assert (basic / 'contents/00002222.app').is_file()
        assert (basic / 'native/code.o').is_file() and (basic / 'native/code.so').is_file()
        subprocess.run([probe, str(basic), 'basic'], check=True, capture_output=True)
        # The output is not a blind cache hit, nor is existing extraction reused.
        before = (basic / 'native/code.so').read_bytes()
        p = subprocess.run([compiler, str(basic)], text=True, capture_output=True, timeout=20)
        assert p.returncode == 1 and 'already exists' in p.stderr
        assert (basic / 'native/code.so').read_bytes() == before
        source = root / 'repeat.cia'; source.write_bytes(application([0xe3a0002a, 0xef000003]))
        p = subprocess.run([driver, '--output', str(basic), str(source)], text=True, capture_output=True, timeout=20)
        assert p.returncode == 1 and 'output already exists' in p.stderr and 'Recompiling' not in p.stdout
        run(application([0xe3a0002a, 0xef000003]), message='r0=42', extra=('--model', 'new'))
        # Data+BSS, read-only mapping, TLS, stack, indirect BLX to Thumb and BX return.
        words = [0xe3a0002a, 0xe59f103c, 0xe5810000, 0xe2811a01, 0xe5810000,
                 0xe59f2030, 0xe5922000, 0xee1d3f70, 0xe5830000, 0xe92d4001,
                 0xe3a00000, 0xe8bd4001, 0xe59f4018, 0xe12fff34, 0xef000003,
                 0xe1a00000, 0x4770202b, 0, 0x300000, 0x200000, 0x100041]
        workload, _ = run(application(words), message='Guest ExitProcess: r0=43 blocks=17')
        subprocess.run([probe, str(workload), 'workload'], check=True, capture_output=True)
        # Legacy BL halves execute as separate precompiled Thumb entries.
        legacy = struct.pack('<II6H', 0xe28f0001, 0xe12fff10, 0xf000, 0xf802, 0xdf03, 0x46c0, 0x202c, 0x4770)
        run(application(raw=legacy), message='Guest ExitProcess: r0=44 blocks=7')
        run(application([0xeafffffe]), 3, 'block budget exhausted', extra=('--max-blocks', '3'))
        run(application([0xef000032]), 3, 'unsupported guest operation')
        run(application([0xe1900f9f]), 3, 'unsupported guest operation')  # LDREX, no flat fallback
        run(application([0xee300a01]), 3, 'unsupported guest operation')  # VFPv2
        run(application([0xe3010234]), 1, 'guest fault')  # MOVW is not ARM11
        run(application([0xe3a01000, 0xe5910000]), 1, 'guest fault')  # unmapped data
        run(application([0xe59f1004, 0xe5810000, 0xef000003, 0x200000]), 1, 'guest fault')  # RO store
        run(application([0xe59ff000, 0xe1a00000, 0x200000]), 1, 'exit=11')  # non-X branch
        run(application([0xea000002]), 3, 'missing static entry')  # RX page padding, outside declared text
        # Exhaustive discovery includes the last valid ARM word at the cap.
        maximum = bytearray(4096)
        put(maximum, 0, '<I', 0xea0003fd)
        put(maximum, 4092, '<I', 0xef000003)
        run(application(raw=bytes(maximum)), message='Guest ExitProcess: r0=0 blocks=2')
        run(application([0xef000003], text_size=4100), message='Guest ExitProcess: r0=0 blocks=1')
        # Sparse RO function pointer -> indirect BLX -> Thumb, beyond the old cap.
        sparse = bytearray(0x2010)
        put(sparse, 0, '<6I', 0xe59f300c, 0xe5933000, 0xe12fff33, 0xef000003, 0xe1a00000, 0x200000)
        put(sparse, 0x2000, '<HH', 0x202f, 0x4770)
        run(application(raw=sparse, ro=struct.pack('<I', 0x102001)), message='Guest ExitProcess: r0=47 blocks=3')
        # More than 128 block entries forces multiple independent LLVM chunks.
        chained = bytearray(0x1800)
        for i in range(129):
            put(chained, i * 32, '<I', 0xea000006)
        put(chained, 129 * 32, '<II', 0xe3a00031, 0xef000003)
        chain, _ = run(application(raw=chained), message='Guest ExitProcess: r0=49 blocks=130')
        assert len(list((chain / 'native').glob('block-*.o'))) >= 2
        # Unproven dynamic targets do not trigger discovery/compilation at run time.
        uncovered = bytearray(0x1800)
        put(uncovered, 0, '<3I', 0xe3a01601, 0xe2811c01, 0xe12fff11)
        put(uncovered, 0x100, '<I', 0xef000003)
        run(application(raw=uncovered), 3, 'missing static entry')
        too_large, _ = run(application([0xef000003], text_size=16 * 1024 * 1024 + 4), 3, 'compiler failed')
        assert not (too_large / 'native').exists()
        extracted, text = run(application([0xe3a0002a, 0xef000003]), message='Extracted', extra=('-x',))
        assert 'Running ARM11' not in text and not (extracted / 'native').exists()
        # Compiler input is revalidated from raw files, never trusted manifest fields.
        (extracted / 'exefs/.code').write_bytes(b'truncated')
        p = subprocess.run([compiler, str(extracted)], text=True, capture_output=True, timeout=20)
        assert p.returncode == 1 and 'bounded expanded .code' in p.stderr and not (extracted / 'native').exists()
        assert not list(root.rglob('.snx-*'))
        # Real dlopen/dlsym ABI validation against freshly generated, trusted ELF copies.
        original = (basic / 'native/code.so').read_bytes()
        offset = descriptor_offset(original)
        assert C.sizeof(Descriptor) == 112
        mutations = [offset, offset + Descriptor.identity.offset + 31]
        mutations += [offset + Descriptor.fields.offset + i * 4 for i in range(14)]
        for i, at in enumerate(mutations):
            data = bytearray(original); data[at] ^= 1
            path = root / f'bad-module-{i}.so'; path.write_bytes(data)
            subprocess.run([probe, str(basic), 'reject', str(path)], check=True, capture_output=True)
        # Null function-table pointers are unit-tested in C: patching the ELF
        # pointer bytes alone would be undone by the dynamic loader's RELA.
        # A different expanded image identity is rejected by the same module.
        raw = bytearray((basic / 'exefs/.code').read_bytes()); raw[0] ^= 1
        (basic / 'exefs/.code').write_bytes(raw)
        subprocess.run([probe, str(basic), 'reject'], check=True, capture_output=True)
        # Deployment failure is explicit; the runtime itself needs no LLVM DLL.
        isolated = root / 'driver only'; isolated.mkdir()
        copied_driver = isolated / 'scarletnx-3ds'; shutil.copy2(driver, copied_driver)
        source.write_bytes(application([0xe3a0002a, 0xef000003]))
        missing_output = root / 'missing compiler output'
        p = subprocess.run([str(copied_driver), '--output', str(missing_output), str(source)],
                           text=True, capture_output=True, timeout=20)
        assert p.returncode == 3 and 'missing adjacent' in p.stderr and 'Running ARM11' not in p.stdout
        assert not (missing_output / 'native').exists()
        subprocess.run([probe, str(workload), 'reject', str(root / 'missing.so')], check=True, capture_output=True)
        readelf = shutil.which('readelf')
        if readelf:
            deps = subprocess.check_output([readelf, '-d', str(workload / 'native/code.so')], text=True)
            assert 'LLVM' not in deps and 'scarletnx' not in deps
            deps = subprocess.check_output([readelf, '-d', driver], text=True)
            assert 'LLVM' not in deps
    print(f'CTR end-to-end static AOT: {cases} pipelines, native ABI mutations and image identity checks passed')


if __name__ == '__main__':
    main()
