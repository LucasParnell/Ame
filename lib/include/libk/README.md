# libk — the freestanding runtime

libk is the shared freestanding base for the kernel and userland: the start of
the "libc replacement". Which tier a symbol lives in is a **policy decision, not
a style one**, because it decides what a service author is allowed to reach for.

## Tiers

1. **Advertised, safe** — `bytes.h`, `str.h`, `num.h`, `integer.h`, `printf.h`,
   `metric.h`, `panic.h`.
   This is what callers use. Every helper is bounded: sizes travel with the
   pointer, a wrong length yields a short count or `false`, parsers reject
   overflow, writers never exceed the capacity they were given.

2. **Internal runtime floor** — `string.h` (`memcpy`/`memset`/`memmove`/
   `memcmp`, `be32`/`be64`).
   Untyped `(pointer, length)`; the compiler also emits calls to these for
   aggregate copies, so the symbols must exist in every image. **Services must
   not include it**: `userland` builds define `AME_MOCHI_SERVICE`, and the
   header turns that into a build error. The runtime, the kernel and the shared
   format library may include it.

3. **Caller-facing policy layer** — `libmochi` (`mochi/*.h`): `Ring` /
   `Channel`, `Handle`, `Arena`, `boot`, terminal, log, the service protocols.
   A service author writes against this and never sees raw memory or a ring
   slot.

## Rules

- Move bytes with `libk::copy` / `copy_n` / `fill` over `libk::bytes` views —
  never `memcpy`.
- Format numbers with `libk::snprintf` or `libk/num.h` — never a digit loop.
- Allocate from a `mochi::Arena` over a registered region; there is no heap.
- Strings are bounded (`libk/str.h`); the `_n` forms are for text that may cross
  a trust boundary without a terminator.
- A new helper belongs in the safe tier unless it genuinely needs untyped
  access — and even then that is a request to widen the floor, not to add a
  second `memcpy`.
