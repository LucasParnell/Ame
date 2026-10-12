# System descriptor

`userland/system.json` is the source for the QEMU and Switch boot profiles. Edit
it to change service selection, dependencies, restart policy, CPU placement,
resource aliases, channel geometry, or fixed channel grants. `profiles` lists
services in each board's boot profile; omitted services are not packaged. Checks
are filtered to that selection.

Each service's `bindings` names a channel, its child-visible boot name, and the
rights to grant. Ring rights are `read`, `write`, `map`, and `duplicate`;
doorbell rights are `wait`, `notify`, and `duplicate`. They are explicit because
a consumer that may wait is not automatically entitled to notify or delegate.
`affinity` is a preferred CPU index; init falls back to CPU 0 when it is offline.
Omitting affinity preserves the scheduler's default placement. `endpoints` lists
pairs that share one geometry; their ring orders must match. Configurable ring
mapping windows reserve the maximum supported ring size, including the header.

The packer validates policy with `scripts/mx/system_descriptor.py`, adds image
hashes, and signs the resulting `manifest.json` in `system.mxb`. The same module
backs `check-caps.py`. Duplicate JSON keys, unknown references, dependency cycles,
oversized names/tables, invalid rights, and invalid CPU/ring values fail the build.
CMake tracks descriptor changes for both init and the signed bundle.

Generated `system_channels.h` supplies channel identifiers and kinds to init;
it contains no grants or ring sizing. Init validates the signed manifest again,
then stores bindings in fixed arrays. Channel lookup is performed during policy
loading; restart and supervision use indexed handles. There is no JSON parsing
or allocation on the IPC data path.

`build/userland/manifest.v0` is a generated compatibility listing installed
alongside the signed JSON. It is not a second policy input. Disk installation
requires this build output so a Switch install cannot fall back to a QEMU list.

The descriptor governs fixed channel wiring. Device discovery, optional GPU
assignment, console configuration, session creation, and capability lifetime
remain init mechanisms. Resource aliases document those bootstrap roles; they
are not a claim that all device grants are dynamically interpreted yet. The GPU
service remains hosted by `dango.manager`, with its conditional endpoint unchanged.
