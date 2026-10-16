<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# SuperTux integration target

The supplied SuperTux CIA now **reaches and executes `main`**, at `0x00167d88`
after **50,489 native blocks**. Its SelfNCCH RomFS open succeeds using NX's
shared indexed host filesystem. It executes scalar VFPv2 in `gettimeofday`
and opens a genuine SelfNCCH archive binding. Last probed it stopped at
**50,629 blocks** on FS `ControlArchive`'s timestamp request; real timestamp
semantics and the GSP/GPU display path now exist, so the next probe advances.
**No first frame, audio or gameplay is claimed.**
There are no title-specific runtime hooks, patched instructions, interpreter,
runtime target discovery or runtime compilation.

## Pinned input (not distributed)

The supplied `d6nm2s3.zip` contains `Supertux.cia`, `Supertux/Supertux.3dsx`
and `Supertux/Supertux.smdh`. This uses the CIA, not a new 3DSX loader.
Keep binaries, extracted assets and derived native modules outside git.

```text
82d2860b06325526c2cdf95c0dc4aa1364668a3a2509eea6f1a74c58a4d6eb01  d6nm2s3.zip
943d98144d6e7ffdbcf9cdbce43a4d0b4630afb8bc88f89d02ecc7f24eae5784  Supertux.cia
061c7021e97c2358da5795edeabb081f174e346472ccee82b57d6b56d9dda3e5  expanded exefs/.code
```

Title `0004000000998200` contains **834 extracted RomFS files** (images,
levels, music and sounds). They back a reconstructed CTR level-3 RomFS view;
the runtime does not reopen the original CIA/container.
Text starts at `0x100000`, with 340 pages / 1,389,676 declared bytes;
RO at `0x254000`, 54 pages / 217,776 bytes; data at `0x28a000`, four pages.
Raw data bytes `0x21ac0` include BSS `0x1e36c`; initialized bytes `0x3754`
fit those pages. BSS is not taken from input code. Stack size is `0x40000`;
expanded page-packed code is 1,630,208 bytes.

## Reproduce

Native 64-bit Linux, a supported Clang and an LLVM SDK are required.
LLVM 19.1.7 was tested; use `SNX_LLVM_CONFIG` for a non-system SDK.

```sh
cmake -S . -B build/ctr-aot \
  -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF \
  -DSNX_3DS_AOT=ON -DSNX_TEST_ARM_AOT=ON \
  -DSNX_TEST_3DS_SUPERTUX=/absolute/path/to/Supertux.cia
cmake --build build/ctr-aot -j4
ctest --test-dir build/ctr-aot -R 3ds_supertux_bootstrap -V
```

Choose a **new** output tree for a manual run:

```sh
build/ctr-aot/scarletnx-3ds --output work/supertux-001 /path/to/Supertux.cia
```

Expected host status is **3**, at unsupported archive timestamp control, not
completed emulation. Coverage is **70,683 blocks / 554 objects**, statically compiled and
linked before guest execution. The driver reports:

```text
unsupported guest operation or missing static entry: pc=0x001def50 exit=4 syndrome=0x00000032 blocks=50629 svcs=61 last_svc=0x32 ipc_service=2 ipc_header=0x080d0144 wait_service=
```

The external test independently observes the CRT load of main's address from
`0x10008c`, its `BX r3` at `0x100054`, entry at `0x167d88`, and CRT return
address `0x1626c4`. It then executes main through the real FS request. This
observation exists only in the test, not the runtime. Block counts are not cycles.

Existing output is refused; there is no validated cache/resume. Native modules
are trusted host code, not sandboxed or signature-authenticated by ABI/hash checks.

## Observed startup

1. Guest entry clears BSS, creates a real arbiter (`0x201`), and reports
   `0x1ee000` initial application allocation on Old 3DS. The configuration page
   is read-only AXI WRAM outside that accounting.
2. Real ControlMemory commits `0x1f09000` heap bytes at `0x08000000`, then the
   same-size linear allocation at `0x160f7000`. Checked ARM11 exclusives execute
   startup locks, with alias/ABA/write/mapping reservation invalidation.
3. `srv:` connection (`0x202`) and generated client registration succeed.
   `APT:S` supplies a shared recursive mutex and two real one-shot events, in
   **one copied-handle descriptor**. Its first/only application receives the
   manager's initial empty Wakeup, not a fabricated Home Menu session.
4. The guest creates a native background thread with independent zeroed TLS
   and stack. Priority/blocking scheduling and event/mutex/arbiter wakeups are
   serialized; waits do not complete just because dispatch is called again.
5. The startup subset handles readiness, receiving the queued Wakeup, releasing
   its startup transition, and the virtual console's open-shell case. Unknown
   applets/transitions and closed-shell sleep remain unsupported.
