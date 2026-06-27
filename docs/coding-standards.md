# Lockstep Coding Standards

These standards are mechanically enforced wherever possible (warnings-as-errors,
clang-tidy in CI, the forbidden-call lint, the sanitizer matrix). Human review is
not the gate — the gate is the gate. This document encodes the two cardinal rules
that govern how C++ is written in this repo, plus the supporting conventions.

> Source of truth: `lockstep-master-plan.md` §4 (Cardinal Rules). Rules 6 and 7
> are reproduced and expanded below; rule 1 (the abstraction boundary) is the
> subject of the forbidden-call lint and is summarized at the end.

---

## Cardinal Rule 6 — Modern C++ discipline

**C++23. RAII. No raw owning pointers. No naked `new`/`delete`. Coroutines for async.**

- **C++23, no extensions.** Every target builds with `-std=c++23`
  (`CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_STANDARD_REQUIRED ON`,
  `CMAKE_CXX_EXTENSIONS OFF`). Never rely on a GNU/compiler extension.

- **RAII for every resource.** Anything with acquire/release semantics — memory,
  file handles (via `IDisk`), connections (via `INetwork`), locks — is owned by
  an object whose destructor releases it. No manual cleanup paths, no `goto
  cleanup`, no "remember to close it".

- **No raw owning pointers.** A `T*` is a non-owning observer, always. Ownership
  is expressed in the type: `std::unique_ptr<T>` for sole ownership,
  `std::shared_ptr<T>` only where shared lifetime is genuinely required (justify
  it). Pass non-owning references as `T&` / `const T&` / `std::span` /
  `std::string_view`. If a raw pointer can be null, say so at the boundary, but it
  still never owns.

- **No naked `new`/`delete`.** Use `std::make_unique` / `std::make_shared` /
  containers. A bare `new` or `delete` in first-party code is a bug. (Custom
  allocators / placement-new, if ever needed, live behind an RAII wrapper and are
  reviewed explicitly.)

- **Coroutines for async.** Asynchronous control flow is expressed with C++20/23
  coroutines (`co_await` / `co_return`) over the project's `Future<T>` /
  `Promise<T>` / `Task` types (Phase 1). Do **not** hand-roll callback chains,
  state machines, or — critically — OS threads to model concurrency. The
  deterministic scheduler resumes coroutines; that is the only concurrency model.

- **Prefer values and the standard library.** `std::optional`, `std::variant`,
  `std::expected`/`Error` for fallible returns, `enum class`, `[[nodiscard]]` on
  anything whose result must be checked, `const`/`constexpr` by default,
  `explicit` constructors. Rule of zero: let the compiler generate special
  members; if you write one, you justify all of them.

Enforced by: `cmake/Warnings.cmake` (`-Wall -Wextra -Werror -Wpedantic`, with one
suppression — `-Wno-missing-field-initializers`, since designated-init that omits
trailing fields is idiomatic here and the omitted fields use their in-struct
defaults), the `cppcoreguidelines-*` / `modernize-*` clang-tidy families, and the
sanitizer matrix (ASan/TSan/UBSan/MSan).

---

## Cardinal Rule 7 — No determinism leaks

**No order-dependent `unordered_map` iteration. No behavior dependent on pointer
addresses. No background OS thread touching shared state in sim.**

A Lockstep run must be a pure function of `(seed, initial tasks)`. Any
nondeterminism that leaks into observable behavior breaks reproducibility — the
single property the whole system is built to guarantee. Therefore:

- **No order-dependent unordered iteration.** Iterating an `unordered_map` /
  `unordered_set` yields an unspecified order that can vary by build, allocator,
  or run. Never let observable behavior (output, scheduling, hashing into
  decisions) depend on that order. Use ordered containers (`std::map`,
  `std::vector` + sort with a total order) when iteration order matters, or sort
  explicitly before acting.

- **No behavior dependent on pointer addresses.** Do not order, hash, key, or
  branch on a pointer's numeric value (addresses vary run-to-run, and ASLR /
  allocator behavior makes them nondeterministic). The scheduler's ready-queue
  order is a *deterministic, documented* key — never pointer order.

- **No background OS thread touching shared state in sim.** Simulation runs on a
  single OS thread under the deterministic scheduler. No `std::thread`, no thread
  pools, no async timers, no coordination atomics (`std::atomic` with explicit
  `memory_order`, `std::memory_order_*`). Concurrency is *logical* (coroutines on
  the scheduler), not physical. Real threads exist only in Phase 7 prod
  providers, behind the abstraction boundary.

- **All randomness via `IRandom`.** A single seeded PRNG is the only source of
  randomness in the system. No `std::rand`, `std::random_device`, or `<random>`
  engines in first-party code.

- **All time via `IClock`.** Virtual time only. No `std::chrono`, no wall-clock,
  no `std::this_thread::sleep`. Time advances only when no continuation is ready.

Enforced by: the forbidden-call lint (`tools/lint/forbidden_calls.py`), the
`concurrency-*` clang-tidy family, and the deterministic-replay gate (same seed ⇒
byte-identical trace) from Phase 1 on.

---

## Supporting conventions

- **Namespace / includes.** Core lives in `namespace lockstep::core`, included as
  `<lockstep/core/IClock.hpp>` etc. (frozen by the integration contract).
- **Formatting.** `.clang-format` is authoritative; CI checks it. Run
  `clang-format -i` before committing.
- **Errors.** Fallible operations return the shared `Error` type
  (`<lockstep/core/Error.hpp>`), not exceptions across the abstraction boundary.
- **The lint-exempt zone is `providers/`** (cardinal rule 1; plus
  `tools/lint/fixtures/`, the deliberately-dirty test fixtures, so the lint's own
  tests don't self-trip). Every other directory is scanned by the forbidden-call
  lint; nondeterministic primitives (clock/network/disk/random/threads) are
  permitted **only** inside provider implementations, behind the interface headers.
