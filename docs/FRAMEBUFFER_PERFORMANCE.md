# Framebuffer rendering and acceleration

The interactive path is `msh` → terminal byte ring → `tty.fb0` character grid →
framebuffer command ring → `io.framebuffer`. Huawei's kernel boot console uses
a separate bounded renderer and retains its one-row wrap policy.

The character grid already rotates rows and stores dirty column ranges
separately from character data. Scrolling now shifts that small dirty metadata
with the grid and coalesces pending scrolls into one overlap-safe pixel copy.
Only exposed rows and existing unsent changes are redrawn. The old cursor cell
is restored as part of the dirty ranges. Command admission advances state only
after the ring accepts a command. A burst that replaces every row skips copying.

The raster walks text runs in destination scanline order, resolves font pointers
once per run and expands complete glyph rows with fixed-scale stores. Fill and
glyph stores use ordinary normal-memory accesses so the compiler can combine
adjacent pixels. Full-width copies use one contiguous move; inset copies keep
overlap-safe row ordering. The kernel renderer shares the pixel span helpers.
Ordinary shell typing appends a byte instead of redrawing the complete edit line.

Framebuffer arenas remain uniformly Normal Non-Cacheable. Cacheable character
metadata/font data and sequential pixel accesses help, but the framebuffer itself
does not gain CPU caching. Startup, explicit presents and automatic appliance
presents publish stores with a barrier. The native boot console retains its
dirty-row cache maintenance. No SIMD instructions are enabled by these changes.

## Measurement and validation

An AArch64 host comparison against commit
`0d4108bf44f175ea49f2a25014643fddecebe3f2`, compiled at `-O3` with
`-mgeneral-regs-only` in both engines, produced median times across seven samples:

| Operation | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Clear 1080 × 2280 | 565.3 µs | 144.1 µs | 3.92× |
| 28 characters, scale 1 | 2401.5 ns | 846.0 ns | 2.84× |
| 28 characters, scale 2 | 6594.5 ns | 1793.2 ns | 3.68× |
| 28 characters, scale 3 | 12336.7 ns | 2707.3 ns | 4.56× |
| 28 characters, scale 8, right clipped | 39227.9 ns | 10182.1 ns | 3.85× |

These measure cached host memory and exclude IPC/display consumption. They are
not phone timings. A copy reads and writes pixels, so Normal NC read bandwidth
could limit native scrolling even though command count and glyph work decrease.
Measure native copy versus redraw before claiming a native scrolling speedup.

```sh
python3 scripts/test/benchmark-framebuffer.py
python3 -m unittest tests/test_framebuffer.py tests/test_console.py \
  tests/test_msh_parse.py tests/test_usb.py tests/test_anne_framebuffer.py \
  tests/test_panic_screen.py tests/test_mochi_ring.py
```

The raster probe compares against an independent per-pixel oracle, including
hostile coordinates, clipping, scaling, guard pixels and every copy-overlap
direction. The terminal probe replays actual commands into pixel memory and
checks cursor restoration, margins, repeated/coalesced scrolls and writes during
partial command admission/consumption, including multi-command dirty rows.

## DMA assessment

Display scanout already reads the framebuffer with DMA. It is distinct from a
memory-to-memory copy/fill engine. `libk/dma.h` supplies ordering barriers, not
copy hardware. The framebuffer service currently receives pixel memory and
optional display control/GPU endpoints; there is no generic framebuffer blit
DMA driver, command queue or completion contract in this tree.

Large fills and rectangle copies are candidates for a verified board DMA/2D
engine. Controller capabilities must be checked separately: memory copies,
constant fills and strided rectangle transfers are different capabilities; see
the [DMAengine capability descriptions](https://www.kernel.org/doc/html/latest/driver-api/dmaengine/provider.html).
An inherited DSS read channel alone does not establish a supported blit engine.

Enabling one requires controller discovery/ownership, clock/reset/IRQ support,
device-visible addresses and routing/IOMMU policy, buffer lifetime rules and
bounded completion/recovery. An overlapping scroll requires ordered safe row
transfers or staging; a general asynchronous memcpy cannot be assumed to have
memmove semantics. Later drawing and presentation must wait for completion.
Small glyph commands should stay on the CPU unless measurements justify setup
cost. A DMA timeout must establish that the engine has stopped before CPU fallback
can modify the same memory. No speculative DMA registers are programmed here.

## SIMD follow-up

The global `-mgeneral-regs-only` flag matches the current register-save paths:
`CpuContext`, `TrapFrame` and host/guest transitions do not preserve vector
registers. Basic ARM64 FP/NEON support needs all 32 128-bit registers plus FPCR
and FPSR (520 bytes of payload, normally 528 with 16-byte alignment), initialized
per thread and saved before that thread can be dispatched on another CPU.

Start with eager saves on actual thread switches, retaining scalar kernel/IRQ
code. Restore on first dispatch and exception return, configure FP access on
every CPU and isolate host/guest vector state at EL2. This avoids placing vector
state on every syscall's hot frame. Test all registers/control bits across
preemption, blocking IPC, migration, destruction/reuse and guest transitions.
Only then enable SIMD for selected userland objects and benchmark vector fills,
copies and glyph expansion. Shared runtime objects need separate build variants
if kernel code must remain scalar.

General kernel SIMD is a further step: explicit ownership/save scopes and rules
for preemption, nesting and interrupts, or preservation at every relevant entry.
Removing the global compiler restriction before that work is unsafe. SIMD can
reduce CPU work in bulk operations, but does not increase DRAM bandwidth or
remove noncached framebuffer traffic.
