# Linux AArch64 GNU ABI runtime

## Index

* [Build and run](#build-and-run)
* [Implemented scope](#implemented-scope)
* [Differential validation and fuzzing](#differential-validation-and-fuzzing)
* [ABI references](#abi-references)

ScarletNX executes static Linux AArch64 ELF programs with its existing ARM64-to-C
recompiler. `scarletnx-gnuabi` loads the ELF, translates executable ranges into a
temporary native module with host Clang, and runs that module with Linux runtime
callbacks. The shared generated interpreter remains the fallback for indirect
targets. QEMU is an external reference used by tests; it is never the execution
engine or a dependency of this executable.

## Build and run

The GNU ABI runtime can be built without the Switch runtime or its graphics and
media packages:

```sh
cmake -S . -B build/gnuabi -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=ON
cmake --build build/gnuabi
ctest --test-dir build/gnuabi --output-on-failure
```

Both runtimes are enabled by default. The NX runtime always requires SDL3, Vulkan and FFmpeg.
Windows uses the existing
llvm-mingw toolchain and bootstrap environment described in [BUILD.md](BUILD.md).
The GNU runtime needs host Clang when executing a new ELF; use `--cc=PATH` to
select it explicitly.

```sh
aarch64-linux-gnu-gcc -static -O2 tests/gnuabi/libc.c -o /tmp/libc-aarch64
build/gnuabi/scarletnx-gnuabi /tmp/libc-aarch64
```

Arguments following the ELF are passed to the guest. The guest environment is
currently `PATH=/bin` and `LANG=C`. The input ELF is the only host file exposed as
guest `/program`; guest file operations otherwise use the isolated virtual
filesystem. Standard input, output, and error connect to the launching process.

## Implemented scope

The independent ELF loader accepts ELF64 little-endian `EM_AARCH64` `ET_EXEC`
and `ET_DYN`, validates program headers and load intervals, maps `PT_LOAD` with
their permissions, zeroes BSS, and creates an aligned stack containing `argc`,
`argv`, `envp`, and Linux auxiliary vectors. ET_DYN receives a load bias;
position-independent code must handle its own relocations. `PT_INTERP` produces
an explicit unsupported-interpreter error. This is a bounded 4 GiB guest address
space with executable load segments below 1 GiB and an 8 MiB initial stack.

`SVC #0` dispatches syscall number `x8`, arguments `x0` through `x5`, and result
`x0`. Failed calls return negative Linux errno values. Other SVC immediates,
unimplemented instructions, and guest breakpoints terminate with a diagnostic.
Unsupported syscalls return `-ENOSYS`.

The GNU runtime checks alignment and guest execute permission before every
instruction, including AOT instructions. Changed instruction words use the shared
interpreter. Protected guest data accesses terminate with a guest fault diagnostic
and release the temporary compiler module. Guest signal handlers are not entered.

GNU execution checks current instruction-fetch permissions and the live
instruction word. A changed instruction uses shared interpreter semantics rather
than its stale compiled form. Memory permission faults terminate the guest with
a diagnostic and signal-style exit status; this does not deliver a user signal
handler. The shared fetch callback is optional and NX preserves its existing
execution path.

Implemented syscall families cover file descriptors (`read`, `write`, vectored
I/O, `openat`, `close`, `lseek`, `dup`, `fcntl`), metadata and directory operations
(`fstat`, `newfstatat`, `readlinkat`, `getdents64`, `mkdirat`, `unlinkat`,
`ftruncate`, `chdir`, `getcwd`), memory (`brk`, `mmap`, `munmap`, `mprotect`),
process exit and identification, `uname`, clocks, random bytes, signal action and
mask storage, and basic single-thread futex operations. Networking adds host-backed
sockets, socketpair, poll/select, epoll and eventfd through the shared stack;
read-only virtual `/etc` files support guest libc DNS. See [NETWORKING.md](NETWORKING.md)
for configuration, tests, and current semantic limitations. Guest structures and
constants are encoded for AArch64 rather than copied from the host ABI.

The initial runtime executes one guest thread. `clone`, signal delivery and
`rt_sigreturn`, dynamic linking, file-backed `MAP_SHARED`, and a general Linux
distribution environment are unsupported. Futex wake returns zero without
peers; waits with a mismatched value return `-EAGAIN`, timed waits can return
`-ETIMEDOUT`, and a blocking wait without a timeout returns `-ENOSYS`. Signal
actions and masks are stored, but no signal handler is entered. These limits
must be considered when choosing test programs.

The filesystems are intentionally small:

| Mount | Contents and behavior |
| --- | --- |
| `/tmp` | Ephemeral in-memory files and directories, mutation, metadata, and enumeration |
| `/dev` | `null`, `zero`, `random`, `urandom`, and descriptor links under `fd` |
| `/proc` | Guest `self/exe`, `self/maps`, `self/cmdline`, `self/status`, `self/fd`, and AArch64 `cpuinfo` |

No Maxwell, Vulkan, OpenGL, DRM/KMS, display, or graphics service is initialized
by this runtime. It cannot open arbitrary host paths through guest `openat`.

## Differential validation and fuzzing

Install `qemu-aarch64` and an AArch64 cross compiler outside ScarletNX, then run:

```sh
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    --suite all --fuzz 100 --seed 1000
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    /tmp/libc-aarch64
```

The fixtures are freestanding static ELF files. The CPU suite covers arithmetic,
branching, unaligned memory, floating point, SIMD, exclusive atomics, a function
crossing a 4 KiB instruction boundary, and initial stack alignment. The syscall
suite exercises tmpfs, devfs, procfs, descriptor and metadata calls, clocks,
random reads, and unsupported-call errors. The PIE suite verifies ET_DYN,
PC-relative global accesses, and BSS initialization. The optional `libc.c` fixture exercises
static libc startup, allocation, memory operations, and formatted output.
The standalone `self_modify.S` fixture must exit 42 after replacing a compiled
instruction; `fetch_fault.S` must fault after removing the executing page's
execute permission. These intentional faults should be checked directly rather
than passed to the default differential success suite.

Seeded fuzzing generates unsigned integer and memory programs with reproducible
source; it is an initial corpus generator rather than complete ISA coverage.
Each generated fixture must exit zero. Existing input ELFs may intentionally
return another status. The tool compares exact stdout bytes and exit status,
detects timeouts and crashes, and optionally compares stderr with
`--compare-stderr`. Avoid comparing printed timestamps, process IDs, random
values, filesystem enumeration order, or host-specific paths. Tests should
assert their properties and print deterministic results instead.

Failures are retained under `build/gnuabi-failures` by default. Each case includes
the ELF, available source and seed, stdin, both output streams, compiler command,
ELF SHA-256, timings, original commands, and replay commands in `result.json`.
`--keep-success` also retains matching cases. Use `--timeout` to allow host
compilation of larger ELFs and `--compile-timeout` for fixture cross compilation.

Clang plus LLD can build the freestanding suites without a target libc:

```sh
python3 tools/gnuabi_diff.py --scarletnx build/gnuabi/scarletnx-gnuabi \
    --suite all --cc clang --cc-arg=--target=aarch64-linux-gnu \
    --cc-arg=-fuse-ld=lld --qemu /path/to/qemu-aarch64
```

Source-boundary and transitive CMake dependency checks run as
`runtime_architecture` in CTest. The harness unit tests intentionally create an
output mismatch and verify that a reproducible ELF and both results are saved.

## ABI references

The implementation uses the official
[Linux arm64 UAPI headers](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/include/uapi/asm),
[asm-generic syscall numbers](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/asm-generic/unistd.h),
and [Linux UAPI definitions](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi).
The [arm64 syscall entry](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/kernel/syscall.c)
also defines the syscall number's 32-bit conversion from `x8`.
Behavior is cross-checked with
[QEMU Linux user-mode documentation](https://www.qemu.org/docs/master/user/main.html)
and its [AArch64 syscall entry](https://gitlab.com/qemu-project/qemu/-/blob/master/linux-user/aarch64/cpu_loop.c).
