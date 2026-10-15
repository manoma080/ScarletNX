#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Optional pinned user-supplied SuperTux CIA native main-entry gate, NOT gameplay.
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
        length = initialized + (((struct.unpack_from('<I', ex, 0x3C)[0] + 4095) & -4096) if index == 2 else 0)
        emu.mem_map(base, length)
        emu.mem_write(base, code[offset : offset + initialized])
        if index < 2:
            emu.mem_protect(base, length, U.UC_PROT_READ | (U.UC_PROT_EXEC if index == 0 else 0))
        offset += initialized
    stack_size = struct.unpack_from('<I', ex, 0x1C)[0]
    emu.mem_map(0x10000000 - stack_size, stack_size, U.UC_PROT_READ | U.UC_PROT_WRITE)
    emu.mem_map(0x1FF82000, 4096, U.UC_PROT_READ | U.UC_PROT_WRITE)
    regs = [getattr(R, f'UC_ARM_REG_R{i}') for i in range(13)] + [R.UC_ARM_REG_SP, R.UC_ARM_REG_LR, R.UC_ARM_REG_PC]
    emu.reg_write(R.UC_ARM_REG_CPSR, 0x10)
    for reg in regs:
        emu.reg_write(reg, 0)
    emu.reg_write(R.UC_ARM_REG_SP, 0x10000000)
    emu.reg_write(R.UC_ARM_REG_C13_C0_3, 0x1FF82000)
    seen = []

    def interrupt(cpu, number, context):
        pc = cpu.reg_read(R.UC_ARM_REG_PC)
        assert number == 2 and struct.unpack('<I', cpu.mem_read(pc - 4, 4))[0] == 0xEF000021
        seen.append(pc)
        cpu.emu_stop()

    hook = emu.hook_add(U.UC_HOOK_INTR, interrupt)
    emu.emu_start(0x100000, 0x4000000, count=1000000)
    assert seen == [0x1DEEA8]
    values = dict(line.split('=', 1) for line in native.splitlines() if '=' in line)
    for i, reg in enumerate(regs):
        assert emu.reg_read(reg) == int(values[f'r{i}'], 16), f'oracle r{i} mismatch'
    assert emu.reg_read(R.UC_ARM_REG_CPSR) == int(values['cpsr'], 16), 'oracle flags mismatch'
    for name, address, length in [
        ('data', 0x28A000, 0x23000),
        ('stack', 0x10000000 - stack_size, stack_size),
        ('tls', 0x1FF82000, 4096),
    ]:
        assert hashlib.sha256(emu.mem_read(address, length)).hexdigest() == values[name], f'oracle {name} mismatch'
    print('Independent ARM11 oracle matches GPRs/CPSR and data/stack/TLS snapshots at first SVC.')
    # CPU cross-check after a TEST-SCRIPTED HLE transcript. This does not
    # independently validate the selected kernel personality or heap policy.
    emu.hook_del(hook)
    config = bytearray(4096)
    config[2] = config[0x62] = 0x3A
    config[3] = config[0x63] = 2
    config[0x14] = config[0x16] = 1
    config[0x10] = config[0x64] = 2
    struct.pack_into('<H', config, 0x18, 0xF450)
    struct.pack_into('<H', config, 0x68, 0xF450)
    struct.pack_into('<III', config, 0x40, 0x4000000, 0x2C00000, 0x1400000)
    emu.mem_map(0x1FF80000, 4096)
    emu.mem_write(0x1FF80000, bytes(config))
    emu.mem_protect(0x1FF80000, 4096, U.UC_PROT_READ)
    emu.reg_write(R.UC_ARM_REG_R0, 0)
    emu.reg_write(R.UC_ARM_REG_R1, 0x201)
    transcript = []

    def modeled_kernel(cpu, number, context):
        pc = cpu.reg_read(R.UC_ARM_REG_PC)
        assert number == 2
        call = struct.unpack('<I', cpu.mem_read(pc - 4, 4))[0] & 0xFFFFFF
        args = [cpu.reg_read(reg) for reg in regs[:6]]
        transcript.append(call)
        if call == 0x2A:
            assert args[1:3] == [0, 1]
            cpu.reg_write(R.UC_ARM_REG_R1, 0x1EE000)
            cpu.reg_write(R.UC_ARM_REG_R2, 0)
        elif call == 1:
            assert args[0] in (3, 0x10003)
            assert args[1:5] == [0x08000000 if args[0] == 3 else 0, 0, 0x1F09000, 3]
            address = 0x08000000 if args[0] == 3 else 0x160F7000
            cpu.mem_map(address, 0x1F09000, U.UC_PROT_READ | U.UC_PROT_WRITE)
            cpu.reg_write(R.UC_ARM_REG_R1, address)
        else:
            assert call == 0x2D and bytes(cpu.mem_read(args[1], 5)) == b'srv:\0'
            cpu.emu_stop()
            return
        cpu.reg_write(R.UC_ARM_REG_R0, 0)

    hook = emu.hook_add(U.UC_HOOK_INTR, modeled_kernel)
    emu.emu_start(0x1DEEA8, 0x4000000, count=1000000)
    assert transcript == [0x2A, 1, 1, 0x2D]
    for i, reg in enumerate(regs):
        assert emu.reg_read(reg) == int(values[f'kernel_r{i}'], 16), f'post-HLE oracle r{i} mismatch'
    assert emu.reg_read(R.UC_ARM_REG_CPSR) == int(values['kernel_cpsr'], 16)
    for name, address, length in [
        ('data', 0x28A000, 0x23000),
        ('stack', 0x10000000 - stack_size, stack_size),
        ('tls', 0x1FF82000, 4096),
        ('heap', 0x08000000, 0x1F09000),
        ('linear', 0x160F7000, 0x1F09000),
    ]:
        assert hashlib.sha256(emu.mem_read(address, length)).hexdigest() == values[f'kernel_{name}'], name
    print('Independent ARM11 oracle matches startup locks/GPRs/CPSR and RAM after a test-scripted HLE transcript.')
    emu.hook_del(hook)
    emu.reg_write(R.UC_ARM_REG_R0, 0)
    emu.reg_write(R.UC_ARM_REG_R1, 0x202)
    ipc_transcript = []

    def modeled_ipc(cpu, number, context):
        pc = cpu.reg_read(R.UC_ARM_REG_PC)
        assert number == 2 and struct.unpack('<I', cpu.mem_read(pc - 4, 4))[0] == 0xEF000032
        assert cpu.reg_read(R.UC_ARM_REG_R0) == 0x202
        words = struct.unpack('<64I', cpu.mem_read(0x1FF82080, 256))
        ipc_transcript.append(words[0])
        if words[0] == 0x00010002:
            assert words[1] == 0x20
            reply = struct.pack('<64I', 0x00010040, *([0] * 63))
            cpu.mem_write(0x1FF82080, reply)
            cpu.reg_write(R.UC_ARM_REG_R0, 0)
        else:
            assert words[:5] == (0x00050100, 0x3A545041, 0x53, 5, 0)
            cpu.emu_stop()

    emu.hook_add(U.UC_HOOK_INTR, modeled_ipc)
    emu.emu_start(0x1DEF40, 0x4000000, count=1000000)
    assert ipc_transcript == [0x00010002, 0x00050100]
    for i, reg in enumerate(regs):
        assert emu.reg_read(reg) == int(values[f'ipc_r{i}'], 16), f'IPC transcript oracle r{i} mismatch'
    assert emu.reg_read(R.UC_ARM_REG_CPSR) == int(values['ipc_cpsr'], 16)
    for name, address, length in [
        ('data', 0x28A000, 0x23000),
        ('stack', 0x10000000 - stack_size, stack_size),
        ('tls', 0x1FF82000, 4096),
        ('heap', 0x08000000, 0x1F09000),
        ('linear', 0x160F7000, 0x1F09000),
    ]:
        assert hashlib.sha256(emu.mem_read(address, length)).hexdigest() == values[f'ipc_{name}'], name
    print(
        'Independent CPU reference matches all GPRs/CPSR and RAM before APT:S lookup handling under a scripted IPC transcript; not an independent IPC-policy oracle.'
    )


