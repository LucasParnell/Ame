# Guest memory layout for the lavapipe guest (blocker #1)

How the appliance's guest memory *should* work so a libc guest with a real Vulkan ICD fits, and why it
does not today. Everything below is measured from the tree at `c32ba56`.

## Why it does not fit today

| Fact | Where |
|---|---|
| Guest RAM is 128 MiB at `0x4000_0000` | `kernel/include/anko/vm.h` `kDangoRamBase/Size` |
| Stage 2 installs **4 KiB leaves only** | `kernel/vm/stage2.cc` — the L3 leaf descriptor is the only one written; L1/L2 links are tables, never blocks |
| The mapping budget is a **fixed soft table** | `Stage2AddressSpace::mappings[40960]`, 24 B/entry ⇒ ~960 KiB of kernel BSS |
| ⇒ total addressable guest space is capped at 40960 × 4 KiB = **160 MiB** | RAM alone is 32768 entries; the device windows (BAR 16 MiB = 4096, staging 3 MiB = 768, shm 1 MiB = 256, channel 128 KiB = 32) take it past 37k |
| The initrd load window is 16 MiB | `linux-artifacts/dts/dango-linux-gpu.dts.in`, `scripts/linux/dango-load.env` |

The lavapipe guest needs ~186 MiB unpacked / **58 MiB compressed** for the rootfs and its acceptance was
measured at **512 MiB RAM**. Both numbers are outside today's hard bounds, and neither is fixed by
bumping the manifest — the kernel constants are the real limit.

## How it should work

1. **Support level-2 block mappings in stage 2, and use them for guest RAM.** 2 MiB blocks are the
   architectural answer (that is what a real stage-2 does for RAM): a 512 MiB guest becomes 256 block
   entries instead of 131072 page entries. The leaf writer in `stage2.cc` gains a `S2_DESC_BLOCK`
   path, and `Stage2Mapping` gains a granularity so unmap/teardown can split or drop a whole block.
   Device windows (BAR/staging/shm/channel) keep 4 KiB pages, because they need page-granular rights.
2. **Pick a RAM size from a measurement, not a guess.** With blocks, the soft-table cost stops mattering,
   so the number should come from the smallest RAM the libc guest + lavapipe actually boots and runs the
   acceptance in. 256 MiB is the first thing to try; 512 MiB is the known-good figure.
3. **Deliver the rootfs as a disk, not as an initrd.** 58 MiB compressed will not fit a 16 MiB initrd
   window, and growing that window means reserving a large contiguous chunk at a fixed address for a
   one-shot copy. The guest already has a virtio-blk driver and a durable `/data` path, so the right
   answer for a 186 MiB userland is a **virtio-blk root** (or a small initrd that `switch_root`s to it).
4. **Move the fixed windows with RAM.** The channel (0x4800_0000), staging (0x4900_0000) and shm
   (0x4A00_0000) windows sit immediately above RAM. Growing RAM past 0x4800_0000 collides with them, so
   the windows move up in the same change. This is safe because the binding in
   `midix/abi/dango_channel_guest.h` is the single source: change it there, regenerate the DTS, and the
   kernel, manager and guest driver all follow (the asserts in `vm.h` bind the two together).
5. **"Which guest" must reach the kernel, not just the artifacts.** `kDangoRamSize` and the window bases
   are kernel constants, so the selection has to be an Anko build option as well as an artifact
   selection. Plumbed now: `DANGO_LINUX_GUEST=minimal|lavapipe` (manifest field + CMake cache var, read
   by `dango.manager`). Still needed: an `ANKO_GUEST_LAYOUT` option that feeds the same choice into the
   `vm.h` constants, with a `static_assert` tying the two so a mismatch is a build failure rather than a
   guest that faults.

## Order

1. stage-2 block mappings (kernel) + a host test that a 512 MiB RAM map fits the budget.
2. `ANKO_GUEST_LAYOUT` + moved windows, defaulting to today's `minimal` layout (no behaviour change).
3. virtio-blk rootfs delivery for the lavapipe guest.
4. Re-run `AME_VERIFY_VULKAN` — the standalone executor acceptance — with the guest actually booting it.

Until 1–3 land, `AME_VERIFY_VULKAN` stays the *standalone* proof and the in-appliance claim stays open.

## Stage-2 block mappings — exact change list

