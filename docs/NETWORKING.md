# Shared networking

## Index

* [Layout and translation tables](#layout-and-translation-tables)
* [Runtime behavior](#runtime-behavior)
* [Current limitations](#current-limitations)
* [Tests](#tests)

`src/common/network` is the C networking backend used by both runtimes. TCP and UDP
use host sockets; ICMP echo has a partial emulation for hosts that refuse raw
sockets. Host descriptors are never exposed directly to guests.

This ports the networking design and service behavior from Eden's
`lizzie/refactor-networking-12`, revision
`2014a5778273eef450bf3a48530d703750487a18`, principally
`src/core/internal_network/` and the sockets/NIFM services. Source files retain
Eden/yuzu notices where adapted; existing SwitchRecomp/MZ Labs notices remain on
modified files. This is a C adaptation, not a vendored copy of Eden's C++ runtime.

## Layout and translation tables

- `src/common/network/`: reference-counted sockets, host ABI translation, poll,
  cancellation, timeouts, ICMP, eventfd, resolver and interface discovery.
- `src/gnuabi/network/`: AArch64 Linux syscall/layout translation and resolver files.
  Sockets share the VFS descriptor namespace and open-file lifetime with ordinary
  files, including `dup`, `read`, `write`, `fcntl`, `ioctl` and `fstat`.
- `src/nx/hle/services/network/`: BSD IPC, sfdnsres/nsd and NIFM adapters.

Each layer's `abi/*.inc` files are re-includable X-macro tables. Common tables
cover errno (including Winsock), domains, socket types, levels/options, message
and poll flags, and resolver errors/flags. Guest tables cover their Linux or
Horizon constants and Linux syscall numbers. Define the relevant row macro before
including a table; tables clean up the macros afterwards. Add mappings there
rather than introducing parallel switch statements or relying on host numbers.
Option tables also specify the value layout. Address and port bytes remain in
network byte order in the shared `SnxSockAddr` representation; the IPC resolver's
serialized records have their own wire encoding.

Host sockets stay nonblocking. Guest blocking is implemented by waiting for
readiness with receive/send deadlines. The cancellation API wakes blocked waits;
callers retaining a socket reference can safely finish I/O after a descriptor is
closed. This library does not provide an Ethernet/IP virtual machine, virtual
LAN, or Eden multiplayer proxy.

## Runtime behavior

GNU ABI networking uses the host connection. Its read-only virtual `/etc` contains
`resolv.conf`, `hosts`, `nsswitch.conf`, `protocols`, and `services`. Guest libc can
resolve names using those files and ordinary UDP/TCP DNS. No arbitrary host file
paths become accessible; AF_UNIX pathname bind/connect is unsupported, while
`socketpair` is supported.

NX defaults to `network=0`. It reports offline through NIFM, permits outbound
loopback destinations and numeric/localhost resolution. Set `network=1` in the
runtime options to use host networking and host-derived NIFM configuration.
Known service domains in `blocked_hosts.inc` are blocked in the NX resolver.
This is **not a security sandbox or an IP firewall**: direct IP traffic is not
filtered by that domain list, and listeners bind host interfaces. Use OS-level
network isolation for untrusted guests.

ICMP first uses a native socket where permitted, then an unprivileged ping
socket, then POSIX `ping`/`ping6` or Windows `IcmpSendEcho`. The helper backend
synthesizes an echo reply only after the host helper reports success. It preserves
the request identifier, sequence and payload, and adds an IPv4 header for raw
IPv4 receives. Without a usable backend, sends can be accepted but receive waits
time out; no successful live echo is fabricated. Other ICMP message types are
not emulated. The Windows helper currently supports IPv4 only.

## Current limitations

- Host resolver calls are synchronous and not interrupted by socket cancellation.
- GNU ABI still runs one guest thread. Poll/select signal masks are not delivered.
- Epoll approximates edge-triggered requests with level readiness, supports
  oneshot, and does not expose nested epoll readiness through poll.
- Ancillary/control messages are not forwarded. `recvmsg` reports no control data.
- `recvmmsg` does not yet implement its timeout argument.
- Eventfd overflow returns `EAGAIN`, including for a blocking write.
- NX descriptor state belongs to each service object and its cloned sessions;
  separate registrations do not yet join a process-wide shared-client table.
- NX's resolver serialization and NIFM profiles have not been validated in a game.
  Several system-only commands remain stubs or return unsupported errors.

## Tests

The dependency-light tests run even with NX disabled:

```sh
cmake -S . -B build/gnuabi -DSNX_BUILD_NX=OFF
cmake --build build/gnuabi
ctest --test-dir build/gnuabi --output-on-failure
```

- `network_stack`: host loopback TCP/UDP, options, timeout/nonblocking behavior,
  polling, socketpair, eventfd, cancellation, numeric resolution and interfaces.
  Its live ICMP check explicitly reports a skip when no echo arrives.
- `network_icmp`: deterministic injected-reply tests of odd-length checksums,
  headers, truncation, peek ownership, queue limits and missing-helper cleanup.
  These do not claim to prove host connectivity.
- `gnuabi_network`: direct guest-memory/syscall tests, descriptor ownership,
  epoll/select, socket metadata and filesystem isolation.
- `bsd_service`: production BSD dispatch with a stubbed kernel; checks guest
  sockaddr/constants, TCP/UDP, poll/select, options, eventfd, offline rejection,
  exact error reply sizes and zero-length receives.

An optional static AArch64 libc fixture exercises the complete ELF-to-native
execution path, without external DNS or ICMP requirements:

```sh
aarch64-linux-gnu-gcc -static -O1 tests/gnuabi/network.c -o /tmp/network-aarch64
# Alternatively: zig cc -target aarch64-linux-musl -static -O1 tests/gnuabi/network.c -o /tmp/network-aarch64
timeout 60 build/gnuabi/scarletnx-gnuabi --cc=clang /tmp/network-aarch64
```

Expected stdout: `network test: ok`. The fixture is separate from the freestanding
CPU/syscall suites because it needs an AArch64 libc/toolchain. Build/test success
is not game validation; no game scene has been tested for this port.
