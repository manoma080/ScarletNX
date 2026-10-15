<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# SuperTux integration target

The user-supplied SuperTux homebrew is the primary end-to-end fixture.
**Current milestone: real native startup creates an arbiter, allocates heap and
linear memory, executes startup locks, connects/registers `srv:`, then blocks
waiting for the absent `APT:S` endpoint.
It has not reached `main`, rendering, audio or gameplay.** No title-specific
runtime shortcuts, patched instructions, interpreter or runtime JIT are used.

## Pinned input (not distributed)

The supplied `d6nm2s3.zip` contains `Supertux.cia`, `Supertux/Supertux.3dsx`
and `Supertux/Supertux.smdh`. This work uses the CIA, not a new 3DSX loader.
Keep game binaries, extracted assets and derived native modules outside git.

SHA-256:

```text
82d2860b06325526c2cdf95c0dc4aa1364668a3a2509eea6f1a74c58a4d6eb01  d6nm2s3.zip
943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784  Supertux.cia
061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5  expanded exefs/.code
```

Title ID: `0004000000998200`. Extraction materializes all present content,
metadata and **834 RomFS files**, including images, levels, music and sounds.
These extracted files are not exposed through guest FS services yet.

ExHeader layout:

- Entry/text `0x00100000`, 340 pages, 1,389,676 declared bytes.
- Read-only data `0x00254000`, 54 pages, 217,776 bytes.
- Initialized data `0x0028a000`, 4 pages.
- Raw data bytes `0x21ac0` include BSS `0x1e36c`. Initialized bytes `0x3754`
  fit those pages. Bounded normalization accepts this maker's representation
  without materializing BSS from the input code.
- Initial stack `0x40000` bytes. Expanded page-packed code 1,630,208 bytes.

## Reproduce

Native 64-bit Linux, Clang and an LLVM SDK are required. LLVM 19.1.7 was tested.

```sh
cmake -S . -B build/ctr-aot \
  -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF \
  -DSNX_3DS_AOT=ON -DSNX_TEST_ARM_AOT=ON \
  -DSNX_TEST_3DS_SUPERTUX=/absolute/path/to/Supertux.cia
cmake --build build/ctr-aot -j4
ctest --test-dir build/ctr-aot -R 3ds_supertux_bootstrap -V
```

Select a non-system SDK with `SNX_LLVM_CONFIG`. `SNX_TEST_ARM_DIFF=ON` and pinned
Python `unicorn==2.1.4` add three CPU-reference gates:

- Before the first SVC: all 16 GPRs/CPSR and complete data/BSS, stack and TLS
  snapshots match independent ARM11 MPCore execution without HLE effects.
- At ConnectToPort: startup locks, GPRs/CPSR and complete data/BSS, both stacks,
  TLS, heap and linear-memory snapshots match after a **test-scripted HLE
  transcript**. This checks CPU execution under the selected process layout,
  not independent hardware correctness of that kernel model.
- At the APT:S wait: GPRs/CPSR and complete RAM snapshots also match under a
  **scripted IPC transcript**. This is not an independent service-policy oracle.

Unicorn is test-only, never linked or invoked by the runtime. The normal driver
runs real process-owned kernel handlers, not the oracle's scripted replies.

For a manual run, choose a **new** output directory:

```sh
build/ctr-aot/scarletnx-3ds --output work/supertux-001 /path/to/Supertux.cia
```

Expected host status is **3**, not success:

```text
Discovered ARM11 static coverage: 49085 blocks (conservative CFG)
Compiled ARM11 static module: 49085 blocks in 385 objects
Running ARM11 static AOT
guest blocked (scheduler not implemented): pc=0x001def50 exit=4 syndrome=0x00000032 blocks=31018 svcs=7 last_svc=0x32 ipc_service=1 ipc_header=0x00050100 wait_service=APT:S
```

The graph visits 208,022 ARM/Thumb instructions and conservative candidates.
This does not certify every computed target. LLVM chunks and native linking
finish **before guest execution**. Existing extraction/native output is still
refused. There is no cache/resume or runtime compilation.

## Observed startup

1. Entry branches to `0x100020` and clears BSS in guest code.
2. SVC `0x21` at `0x1deea4` creates an actual address-arbiter object. Its handle
   is stored at `0x2a4744`. The pre-HLE CPU gate remains PC `0x1deea8`, 30,949 blocks.
3. GetSystemInfo (`0x2a`, type 0, application) reports `0x1ee000` allocated bytes.
   The read-only configuration page reports the modeled 64 MiB application pool.
4. ControlMemory (`0x01`) commits `0x1f09000` bytes at `0x08000000`, then the same
   amount of physically contiguous linear memory at `0x160f7000`. Linear VAs
   preserve their physical-address relationship. The runtime moves its stack
   into the heap, with SP `0x08007fec` at the next gate.
5. Startup's `LDREX`/`STREX` locks now use checked RAM reservations. A board-wide
   write/mapping epoch rejects aliases, external writes and ABA changes without
   falling back to flat-memory accesses or value-only compare/exchange.
6. ConnectToPort (`0x2d`) requests `srv:` and returns actual session handle
   `0x202`. The earlier CPU gate at PC `0x1def40`, 30,980 blocks is preserved
   before handling this call.
7. SendSyncRequest (`0x32`) submits `RegisterClient`, header `0x00010002`.
   The generated handler associates the kernel-supplied caller with that session
   and replies `0x00010040`, result zero.
8. A second request, `0x00050100`, asks for `APT:S`, length 5, wait flag zero.
   This endpoint is absent. The guest is **BLOCKED**, not given a response or
   session, at PC `0x1def50`, 31,018 blocks / 7 SVCs. R0 remains `0x202`,
   SP is `0x08007f50`, LR `0x1df300`. Dispatch cannot resume past this wait.

The original kernel and srv connection barriers are fixed. APT is now the next
observed service boundary. FS session initialization is synthetic-test validated,
not yet reached by this title. See [CTR IPC](CTR-IPC.md) for precise scope.
The generic kernel and CPU implementation contains no title-ID or PC special case.

## Next gates

1. Real APT:S lifecycle/synchronization and subsequent requested services.
   The initial SwIPC-derived CTR wire subset and srv registration/lookup are
   implemented, but incoming handles/static/mapped buffers, FS resources, APT,
   GSP and HID are not. Resources must come from extracted RomFS and common
   filesystem facilities, not a mount of the CIA or Switch services.
2. Scheduling, wakeups, remaining SVCs, memory modes/resource capabilities and
   shared-memory lifetimes as actual execution requires them. Pending waits
   currently stop as BLOCKED, never successful completion.
3. Required ARM11 VFPv2 semantics, including short vectors. VFP is still unsupported.
4. Separately validate `main`, service initialization, first framebuffer, input,
   audible output and gameplay. None of these later milestones is reached yet.

Preserve shared A57 defaults and CPU ABIs. Broader static target coverage and
validated cache/resume remain separate work. See [3DS port](3DS.md) for exact
ISA, kernel-subset, memory and trusted-native-loading contracts. Title assets
remain external and retain their own licenses.
