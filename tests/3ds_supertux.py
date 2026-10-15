#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Optional pinned user-supplied SuperTux CIA native BOOT gate, NOT gameplay.
No download, title assets or native cache are shipped/reused. Takes DRIVER CIA PROBE.
"""
import hashlib
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


def oracle(output, native):
    # Independent test reference only; never linked/called by the CTR runtime.
    import unicorn as U
    from unicorn import arm_const as R
    assert U.__version__ == '2.1.4'
    emu = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    emu.ctl_set_cpu_model(R.UC_CPU_ARM_11MPCORE)
    ex = (output / 'exheader.bin').read_bytes()
    code = (output / 'exefs/.code').read_bytes()
    offset = 0
    for index, at in enumerate((0x10, 0x20, 0x30)):
        base, pages, _ = struct.unpack_from('<III', ex, at)
        initialized = pages * 4096
        length = initialized + (((struct.unpack_from('<I', ex, 0x3c)[0] + 4095) & -4096) if index == 2 else 0)
        emu.mem_map(base, length)
        emu.mem_write(base, code[offset:offset + initialized])
        if index < 2:
            emu.mem_protect(base, length, U.UC_PROT_READ | (U.UC_PROT_EXEC if index == 0 else 0))
        offset += initialized
    stack_size = struct.unpack_from('<I', ex, 0x1c)[0]
    emu.mem_map(0x10000000 - stack_size, stack_size, U.UC_PROT_READ | U.UC_PROT_WRITE)
    emu.mem_map(0x1ff82000, 4096, U.UC_PROT_READ | U.UC_PROT_WRITE)
    regs = [getattr(R, f'UC_ARM_REG_R{i}') for i in range(13)] + [R.UC_ARM_REG_SP, R.UC_ARM_REG_LR, R.UC_ARM_REG_PC]
    emu.reg_write(R.UC_ARM_REG_CPSR, 0x10)
    for reg in regs:
        emu.reg_write(reg, 0)
    emu.reg_write(R.UC_ARM_REG_SP, 0x10000000)
    seen = []

    def interrupt(cpu, number, context):
        pc = cpu.reg_read(R.UC_ARM_REG_PC)
        assert number == 2 and struct.unpack('<I', cpu.mem_read(pc - 4, 4))[0] == 0xef000021
        seen.append(pc)
        cpu.emu_stop()

    emu.hook_add(U.UC_HOOK_INTR, interrupt)
    emu.emu_start(0x100000, 0x4000000, count=1000000)
    assert seen == [0x1deea8]
    values = dict(line.split('=', 1) for line in native.splitlines() if '=' in line)
    for i, reg in enumerate(regs):
        assert emu.reg_read(reg) == int(values[f'r{i}'], 16), f'oracle r{i} mismatch'
    assert emu.reg_read(R.UC_ARM_REG_CPSR) == int(values['cpsr'], 16), 'oracle flags mismatch'
    for name, address, length in [('data', 0x28a000, 0x23000), ('stack', 0x10000000 - stack_size, stack_size), ('tls', 0x1ff82000, 4096)]:
        assert hashlib.sha256(emu.mem_read(address, length)).hexdigest() == values[name], f'oracle {name} mismatch'
    print('Independent ARM11 oracle matches GPRs/CPSR and data/stack/TLS snapshots at first SVC.')


def main():
    assert len(sys.argv) == 4 or (len(sys.argv) == 5 and sys.argv[4] == '--oracle')
    driver, cia, probe = [str(Path(p).resolve()) for p in sys.argv[1:4]]
    digest = hashlib.sha256(Path(cia).read_bytes()).hexdigest()
    assert digest == '943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784', 'Not the pinned supplied SuperTux CIA'
    with tempfile.TemporaryDirectory(prefix='snx-supertux-boot-') as temp:
        output = Path(temp) / 'title'
        result = subprocess.run([driver, '--output', str(output), cia], text=True, capture_output=True, timeout=1500)
        text = result.stdout + result.stderr
        print(text)
        assert result.returncode == 3 and 'syndrome=0x00000021' in result.stderr
        assert 'pc=0x001deea8' in result.stderr and 'blocks=30949' in result.stderr
        assert 'Running ARM11 static AOT' in result.stdout and 'Guest ExitProcess' not in text
        assert 'AddressSanitizer' not in text and 'runtime error:' not in text
        assert hashlib.sha256((output / 'exefs/.code').read_bytes()).hexdigest() == '061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5'
        assert len([p for p in (output / 'romfs').rglob('*') if p.is_file()]) == 834
        assert not list(output.rglob('.snx-*'))
        state = subprocess.run([probe, str(output), 'supertux_boot'], text=True, capture_output=True, timeout=30)
        print(state.stdout, state.stderr)
        assert state.returncode == 0 and 'reached CreateAddressArbiter (unsupported)' in state.stdout
        if '--oracle' in sys.argv[4:]:
            oracle(output, state.stdout)
    print('Pinned SuperTux extraction/static compilation/native startup gate passed; kernel/HLE still blocks gameplay.')


if __name__ == '__main__':
    main()
