<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Initial CTR IPC (3a)

This is **not** Switch HIPC/CMIF. It reuses the lexer/AST/output utilities of
`tools/swipc/swipc.c`, with a separate `--ctr` validator/emitter in `ctr.inc`.
The existing default emitter/output remains unchanged. See [NX IPC](IPC.md)
for that separate profile, and [3DS status](3DS.md) for outstanding work.

## Wire and ownership

- A command occupies 64 little-endian words at the active thread's TLS + `0x80`.
  Header bits 31:16 are the command ID, 11:6 normal-word count, 5:0 translated-word
  count; bits 15:12 must be zero. Header plus both regions must fit 64 words.
- The initial runtime has one initial thread with a `0x200`-aligned TLS slot.
  Full RAM read/write permissions are checked **before** invoking handlers or
  allocating output handles. MMIO is never used for command-buffer transfers.
  Serialization makes the write preflight stable through response publication.
- Translation framing checks descriptor forms and payload counts, classifying
  buffers before handles. PID is a 32-bit kernel-supplied caller identity, not
  the value claimed by the guest. Incoming handles and static/PXI/mapped buffers
  are recognized but remain **unsupported**; addresses are not dereferenced or
  interpreted using Switch descriptors. Static-buffer tables are not implemented.
- The response repeats the command ID; its first normal word is the service
  result. Transport SVC result and service result are distinct. Failed service
  calls publish a result-only response. Malformed requests return a transport
  error without changing the command buffer or invoking the handler. Unknown
  commands and valid-but-unimplemented translation features stop explicitly.
- Sessions are real typed kernel objects. Duplicates share state and retain a
  reference; final close frees the object. Output handles are newly owned client
  references, published with the declared copy/move descriptor and relinquished
  on handler failure. HLE has no separate server address space/handle table yet.
- Bounds are 256 handles/256 object slots including roots; endpoint caps are
  64 `srv:` and 32 FS sessions. These are bounded implementation quotas, not
  complete hardware resource-limit/ExHeader capability enforcement.

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
  GetServiceHandle (`5`) returns real FS sessions. An absent endpoint with the
  non-wait flag returns ServiceNotRegistered; a waiting request becomes BLOCKED
  without a response/session. Dispatch cannot resume past it. No registration
  wakeups, service-install API or scheduler are implemented yet.
- `fs:USER` Initialize (`0x801`) and InitializeWithSdkVersion (`0x861`) record
  caller/SDK association. **No archives/files are opened or mounted.** All other
  FS commands stop explicitly. FS initialization is synthetic-test validated;
  the supplied title has not reached it yet.

SuperTux connects/registers `srv:`, then blocks requesting absent **`APT:S`**.
This remains **before main, graphics and audio**. Next work is genuine applet
lifecycle, synchronization and requested services, not fabricated availability.
When FS resource operations are implemented they must use the extracted RomFS
and common host-filesystem facilities, not the original CIA or NX services.

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
and adds a third at the APT:S wait. The post-HLE comparisons script selected HLE
and IPC effects: they validate CPU behavior, **not independently hardware-correct
kernel/service semantics**. The runtime executes actual C handlers, never that
script or a CPU interpreter.

Reference facts: Azahar `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e`,
`core/hle/ipc.h`, `kernel/hle_ipc.cpp`, `kernel/svc.cpp`,
`service/sm/{srv.cpp,sm.h}` and `service/fs/fs_user.cpp` (GPL-2.0-or-later).
New C transport/services and generator extension are independent project code
under GPL-3.0-or-later, retaining the shared SwIPC tool's provenance. No Azahar
runtime/build dependency or title assets are included.
