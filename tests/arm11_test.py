#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Checked A32 integer AOT vs Unicorn's ARM11 MPCore model, at O0/O2.

An independent ISA oracle, unlike arm_checked_matrix.py. Includes serialized
ARMv6K exclusive sequences. A separate C gate tests the real CTR bus against
aliases, external writes, ABA, permissions and mapping changes. Thumb legacy
BL halves have separate C golden witnesses.
"""

import argparse
import ctypes as C
import pathlib
import tempfile

import arm_test as A
import arm_checked_matrix as M


def main():
    import unicorn as U
    from unicorn import arm_const as R

    assert U.__version__ == '2.1.4', 'Use the pinned unicorn==2.1.4'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emitter', required=True)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    A.catalogue()
    regs = [getattr(R, f'UC_ARM_REG_R{i}') for i in range(13)] + [R.UC_ARM_REG_SP, R.UC_ARM_REG_LR, R.UC_ARM_REG_PC]
    emu = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    emu.ctl_set_cpu_model(R.UC_CPU_ARM_11MPCORE)
    emu.mem_map(0, A.SIZE)
    with tempfile.TemporaryDirectory(prefix='snx-arm11-diff-') as tmp:
        root = pathlib.Path(tmp)
        (root / 'words.txt').write_text(
            ''.join(f'{pc:x} ' + ' '.join(f'{w:08x}' for w in words) + '\n' for pc, words in zip(A.PCS, A.PROGRAMS))
        )
        for opt in ('0', '2'):
            functions = M.compile_blocks(args, root, 'arm11', 'checked', opt)
            total = invalid = 0
            for index, group in A.CASES:
                words = A.PROGRAMS[index]
                for seed in range(32):
                    cpu, memory = A.state(words, seed, group)
                    emu.mem_write(0, bytes(memory))
                    emu.ctl_remove_cache(A.PC, A.PC + 4 * len(words))
                    emu.reg_write(R.UC_ARM_REG_CPSR, cpu.cpsr)
                    for reg, value in zip(regs, cpu.r):
                        emu.reg_write(reg, value)
                    error = None
                    try:
                        emu.emu_start(A.PC, A.SIZE, count=len(words))
                    except U.UcError as exc:
                        error = exc
                    faults = []
                    epoch, reservation = [1], [0, 0, 0]

                    @M.Access
                    def access(context, operation, address, size, data):
                        if operation not in (1, 2, 3, 0x101, 0x102, 0x201, 0x202) or not 1 <= size <= 64:
                            faults.append((operation, size))
                            return 1
                        if address + size > A.SIZE:
                            return 1
                        if operation == 0x202:
                            valid = reservation == [epoch[0], address, size]
                            reservation[:] = [0, 0, 0]
                            if not valid:
                                return 2
                        if operation & 255 == 2:
                            C.memmove(C.addressof(memory) + address, data, size)
                            epoch[0] += 1
                        else:
                            C.memmove(data, C.addressof(memory) + address, size)
                            if operation == 0x201:
                                reservation[:] = [epoch[0], address, size]
                        return 0

                    state = M.Checked()
                    C.memmove(C.byref(state.cpu), C.byref(cpu), C.sizeof(cpu))
                    state.abi, state.access = 1, access
                    state.cpu.mem = C.cast(1, C.POINTER(C.c_uint8))
                    state.cpu.mem_size = 2**64 - 1
                    functions[index](C.byref(state.cpu))
                    actual = state.cpu
                    label = f'O{opt} {words} {group} seed={seed}'
                    assert not faults, (label, faults)
                    if error:
                        assert error.errno == U.UC_ERR_INSN_INVALID and actual.exit == 1, (label, error, actual.exit)
                        assert actual.r[15] == A.PC, (label, 'undefined PC')
                        invalid += 1
                        continue
                    assert actual.exit == 0, (label, actual.exit)
                    expected = [emu.reg_read(reg) for reg in regs] + [emu.reg_read(R.UC_ARM_REG_CPSR)]
                    assert list(actual.r) + [actual.cpsr] == expected, (label, list(actual.r), expected)
                    assert bytes(memory) == bytes(emu.mem_read(0, A.SIZE)), (label, 'memory mismatch')
                    total += 1
            print(f'ARM11 checked O{opt}: {total} exact comparisons, {invalid} matched undefined encodings', flush=True)


if __name__ == '__main__':
    main()
