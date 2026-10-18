# Console status bar

`tty.fb0` owns the bar; the kernel boot renderer and graphics clients do not draw
it. The phone profile reserves a conservative 448-pixel centre cutout across a
128-pixel top strip, with 64-pixel outer margins and 2x console glyphs. This is a
configurable safe area, not a measured notch outline. The existing terminal
starts at y=224; scrolling copies only the terminal viewport. Other console
profiles get a compact 64-pixel strip and automatic terminal geometry below it.
Set `tty.status=0` to disable it. Geometry bindings are `tty.status.height`,
`tty.status.scale`, `tty.status.margin`, and `tty.status.notch` (all pixels except
scale). A strip that cannot fit both panels is disabled.

CPU is the interval's non-idle scheduler time divided by elapsed time and the
number of online CPUs: 100% means all online cores busy. Its cumulative counter
survives process/principal exit. The first sample and CPU-set changes show `--%`
until a valid interval exists. RAM is allocated plus reserved physical pages in
the allocator's managed RAM region, divided by the total managed pages. Used
pages are maintained under the existing allocator lock, so telemetry does not
scan the RAM bitmap with interrupts masked.

The battery field shows **voltage**, not state of charge. The captured phone DT
identifies `hisilicon,hi6555v100_coul` under the PMIC at 0xfff34000. The reader
checks VERSION4=1, reads V_OUT at registers 0x1f4/0x1f5 with a bounded high/low/high
retry, and applies the vendor ADC scale and OTP factory voltage trims from
0x182/0x183. All accesses are byte reads at four-byte PMUSSI strides within the
already mapped PMIC page. No charger settings, gauge initialization, saved SOC,
or interrupt registers are written. It relies on the inherited gauge continuing
to sample; live updates and absolute accuracy still need handset verification.
It does not load Android's additional nonvolatile calibration. Unsupported IDs,
invalid/out-of-range samples, and platforms without this gauge show `BAT --.--V`.
Voltages below 3.50 V use amber text. A percentage/charging indicator requires
state-of-charge and charger policy support, which this bar does not invent.

Primary register/conversion references, pinned to the same vendor revision as
other Anne board mechanisms:

- [Hi6555v100 driver](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/power/hisi/coul/hi6555v100/hisi_hi6555v100_coul.c): `hi6555v100_coul_convert_regval2uv`, `hi6555v100_coul_get_battery_voltage_uv`, `hi6555v100_coul_get_ate_a`, and `hi6555v100_coul_get_ate_b`.
- [Gauge definitions](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/power/hisi/coul/hi6555v100/hisi_hi6555v100_coul.h).
- [Hi6250 PMIC register map](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/ap/platform/hi6250/pmic_interface.h).

Telemetry samples once per second through `MX_DIAG_HEALTH`. Unchanged displayed
values produce no drawing commands. Publication shares the terminal's single
producer, retains partially admitted frames under backpressure, and participates
in the normal retry/deadline loop. No UART/USB output includes the bar.

Host validation is in `tty_status_probe.cc`, run by `tests.test_framebuffer`:
notch/text bounds, unavailable telemetry, normalized multicore CPU usage, CPU-set
changes, redraw suppression, ring saturation, pixel-copy scrolling, and PMIC
identification/byte lanes/voltage conversion/read-only behavior. Scheduler probes
also check the monotonic busy counter across concurrent accounting and principal
reuse. The optional probe output renders the actual font/rasterizer to a PPM
preview. Hardware battery sampling and the physical safe area remain unverified.