Read against `kernel/vm/stage2.cc` at `daa3dc8`. Blocks are the right first step because they are both the
robustness fix and the **smallest footprint**: one descriptor per 2 MiB instead of 512, and one soft-table
record instead of 512.

1. **Constants.** `S2_DESC_BLOCK_PAGE` today is the *L3 page* descriptor (`VALID|TABLE`); add
   `S2_DESC_BLOCK` (`VALID` with no `TABLE`) plus `kStage2BlockSize = 2 MiB` / `kStage2BlockPages = 512`.
   Stage-2 block descriptors must be 2 MiB aligned on both `ipa` and `pa`.
2. **Accounting.** `Stage2Mapping` gains a granularity (`u8 pages` or `bool block`), and `mapped_pages`
   then counts *pages covered* so the RAM/device budget stays meaningful. Note `record_mapping` currently
   relies on `mapped_pages` also being the next free record slot (`stage2.cc:130-156`) — that invariant
   breaks with blocks, so use the existing sparse-map free-slot scan unconditionally and cap *records*
   separately from *covered pages*.
3. **`stage2_map_block(s2, ipa, pa, rights, mem_type)`** — validate 2 MiB alignment, trap overlap and
   `ipa_bits`; require the L2 slot to be empty; install the block descriptor and one record.
4. **Split on page map.** `stage2_map_page_ex` currently returns `E_BUSY` when the L2 slot holds a block
   (`stage2.cc:236-238`). Replace with a split: allocate an L3 table, write 512 leaves derived from the
   block (same `pa` base + i·4 KiB, same attrs/mem_type), replace the L2 link, replace the block record
   with 512 page records, then write the requested leaf.
5. **Unmap + reclaim.** `unmap_page` must detect a covering block, split, clear the leaf, then *reclaim*:
   if all 512 leaves of the 2 MiB unit are absent, free the L3 table and restore a single block record.
6. **Lookup.** `stage2_lookup_page` must resolve a page inside a block (`pa = block_base + offset`,
   rights/mem_type from the block record) — it is used by the device-revoke path.
7. **Overlap.** `stage2_validate_no_overlap` must treat a mapped block as covering its full 2 MiB.
8. **Teardown.** Blocks own no tables; `prune_tables`/`stage2_destroy` free L3 tables exactly as today.
9. **Use it for RAM.** Map the guest RAM range with `map_block` (256 records for 512 MiB instead of
   131072); device windows (BAR/staging/shm/channel) keep 4 KiB pages because they need page-granular
   rights and revocation.
10. **Tests** (`tests/kernel/`): block-mapped RAM accounting and coverage; page-map inside a block splits
    with neighbours and attributes intact; unmap back to a block reclaims; overlap detection across a
    block boundary; teardown with a mix of blocks and pages; and a capacity test that the old 160 MiB
    ceiling is gone.

## The footprint problem, quantified (why blocks are also the *smallest* option)

`Stage2AddressSpace` is embedded in `Vm` (`kernel/include/anko/vm.h:522`) and `kMaxVms = 4`, so the soft
table is **static BSS per VM**, allocated whether or not the VM is ever used:

| | records | bytes/record | per VM | all 4 VMs |
|---|---|---|---|---|
| today (`mappings[40960]`, 4 KiB leaf) | 40960 | 32 | **1.25 MiB** | **5 MiB** |
| with 2 MiB blocks, table capped at 2048 | 2048 | 32 | **64 KiB** | **256 KiB** |

and the same array is what caps the guest at 160 MiB. So the block change is not just "make 512 MiB
fit" — it is the *right* fix for the footprint too: one descriptor and one record per 2 MiB means a small
fixed table can still cover 4 GiB.

**Retune that follows** (do it with the block change, not after):

- Keep `kMaxStage2Mappings` as the *record* budget, but justify its value from the block-based worst case
  (4 GiB of address space / 2 MiB = 2048 records, plus headroom for device pages), and shrink it from
  40960 accordingly. `mapped_pages` already counts records, so the call sites that budget against it
  (`vm_channel.cc` headroom, `vm.h` assert) need only their *expected* numbers updated, not their shape.
- Device windows keep 4 KiB pages, so leave headroom in the record budget for the worst case a caller can
  legitimately produce (BAR 16 MiB = 4096 pages is the largest today).
- The `static_assert` at `vm.h:765` becomes: RAM is mapped as blocks, so
  `kDangoRamPages / kStage2BlockPages + device_records <= kMaxStage2Mappings`.
