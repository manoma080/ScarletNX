<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# CTR IPC and minimal startup services (3a/3b)

This is **not** Switch HIPC/CMIF. It reuses the lexer/AST/output utilities of
`tools/swipc/swipc.c`, with a separate `--ctr` validator/emitter in `ctr.inc`.
The existing default emitter/output remains unchanged. See [NX IPC](IPC.md)
for that separate profile, and [3DS status](3DS.md) for outstanding work.

## Wire and ownership

- A command occupies 64 little-endian words at the active thread's TLS + `0x80`.
  Header bits 31:16 are the command ID, 11:6 normal-word count, 5:0 translated-word
  count; bits 15:12 must be zero. Header plus both regions must fit 64 words.
- Each native guest thread has its own zeroed, `0x200`-aligned TLS slot.
  Full RAM read/write permissions are checked **before** invoking handlers or
  allocating output handles. MMIO is never used for command-buffer transfers.
  Serialization makes the write preflight stable through response publication.
- Translation framing checks descriptor forms and payload counts, classifying
  buffers before handles. PID is a 32-bit kernel-supplied caller identity, not
  the value claimed by the guest. Incoming handles and PXI/mapped buffers remain
  **unsupported**. Static buffers are accepted only by the checked APT utility /
  ReceiveParameter and FS absent-SD paths below, not via a general schema type.
  Other static-buffer commands stop explicitly; Switch descriptors are never used.
- The response repeats the command ID; its first normal word is the service
  result. Transport SVC result and service result are distinct. Failed service
  calls publish a result-only response. Malformed requests return a transport
  error without changing the command buffer or invoking the handler. Unknown
  commands and valid-but-unimplemented translation features stop explicitly.
- Sessions are real typed kernel objects. Duplicates share state and retain a
  reference; final close frees the object. Output handles are newly owned client
  references, published with the declared copy/move descriptor and relinquished
  on handler failure. Contiguous output handles of the same kind share one
  descriptor, including APT's two copied events and HID's six copied handles. HLE has no separate server address space/handle table yet.
- Bounds are 256 handles/256 object slots including roots; endpoint caps are
  64 `srv:` and 32 FS sessions. These are bounded implementation quotas, not
  complete hardware resource-limit/ExHeader capability enforcement. APT/PTM/HID
  session caps are 2 each; thread cap is 16 including the initial thread.

## Generator subset

Use `snx_swipc(target CTR ROOT ... FILES ...)` in CMake or:

```sh
swipc --ctr --root src/3ds --out GENERATED services/srv.swipc services/fs.swipc
```

The output directory must exist. Schemas live beside service C files. Include
`services/name.swipc.inc` only in the C file defining its static typed handlers;
other translation units include the generated `.h`.

- `u32`, `u64`, word-sized `bytes<N>`, one `pid` input, and copy/move handle
  outputs. Raw words follow **declaration order**, with no NX alignment sorting
  or 64-bit alignment padding. Named aliases resolve to the same concrete types.
- Handler signature: `uint32_t interface_Command(SnxCtrIpc*, arguments...)`.
  A PID parameter is `uint32_t`; scalar outputs are pointers, byte arrays use
  byte pointers, and the return value is the service result. Non-return outcomes
  are explicit context states, not successful responses.
- Command IDs are 16 bits. Request and result/output layouts must fit the full
  64-word envelope; unsupported wire types and `@stub`, `@unsorted`, `@fallback`
  are refused. Unspecified commands stop; there is no default successful stub.
- The host generator and dedicated hand-encoded generator fixture compile as
  strict C90 on the tested LP64 host. The CTR runtime target remains C11.

## Implemented services

- ConnectToPort (`0x2d`) recognizes the actual named `srv:` endpoint. Invalid
  pointers/names and connection/resource limits return errors, not fake handles.
- SendSyncRequest (`0x32`) invokes checked generated dispatch on a session.
  GetProcessId (`0x35`) identifies the real single application's process root;
  PID `0x10` is local to that independently owned board/process instance.
- `srv:` RegisterClient (`1`) associates the caller process with that session.
  GetServiceHandle (`5`) returns real FS/APT/PTM/HID sessions. An absent endpoint
  with the non-wait flag returns ServiceNotRegistered; a waiting request blocks
  its actual thread without a response/session. Peers can execute; the wait is
  not completed without registration. Dynamic service installation is unsupported.
- `fs:USER` Initialize (`0x801`) and InitializeWithSdkVersion (`0x861`) record
  caller/SDK association. OpenArchive (`0x80c`) accepts only archive 9, empty
  path and a checked size-one static-buffer descriptor, returning FS NotFound
  (`0xc8804478`) because this virtual console has no SD backing. **No archives /
  files are opened or mounted.** Other FS operations remain unsupported.
