# Anne runtime CPU measurements

Measured on the handset on 2026-10-15 with the Mac connected over native CDC,
using `anne-runtime-profile-v1`. Three five-second windows covered 15.008 seconds
with eight CPUs online and no requested application workload. No process was
excluded because of exit, restart, or counter regression.

Percentages below describe **one CPU's time**, calculated from raw nanosecond
deltas across all three windows. Eight fully occupied CPUs would total 800%.

| Process | Average CPU | Observation |
| --- | ---: | --- |
| `nameservice` | 23.393% | Scans client sessions, then yields when no request arrived. |
| `init` | 23.390% | Checks control requests and supervision, then yields when idle. |
| `io.framebuffer` | 0.792% | Bursty: 0.0014%, 0.0014%, then 2.374%; cause not isolated. |
| `drv.i2c.dw` | 0.0783% | Largest consistent driver cost in these quiet windows. |
| `drv.typec.fusb301` | 0.0179% | Type-C service. |
| `io.usb.role` | 0.00954% | Automatic USB role policy. |
| `drv.power.hi6523` | 0.00524% | Charger and source supervision. |
| `drv.usb.dwc2` | 0.00146% | USB controller with the Mac connected. |

Attributed process CPU totals 47.690% of one CPU, or 5.961% of the eight-CPU
capacity. `init` and `nameservice` account for 98.096% of that attributed work.
Their idle paths call `sys_yield()` and remain runnable: yielding is not waiting
for an event. Replacing these scans with correctly notified waits is the first
runtime optimization target. It must preserve supervision, session generation
changes, request delivery, and lost-wakeup protection. No speedup from that change
has been measured yet.

## Collection and limits

Run `diag runtime 5000` in msh. The command reads existing process runtime
counters before and after a quiet interval, then prints results. It adds no
accounting writes to the scheduler hot path. Report output happens after the
second snapshot; earlier terminal output and asynchronous rendering can still
affect later windows, so the framebuffer burst is not established as a recurring
idle cost.

The kernel copies at most eight 64-byte rows under the process lifecycle lock.
The shell retains two fixed snapshots outside its small stack, computes deltas
in separate numeric columns, and sorts a compact index array. PID generations
prevent attributing a replacement process to an earlier instance. A full snapshot
uses several bounded reads; the reported skew was tens of microseconds here.

These are scheduler-accounted process times, including kernel work while that
process is current. Idle time and interrupt work with no current process are not
represented. This is a service ranking, not a complete CPU or function profile.
Keyboard, graphics, IPC throughput, and other active workloads require separate
measurements.

Private captures and raw deltas are in
`out/anne-device-private/runtime-profile-v1/runtime-{1,2,3}.{txt,json}` and
`runtime-summary.json`. The retained boot log confirmed the profiling image,
all eight CPUs, automatic USB switching, and msh readiness at 0.23 seconds on
this boot. That single boot is not a repeatability claim.

Validation: bounded kernel snapshot probe and shell reporting probe passed;
native Release build passed; an eight-CPU synthetic Anne Release boot passed;
all three handset samples and the retained boot-log check completed.
