# mythread_tests

A standalone test framework for a minimal POSIX-like threading library (`mythread`). It runs each test in a **forked child process** with a configurable timeout, so a crashing or hanging test never takes down the whole suite.

---

## Build & Run

### Reference implementation (pthreads)

```bash
make check-reference
```

### Your custom implementation

Copy your library directory into impl/custom/ (sources, an .a archive or
a mix — see "Connecting your library" below), then:

```bash
make check-custom
```

### Manual build

```bash
# Reference
make IMPL=reference mythread_tests
./mythread_tests

# Custom
make IMPL=custom mythread_tests
./mythread_tests
```

Clean up with `make clean` (removes the binary and the generated `build/` shim directory).

---

## Project Structure
```
mythread_tests/
├── include/
│ └── mythread.h 		# Public API — the only mythread.h in the project
├── impl/
│ ├── reference/ 		# pthread-backed reference implementation
│ │ ├── mythread.c
│ │ └── mythread_types.h 	# defines mythread_t / mythread_attr_t
│ └── custom/ 			# Your implementation — copy your library here
│  	 			# (any layout, see "Connecting your library")
├── tests/ 				# Individual test suites
│ ├── tests.h 			# Test declarations for main.c
│ ├── test_create.c
│ ├── test_join.c
│ ├── test_detach.c
│ ├── test_exit.c
│ ├── test_lifecycle.c 	# cross-function lifecycle & process semantics
│ ├── test_race.c 		# concurrency & race scenarios
│ ├── test_resource.c 	# leaks, reclamation, fd hygiene
│ ├── test_errors.c 	# bad-input behaviour
│ └── test_stress.c
├── framework/
│ ├── runner.c 			# fork + timeout per test
│ ├── runner.h
│ ├── assert.h 			# Assertion macros
│ └── colors.h 			# ANSI colour helpers
├── main.c 				# Test registry & entry point
└── Makefile
```

---

### Header resolution rules

There is exactly **one** `mythread.h` — the public contract in include/.
It comes first on the include path, so no implementation header can
shadow it. If your implementation needs its own private declarations,
name the file anything else (e.g. `mythread_impl.h`) and include it only
from your own `.c` files.

The *types* `mythread_t` and `mythread_attr_t` are defined by the
implementation, not by the contract: a pthread wrapper (1.6) and a
userspace scheduler (1.7) represent a thread handle differently. Which
definition wins is resolved like **weak symbols**, but for types:

1. `impl/custom/mythread_types.h` — your file, used verbatim;
2. any other header under your impl directory that defines `mythread_t`
   — wired up through an auto-generated redirect in `build/`;
3. no types header at all — the framework fallback
   (`include/mythread_types_default.h`, `unsigned long` handle) is used,
   and the build prints a NOTE about it.

The fallback lets a skeleton implementation compile before any types are
written. It only fits handles that are binary-compatible with
`unsigned long` (a pthread_t, a kernel TID, a TCB index/pointer — the
three typical designs). Anything else requires your own types header.

---

## Connecting your library

Drop your library into `impl/custom/` — copy the **whole directory**, any
internal layout works. The Makefile discovers everything on its own:

```bash
cp -r ~/my-os-thread-lib mythread_tests/impl/custom
make check-custom
```

What is discovered automatically under `impl/custom/`:

| Your files | What the Makefile does with them |
|---|---|
| `*.c` (any depth) | compiled and linked together with the tests |
| `*.a` (any depth) | linked in as static archives (sources optional) |
| directories containing headers | added to the include path |
| `mythread_types.h` | used as the types header, no questions asked |
| any other header defining `mythread_t` | found and wired up via an auto-generated redirect in `build/` — you don't have to rename anything |
| nothing mentioning `mythread_t` | the framework fallback types are used, a NOTE is printed |

Dropping your library never fails the build over types: at worst the
fallback is used with a NOTE. Watch for that NOTE — if your thread
handle is not an `unsigned long`, the fallback will compile but behave
incorrectly, and you need your own types header.

Two tips:

- If several headers define `mythread_t`, the one picked may surprise
  you — simply name your chosen types header `mythread_types.h` and it
  wins over the auto-detection.
- The generated shim lives in `build/` and is regenerated on every
  build, so renaming your headers later needs no `make clean`.

---

## How it works

1. **Fork-per-test runner (framework/runner.c)**
Each test runs in a child process created with fork(). The parent waits with a timeout (default 5000 ms, per-test override via timeout_ms). If the child doesn't exit in time it is killed with SIGKILL and reported as **TIMEOUT**. Crashes (segfaults, aborts) are reported as **FAIL** with the signal number.

2. **Negative tests and strict mode (tests/test_errors.c and others)**
POSIX leaves most bad-input cases undefined: join/detach of a spent, detached or zero handle, self-join, several joins of one thread, a NULL start routine. glibc crashes or misbehaves on many of them, so by default these calls are **not made at all** — a thin pthread wrapper must not fail for reasons outside the student's control. Build with `-DMYTHREAD_STRICT_ERRORS` (`make check-strict`, or `make STRICT=1`) to make them and require explicit error codes — use this when grading implementations that validate their handles. The pthread reference is not one of them, so several strict tests fail on it by design.

---

## Test environment knobs

| Knob | Meaning |
|---|---|
| `MYTHREAD_REPEAT=20 ./mythread_tests` | run every test N times, each in a fresh process — race-hunt mode (nondeterministic failures report which repetition broke) |
| `make CPPFLAGS=-DMYTHREAD_STRESS_LEVEL=3 ...` | heavier stress loops (100k lifecycles, longer soak); levels 1–3, default 1 |
| `make sanitize` | the whole suite under ThreadSanitizer — proves tests are race-free and surfaces races in the implementation |
| `make sanitize-mem` | the whole suite under ASan + UBSan (use-after-free, leaks) |
| `make valgrind` | the whole suite under Valgrind memcheck |

The tests synchronize exclusively through C11 `__atomic_*` builtins — no volatile-based "synchronization" (it is not synchronization). No POSIX mutexes/condvars either: a userspace scheduler (1.7) must not be tested through blocking syscalls its own implementation may not survive. Sleeps appear only where wall-clock behaviour itself is under test (join blocking, soak budget) — never as a correctness signal.

---

## Requirements

- GCC or Clang (C11)
- POSIX system (fork, waitpid, usleep)
- pthread for the reference implementation