6. PTM confirms the existing base CPU configuration; accelerated modes remain
   unsupported. APT/PTM model queries report the actual board profile.
7. `hid:USER` supplies a real read-only system-pool page and five real events.
   The page maps at `0x10000000`. No samples or completed input waits are faked;
   periodic/host input is not implemented in this increment.
8. FS initializes the caller. Opening SDMC archive 9 returns **FS NotFound**:
   no SD backing/device is configured, and no mount/handle is fabricated.
   The title tolerates that error. Constructors run using newly supported
   ARM11 `UADD8`/`SEL`; bounded adjacent-CMP/LDRLS jump-table discovery supplies
   the missing static constructor targets.
9. CRT enters `main`; FS `OpenFileDirectly` on archive 3 returns a genuine
   read-only file object over the process' reconstructed RomFS. Host nodes,
   shared mappings and fallback opens are the same backing implementation NX uses.
10. `gettimeofday` reads the read-only UTC shared page and host-paced
    GetSystemTick (`0x28`), then executes scalar VFPv2 conversions/arithmetic.
    The offline CFG follows VFP fallthrough; no target is discovered at runtime.
11. `OpenArchive` on SelfNCCH returns an owned, FS-private 64-bit token backed
    by the process' real RomFS. `ControlArchive` GET_TIMESTAMP returns the
    backing file's real host modification time as a 3DS datetime (bounded
    UTF-16 path, `/` and `\` separators); save commits and unknown actions stop.
12. `gsp::Gpu` supplies a real 0x1000 GX/interrupt shared block, relay threads,
    a checked GX/LCD register file and a host-paced 60 Hz display. GX
    MemoryFill and DMA run as whole-span checked RAM transactions; PICA command
    lists and format-converting transfers stop explicitly. Vulkan-only
    presentation shows the landscape frame live in an SDL3 window and records
    bounded PPM snapshots in `<output>/frames`. No title frame yet.

See [CTR IPC](CTR-IPC.md) for the deliberately narrow buffer/service policies.
The full APT lifecycle, applets, SD/save mounting, writable archives and general
file operations are not implemented. FSFILE Read/GetSize/Close support the
read-only SelfNCCH RomFS view, not a general archive/path namespace. Bounded
OpenArchive/OpenFile/CloseArchive reuse that view; tokens are not kernel handles.

## Validation boundaries and next work

`SNX_TEST_ARM_DIFF=ON` with pinned `unicorn==2.1.4` preserves three reference
boundaries: before the first SVC (30,949 blocks), before ConnectToPort (30,980),
and **before APT lookup handling** (31,018). All GPRs/CPSR and complete relevant
RAM snapshots match. The later two use test-scripted HLE/IPC effects: they prove
CPU consistency under those transcripts, **not independent hardware service
policy**. The runtime never invokes Unicorn or those scripted replies.
ARM11 packed-byte tests separately compare condition codes, GE flags and
register aliasing at O0/O2. Real service/kernel tests cover rollback and waits;
an asset-free native CIA proves a worker actually wakes and resumes main.
A second native CIA opens RomFS through real CTR IPC, reads its metadata and
resolves/verifies `hello.txt` from the extracted host tree before exiting.
`3ds_filesystem` independently parses Unicode names/hash chains/content and
checks both shared mappings and forced on-demand IO, EOF, failed-read atomicity,
whole-buffer RAM/MMIO/permission checks, quotas, rollback and handle lifetime.
Existing NX tree/service/raw-image fixtures run in CTR-only builds as well.

`arm11_vfp_differential` compares 52,480 exact instruction/state cases against
Unicorn's ARM11 MPCore at O0/O2, flat and checked, including registers, FPSCR,
conditions, aliases, DN/FZ and rounding modes. Checked callbacks forbid direct
pointer bypasses; negative tests verify fault rollback and unsupported controls,
legacy format-word saves and wide transfers. Native CTR tests cover atomic RAM
transfers and real FP helper linkage. The installed offline compiler embeds the
project's PIC semantic archive, stages it only for offline linking and needs no
build-tree archive lookup. Generated modules do not depend on LLVM. Default
A57 ARM/Thumb O0/O2 objects remain byte-identical; public CPU/checked/module
ABIs remain 2/1/1.

Next: advance the title through its remaining startup (real archive timestamps
and the GSP/GPU display path are now in) toward a human-reviewed first frame.
Further HID work and DSP/audio are deferred.
Full VFPv2 (short vectors/exception traps), timers/time
slicing, full thread lifecycle/multicore, broad
resource/capability policy, general IPC buffers and broader static coverage are
still incomplete. Preserve A57 defaults and CPU/module ABIs.
