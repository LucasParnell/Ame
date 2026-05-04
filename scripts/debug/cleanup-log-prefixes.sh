#!/usr/bin/env bash
set -euo pipefail

files=(
  userland/init/main.cc
  userland/dango/manager/main.cc
  userland/dango/auth_probe/main.cc
  userland/demo/ping/main.cc
  userland/console/main.cc
  kernel/vm/vcpu.cc
)

for f in "${files[@]}"; do
  [[ -f "$f" ]] && cp "$f" "$f.bak.log-prefix-cleanup.$(date +%Y%m%d-%H%M%S)"
done

python3 - <<'PY'
from pathlib import Path

def repl(path, pairs):
    p = Path(path)
    s = p.read_text()
    for a, b in pairs:
        s = s.replace(a, b)
    p.write_text(s)

# init already logs under the "init" process tag.
repl("userland/init/main.cc", [
    ('"init: supervision ready\\n"', '"supervision ready\\n"'),
    ('"init: base services ready\\n"', '"base services ready\\n"'),
    ('"init: scheduler tests ready\\n"', '"scheduler tests ready\\n"'),
    ('"init: system ready\\n"', '"system ready\\n"'),

    # Keep subsystem labels where useful, but make them shorter.
    ('"sched: local scheduling ready\\n"', '"local scheduling ready\\n"'),
    ('"sched: remote wake ready\\n"', '"remote wake ready\\n"'),
    ('"sched: fairness check complete\\n"', '"fairness check complete\\n"'),
    ('"ipc: cross-core doorbell ready\\n"', '"cross-core doorbell ready\\n"'),
])

# dango.manager already logs under the "dango.manager" process tag.
repl("userland/dango/manager/main.cc", [
    ('"dango: manager starting\\n"', '"manager starting\\n"'),
    ('"dango: linux artifacts ready\\n"', '"linux artifacts ready\\n"'),
    ('"dango: vm created\\n"', '"vm created\\n"'),
    ('"dango: linux image loaded\\n"', '"linux image loaded\\n"'),
    ('"dango: vcpu stub run complete\\n"', '"vcpu stub run complete\\n"'),
    ('"dango: channel ready\\n"', '"channel ready\\n"'),
    ('"dango: layout ready\\n"', '"layout ready\\n"'),
    ('"dango: starting linux guest\\n"', '"starting linux guest\\n"'),
    ('"dango: vm runner started async\\n"', '"vm runner started async\\n"'),
    ('"dango: service ready\\n"', '"service ready\\n"'),
    ('"dango: linux guest responded\\n"', '"linux guest responded\\n"'),
    ('"dango: linux guest agent started\\n"', '"linux guest agent started\\n"'),

    # Debug/failure strings too, so future logs/errors do not duplicate.
    ('"dango: el2 detection failed\\n"', '"el2 detection failed\\n"'),
    ('"dango: el2 available\\n"', '"el2 available\\n"'),
    ('"dango: layout validation failed\\n"', '"layout validation failed\\n"'),
    ('"dango: creating vm\\n"', '"creating vm\\n"'),
    ('"dango: vm create syscall failed\\n"', '"vm create syscall failed\\n"'),
    ('"dango: vm object invalid\\n"', '"vm object invalid\\n"'),
    ('"dango: inspecting vm\\n"', '"inspecting vm\\n"'),
    ('"dango: vm object ready\\n"', '"vm object ready\\n"'),
    ('"dango: creating guest memory\\n"', '"creating guest memory\\n"'),
    ('"dango: guest memory failed\\n"', '"guest memory failed\\n"'),
    ('"dango: guest memory created\\n"', '"guest memory created\\n"'),
    ('"dango: mapping guest memory\\n"', '"mapping guest memory\\n"'),
    ('"dango: guest memory mapped\\n"', '"guest memory mapped\\n"'),
    ('"dango: inspecting guest memory\\n"', '"inspecting guest memory\\n"'),
    ('"dango: guest memory ready\\n"', '"guest memory ready\\n"'),
    ('"dango: inspecting stage2\\n"', '"inspecting stage2\\n"'),
    ('"dango: stage2 invalid\\n"', '"stage2 invalid\\n"'),
    ('"dango: stage2 ready\\n"', '"stage2 ready\\n"'),
    ('"dango: linux load failed\\n"', '"linux load failed\\n"'),
    ('"dango: copying linux image\\n"', '"copying linux image\\n"'),
    ('"dango: linux image copied\\n"', '"linux image copied\\n"'),
    ('"dango: copying linux initramfs\\n"', '"copying linux initramfs\\n"'),
    ('"dango: linux initramfs copied\\n"', '"linux initramfs copied\\n"'),
    ('"dango: copying linux dtb\\n"', '"copying linux dtb\\n"'),
    ('"dango: linux dtb copied\\n"', '"linux dtb copied\\n"'),
    ('"dango: creating vcpu\\n"', '"creating vcpu\\n"'),
    ('"dango: vcpu object invalid\\n"', '"vcpu object invalid\\n"'),
    ('"dango: vcpu created\\n"', '"vcpu created\\n"'),
    ('"dango: setting vcpu entry\\n"', '"setting vcpu entry\\n"'),
    ('"dango: vcpu entry set\\n"', '"vcpu entry set\\n"'),
    ('"dango: inspecting vcpu\\n"', '"inspecting vcpu\\n"'),
    ('"dango: vcpu ready\\n"', '"vcpu ready\\n"'),
    ('"dango: vcpu stub run failed\\n"', '"vcpu stub run failed\\n"'),
    ('"dango: channel setup failed\\n"', '"channel setup failed\\n"'),
    ('"dango: service failed\\n"', '"service failed\\n"'),
    ('"dango: vm runner create failed\\n"', '"vm runner create failed\\n"'),
])

# dango.auth_probe already logs under "dango.auth_probe".
repl("userland/dango/auth_probe/main.cc", [
    ('"dango: authority check failed\\n"', '"authority check failed\\n"'),
    ('"dango: authority check passed\\n"', '"authority check passed\\n"'),
])

# demo.ping already logs under "demo.ping".
repl("userland/demo/ping/main.cc", [
    ('"demo: affinity ready\\n"', '"affinity ready\\n"'),
])

# io.console already logs with tag "io.console".
repl("userland/console/main.cc", [
    ('"console: ready"', '"ready"'),
])

# Kernel VM log already has kernel tag; remove dango prefix from raw VM notices.
repl("kernel/vm/vcpu.cc", [
    ('"dango: guest boot stub reached\\n"', '"guest boot stub reached\\n"'),
    ('"dango: guest agent started\\n"', '"guest agent started\\n"'),
    ('"dango: guest uart hello\\n"', '"guest uart hello\\n"'),
])
PY

cmake --build build

echo
echo "cleaned duplicated log prefixes"