- `APT:S`: GetLockHandle (`1`), Initialize (`2`), Enable (`3`), NotifyToWait
  (`0x43`), ReceiveParameter (`0xd`), AppletUtility (`0x4b`), IsNew3DS (`0x102`).
  One application slot (`0x300`, attributes 0), persistent recursive mutex,
  one-shot notification/parameter events and a real initial pending Wakeup.
  Unsupported applet attributes/identities/transitions stop explicitly. Duplicate
  initialization returns AlreadyExists and does not replace the slot.
- APT utility supports only operation 7 (release the initial startup transition,
  input `0x10`) and operation 4 (sleep-if-shell-closed, input 0, open shell only).
  A closed-shell sleep request is unsupported. Readiness/Wakeup consumption is
  phase-checked. ReceiveParameter returns an empty Wakeup with a **null object**,
  not a fabricated handle or payload. It consumes the parameter only after full
  command, descriptor and destination preflight. The checked TLS static table
  at +`0x180` must declare the expected buffer ID/capacity; MMIO is refused.
- `ptm:sysm`: model query (`0x40a`) and ConfigureNew3DSCPU (`0x818`) for the
  existing base configuration **0 only**. Boost/L2/other configurations stop.
- `hid:USER`: GetIPCHandles (`0xa`) returns actual system-pool shared memory
  and five one-shot events. MapMemoryBlock (`0x1f`) supports only the fixed HID
  read-only page at `0x10000000` (permissions 1/`0x10000000`). No periodic input
  updates/events or writable/general shared-memory mappings are implemented.

Serialized kernel support includes CreateThread (`8`), fixed per-thread priorities,
ReleaseMutex (`0x14`), Create/Signal/ClearEvent (`0x17`/`0x18`/`0x19`),
WaitSynchronization1/N (`0x24`/`0x25`), mutex recursion, waiter references and
priority-ordered event/mutex/arbiter wakeups. Pending thread CPU/TLS/wait state
is preserved across native dispatch. Only immediate / infinite waits are supported;
finite nonzero pending deadlines stop before mutation. Timers, time slicing,
full thread lifecycle and multicore remain unsupported; no host-thread safety
or hardware-complete scheduler claim is made. With no runnable peers a real
wait stays BLOCKED, never a fabricated successful return.

SuperTux **reaches and executes main**, then stops at FS OpenFileDirectly.
See [the exact witness](3DS-SUPERTUX.md). FS resources must use extracted RomFS
and common filesystem facilities, not mount the CIA or reuse NX services.

## Tests and reference provenance

`3ds_ipc` executes real sessions/handlers against checked memory on both models,
including forged PID replacement, malformed counts/descriptors, read-only,
unmapped/misaligned/MMIO TLS, stale/duplicate handles, endpoint/handle exhaustion,
rollback and non-resumable waits. `ctr_swipc_dispatch` is a generator-only C90
fixture with deliberately test-local resource accounting; it checks declaration
order, 64-bit/byte/PID layout, reply packing and failed-output rollback.
`ctr_swipc_validation` rejects unsupported schemas, checks full-envelope bounds
and pins unchanged legacy default output. Synthetic CIA pipelines execute real
SVC/TLS request and blocked-wait paths through offline native AOT.

The optional supplied-title test preserves both earlier CPU reference boundaries
and a third before APT:S lookup handling, then executes the genuine main path. The post-HLE comparisons script selected HLE
and IPC effects: they validate CPU behavior, **not independently hardware-correct
kernel/service semantics**. The runtime executes actual C handlers, never that
script or a CPU interpreter. `3ds_services` tests both models, APT state/buffer
preflight, real resources, quotas/rollback, and PTM/HID/FS boundaries.
`3ds_threads` tests context/TLS preservation, waiter ownership, event/mutex/
arbiter wakeups, wait-all and no-peer blocking. A native synthetic CIA requires
its worker to write shared RAM and signal main before main can exit successfully.

Reference facts: Azahar `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e`,
`core/hle/ipc.h`, `kernel/hle_ipc.cpp`, `kernel/svc.cpp`,
`service/sm/{srv.cpp,sm.h}`, `service/fs/fs_user.cpp`, `service/apt/`,
`service/hid/`, `kernel/{mutex,event,thread}.cpp` (GPL-2.0-or-later), and libctru's
APT startup protocol (see its own license). Service policy is a bounded HLE
personality, not an independently hardware-validated full implementation.
New C transport/services and generator extension are independent project code
under GPL-3.0-or-later, retaining the shared SwIPC tool's provenance. No Azahar
runtime/build dependency or title assets are included.
