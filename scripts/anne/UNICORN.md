# Offline fastboot analysis

Install the optional [Unicorn Engine](https://www.unicorn-engine.org/docs/tutorial.html)
and Capstone dependencies into the project's virtual environment:

```sh
.venv/bin/python -m pip install -r scripts/anne/requirements-unicorn.txt
```

`emulate-fastboot.py` loads a **raw little-endian AArch64 image**. Its default is
the saved stock ANE-LX1 dump at
`out/anne-device-private/el2-firmware-inspection/fastboot.img`, SHA-256
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
The default base is zero, matching the absolute pointers and offsets in this
dump and the existing analysis ELF. Other images need explicit mapping and
entry choices; `--base` maps bytes without relocating embedded pointers.
An ELF or a factory-loader container must first be converted to its raw payload.

Run the existing command-permission function with synthetic FBLOCK=0,
USERLOCK=1, FRP=1, reproducing the inputs discussed in
[FASTBOOT_EL2.md](../../boards/huawei-anne/FASTBOOT_EL2.md):

```sh
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --profile permission --command flash:kernel --trace
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --profile permission --command flash:fastboot --trace
```

This preset checks the exact stock hash, starts at `0x2abb8`, initializes only
the three lock fields via the pointer at `0x292000`, and supplies a command
string in synthetic RAM. It executes the image's own comparison functions.
On return, x0=1 permits dispatch and x0=0 rejects it. Other commands can reach
additional dependencies and stop. Change `--fblock`, `--userlock`, and `--frp`
to compare input states. These inputs do not measure current device state.

For a different function, provide the entry, initial registers, extra RAM,
and optional in-memory data. For example, observe the known SPSR instruction:

```sh
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --entry 0x10b634 --until 0x10b638 --trace
```

The resulting x1 is `0x3c5`. General setup options are:

```text
--image PATH                 raw AArch64 input image
--entry 0xADDRESS            start execution at a function or instruction
--until 0xADDRESS            stop before this address
--reg x0=0xVALUE             initialize x0..x30, sp, or nzcv (repeatable)
--map 0xADDRESS:0xSIZE       explicitly map zero-filled RAM (repeatable)
--write 0xADDRESS:HEXBYTES   initialize mapped memory (repeatable)
--dump 0xADDRESS:0xSIZE      save memory after execution (repeatable)
--count 100000              instruction limit; maximum 10000000
--timeout-ms 2000           wall time limit; maximum 60000
--trace                     stream observed instructions to trace.jsonl
--output NEW_DIRECTORY      output directory; existing directories rejected
```

Every run saves `report.json` with the image hash, dependency versions, initial
setup assumptions, memory maps, stop reason, registers, fault information, and
last 32 instructions. A temporary private directory is used unless `--output`
is supplied. Put repository-local results under ignored `out/`; reports and
traces can contain firmware code or data. `--write` changes emulator memory
only. The input file is read without modification.

The stack is synthetic RAM at `0x7ff00000..0x7ff0ffff`, the argument page is at
`0x70000000`, and x30 initially targets the return sentinel `0x7ffe0000`.
FP/SIMD is enabled for library routines. Extra RAM is readable/writable;
the captured image is readable/writable/executable to support its globals.
Unmapped/protected memory accesses, emulator errors, and monitor/trap/wait
instructions stop execution. No MMIO responses or SMC results are invented.
A successful function return or `--until` exits 0; faults, traps, or limits
exit 1 with a report. Setup errors exit 2.

This is a function-analysis harness. Starting at offset zero will encounter
boot-stage CPU and hardware dependencies. It does not model a Kirin SoC,
authenticate the boot chain, or demonstrate a full boot or an EL2 handoff.

Validated on 17 October 2026 using the default stock image: `flash:kernel`
returned x0=1 after 1,995 observed instructions and `flash:fastboot` returned
x0=0 after 5,401, with the default synthetic lock inputs. The instruction at
`0x10b634` produced `0x3c5`; executing through `0x10b640` with an explicit
parameter-block mapping stored `c5 03 00 00` at `0x35e80040`. Entry-point
execution stopped at `0xb8` (`msr CNTVOFF_EL2, x1`) after 44 observed
instructions with an emulator exception. These results are saved locally
under `out/anne-device-private/unicorn-*`. The source image hash was preserved.

On Apple Silicon, Unicorn 2.1.4 queries `hw.cachelinesize`. In the Codex
sandbox on this host that query is blocked; Unicorn's fallback reads CTR_EL0
and crashes with SIGILL. The harness checks the query first and reports a
setup error. Run through approved execution outside the sandbox or in a
normal terminal. The native library and firmware do not need patching.

Run synthetic AArch64 regression tests without any private firmware:

```sh
.venv/bin/python -m unittest discover -s tests -p test_anne_unicorn.py -v
```
