<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# SuperTux integration target

The user-supplied SuperTux homebrew is now the primary end-to-end integration
fixture. **Current milestone: real native startup reaches its first syscall,
not `main`, rendering, audio or gameplay.** No game-specific execution shortcuts,
patched guest instructions, interpreter or runtime-JIT fallback are used.

## Pinned input (not distributed)

The supplied archive `d6nm2s3.zip` contains `Supertux.cia`,
`Supertux/Supertux.3dsx` and `Supertux/Supertux.smdh`. This work uses the CIA;
it does not add a 3DSX loader. Keep the input outside the source repository.
No game binaries/assets or derived native modules are included in the patch.

SHA-256:

```text
82d2860b06325526c2cdf95c0dc4aa1364668a3a2509eea6f1a74c58a4d6eb01  d6nm2s3.zip
943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784  Supertux.cia
061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5  expanded exefs/.code
```

The decrypted application CIA has title ID `0004000000998200`. Extraction
materializes all present content, metadata and **834 RomFS files**, including
`images/`, `levels/`, `music/` and `sounds/`. These are not guest FS services yet.

ExHeader layout:

- Entry/text: `0x00100000`, 340 pages, 1,389,676 declared text bytes.
- Read-only data: `0x00254000`, 54 pages, 217,776 bytes.
- Initialized data: `0x0028a000`, 4 pages.
- Data byte count: `0x21ac0`, **including BSS** `0x1e36c`; initialized bytes
  are `0x3754`, which fit those 4 pages. The old extractor incorrectly rejected
  this maker's representation. Bounded normalization now accepts it without
  requiring/materializing BSS as input code bytes.
- Stack: `0x40000` bytes. Expanded page-packed code: 1,630,208 bytes.

## Reproduce

Use a native 64-bit Linux build with Clang and an LLVM SDK (19.1.7 was tested):

```sh
cmake -S . -B build/ctr-aot \
  -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF \
  -DSNX_3DS_AOT=ON -DSNX_TEST_ARM_AOT=ON \
  -DSNX_TEST_3DS_SUPERTUX=/absolute/path/to/Supertux.cia
cmake --build build/ctr-aot -j4
ctest --test-dir build/ctr-aot -R 3ds_supertux_bootstrap -V
```

Select a non-system SDK with `SNX_LLVM_CONFIG`. With Python
`unicorn==2.1.4` installed, `SNX_TEST_ARM_DIFF=ON` additionally runs an independent
ARM11 MPCore reference through the same startup. It compares all 16 GPRs/CPSR
and SHA-256 snapshots of complete mapped data/BSS, stack and TLS at the first
SVC. Unicorn is **test-only**, never a runtime dependency/fallback.

For a manual run, choose a **new** output directory:

```sh
build/ctr-aot/scarletnx-3ds --output work/supertux-001 /path/to/Supertux.cia
```

This currently returns **status 3**, not success. Representative milestones:

```text
Discovered ARM11 static coverage: 48488 blocks (conservative CFG)
Compiled ARM11 static module: 48488 blocks in 380 objects
Running ARM11 static AOT
unsupported guest operation or missing static entry: pc=0x001deea8 exit=4 syndrome=0x00000021 blocks=30949
```

The static graph visits 205,783 ARM/Thumb instructions (including conservative
candidates). It does not certify coverage of every runtime target. Compilation
uses bounded LLVM chunks, then native linking, **all before execution**.
Measured locally: roughly 2.5 minutes for the complete bootstrap test; the native
output occupies about 100 MiB (the linked module about 49 MiB). Existing output
is still refused; validated cache/resume is not implemented. Runtime/kernel
changes must not start silently trusting old native files as a shortcut.

## Observed execution and next gates

Native startup branches from `0x100000` to `0x100020`, clears BSS in the guest,
enters the homebrew runtime initialization path and calls the first syscall at
`0x1deea4`. The next PC is `0x1deea8`; `r0=0x2a4744`,
`sp=0x0ffffff4`, `lr=0x1deca0`. SVC `0x21` is **CreateAddressArbiter**.
It remains explicitly unsupported; no fabricated handle/success is returned.

Prioritize subsequent increments against this fixture:

1. Minimal real kernel object/handle and address-arbiter behavior, then config
   pages, system/memory queries and checked heap/linear-memory setup required by
   homebrew initialization. Startup disassembly indicates these will matter;
   execution has **not** passed the first syscall yet.
2. Checked architectural exclusives/reservations and the required ARM11 VFPv2
   semantics, with independent tests and explicit profile gating. Do not treat
   A57 FP helpers or a flat-memory exclusive implementation as ARM11 support.
3. CTR SVC/IPC services and a SwIPC-derived generator using CTR wire layouts,
   especially service-manager/APT/FS/GSP/HID paths. Serve resources from the
   materialized RomFS tree, not a mount of the original CIA.
4. Progressively gate `main`, service initialization, first frame, menu/input,
   audio and actual gameplay. These need separate implementation/validation;
   reaching this syscall does not imply any of them work.

Continue to preserve shared A57 defaults/ABI. Improve conservative indirect-target
coverage and add safe cache/resume in parallel, without automatic runtime code
generation. See [3DS port](3DS.md) for supported ISA, memory and native trust
contracts. All source changes here are project code; the supplied title and
assets remain external and retain their own licenses.
