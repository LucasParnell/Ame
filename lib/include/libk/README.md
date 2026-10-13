# libk — the freestanding runtime

libk is the shared freestanding base for the kernel and userland: the start of
the "libc replacement". Which tier a symbol lives in is a **policy decision, not
a style one**, because it decides what a service author is allowed to reach for.

## Tiers

1. **Advertised, safe** — `bytes.h`, `str.h`, `text.h`, `num.h`, `integer.h`, `endian.h`, `bits.h`, `slot_pool.h`, `dma.h`, `mmio.h`, `hash.h`, `printf.h`,
   `metric.h`, `panic.h`.
   Byte operations check lengths against caller-supplied views. Array overloads
   infer the extent; pointer overloads require the caller to supply the real
   allocation size. Writers respect destination capacities. NUL-string readers
   require termination; bounded readers document their scan limits. Integer
   helpers state their preconditions, and checked arithmetic rejects overflow.

2. **Internal runtime floor** — `string.h` (`memcpy`/`memset`/`memmove`/
   `memcmp`, `be32`/`be64`).
   Untyped `(pointer, length)`; the compiler also emits calls to these for
   aggregate copies, so the symbols must exist in every image. **Services must
   not include it**: `userland` builds define `AME_MOCHI_SERVICE`, and the
   header turns that into a build error. The runtime, the kernel and the shared
   format library may include it.

3. **Caller-facing policy layer** — `libmochi` (`mochi/*.h`): `Ring` /
   `Channel`, `Handle`, `Arena`, `boot`, terminal, log, the service protocols.
   Typed messages and protocol endpoints handle common service operations.
   Drivers can still access ring slots and mapped storage explicitly; mapping,
   ownership, deadlines and protocol error policy remain in Mochi/services.

## Rules

- Move bytes with `libk::copy` / `copy_n` / `fill` over `libk::bytes` views —
  never `memcpy`.
- Clear an array or object with `libk::zero(buffer)` / `libk::zero(object)`;
  use `libk::zero(pointer, length)` for dynamic storage. Zeroing includes padding
  and requires a data format with a valid all-zero representation. A bare pointer
  is rejected at compile time to prevent accidentally clearing only the pointer.
- Format numbers with `libk::snprintf` or `libk/num.h` — never a digit loop.
- Allocate from a `mochi::Arena` over a registered region; there is no heap.
- Strings are bounded (`libk/str.h`); the `_n` forms are for text that may cross
  a trust boundary without a terminator.
- A new helper belongs in the safe tier unless it genuinely needs untyped
  access — and even then that is a request to widen the floor, not to add a
  second `memcpy`.

## Placement and contracts

Small pure helpers live inline in their own header; shared formatting and the
compiler runtime remain in `libk/*.cc`. Keep protocol opcodes and wire layouts
in `shared/include/midix/abi`, and encode/decode policy in `mochi/message.h`.
Do not add service-local byte loops or pointer/length wrappers that discard the
actual destination capacity.

Use `move_n` when ranges may overlap; ordinary `copy` operations require disjoint
or identical ranges. Views do not own storage or validate
its lifetime. Typed IPC accepts only trivially copyable messages that fit the
inline payload and validates incoming lengths before writing the output. The
sender must initialise transmitted fields and padding within each message;
bytes after its declared payload are cleared by the ring producer.

## Common operations

- `libk::str::Buffer text(storage)` builds text in a caller-owned array. `append`
  and `format` preserve termination; `length` tracks written bytes and `truncated`
  stays set after any short append. Construction starts an empty string.
- `libk::num::parse_u64(text, length, &value)` parses an exact decimal or `0x`
  token. `parse_u64_digits` accepts an explicit base for fixed-format fields.
  Both reject malformed or overflowing input without changing the output.
- `libk::read_le<T>(view, &value)` decodes a bounded unsigned field without an
  aligned load. `load_le<T>(pointer)` is for a caller-proven fixed-width field.
- `libk::crc32_ieee` and `fnv1a32` provide format checksums, not authentication.
  Incremental CRC uses `kCrc32IeeeInit`, `crc32_ieee_update`, then a complement.
- `checked_align_up` rejects non-power-of-two alignment and arithmetic overflow.
  `div_round_up` requires a nonzero divisor and avoids numerator overflow.
- `write_le<T>` checks the entire destination before writing. `read_bits` reads
  up to 64 little-endian bits from a bounded view without aligned loads.
  `write_bits` validates both the entire field and value before changing bytes,
  preserving adjacent bits.
- `SlotPool` allocates generational tokens over caller-owned metadata in O(1).
  It is single-owner; generation exhaustion retires a slot instead of aliasing
  an old token. It does not own or free the associated storage.
- `dma::publish` and `dma::consume` order device-visible memory. They do not
  perform cache maintenance. Use them with coherent or uniformly non-cacheable
  memory, and use the ring library's acquire/release contract for CPU IPC.
- `Mmio` checks register width, range and alignment over a caller-owned mapping.
  Volatile accesses preserve the requested width; ordering remains explicit in
  the hardware backend.
  `read_fifo32` / `write_fifo32` repeatedly access one register, validate the CPU
  view before the first access, and handle a partial final word without reading
  or writing past the view. Writes zero-pad that word; FIFO space and ordering
  belong to the controller owner.