def main():
    assert len(sys.argv) == 4 or (len(sys.argv) == 5 and sys.argv[4] == '--oracle')
    driver, cia, probe = [str(Path(p).resolve()) for p in sys.argv[1:4]]
    digest = hashlib.sha256(Path(cia).read_bytes()).hexdigest()
    assert digest == '943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784', (
        'Not the pinned supplied SuperTux CIA'
    )
    with tempfile.TemporaryDirectory(prefix='snx-supertux-boot-') as temp:
        output = Path(temp) / 'title'
        result = subprocess.run([driver, '--output', str(output), cia], text=True, capture_output=True, timeout=1500)
        text = result.stdout + result.stderr
        print(text)
        assert result.returncode == 3 and 'syndrome=0x00000032' in result.stderr
        assert 'pc=0x001def50' in result.stderr and 'blocks=50629' in result.stderr
        assert 'unsupported guest operation' in result.stderr
        assert 'svcs=61' in result.stderr and 'ipc_header=0x080d0144' in result.stderr
        assert 'Running ARM11 static AOT' in result.stdout and 'Guest ExitProcess' not in text
        assert 'AddressSanitizer' not in text and 'runtime error:' not in text
        assert (
            hashlib.sha256((output / 'exefs/.code').read_bytes()).hexdigest()
            == '061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5'
        )
        assert len([p for p in (output / 'romfs').rglob('*') if p.is_file()]) == 834
        assert not list(output.rglob('.snx-*'))
        state = subprocess.run([probe, str(output), 'supertux_boot'], text=True, capture_output=True, timeout=30)
        print(state.stdout, state.stderr)
        assert state.returncode == 0 and 'CPU gate before APT:S lookup handling' in state.stdout
        assert 'SuperTux main entry reached: pc=0x00167d88 blocks=50489' in state.stdout
        if '--oracle' in sys.argv[4:]:
            oracle(output, state.stdout)
    print(
        'Pinned SuperTux executes scalar VFP, opens RomFS, then stops at archive timestamp control; no graphics/audio/gameplay.'
    )


if __name__ == '__main__':
    main()
