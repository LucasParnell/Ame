"""System policy shared by packaging, validation, and init's channel identifiers.

The JSON owns service selection and grants. Generated C++ contains identifiers
only; init reads rights, ring geometry, and CPU placement from the signed policy.
"""
from __future__ import annotations

import argparse
import copy
import json
import re
from pathlib import Path

PRINCIPALS = {"native_trusted", "native_sandboxed"}
RESTARTS = {"always", "never"}
RIGHTS = {"arena": {"read", "write", "map", "duplicate"}, "ring": {"read", "write", "map", "duplicate"},
          "doorbell": {"wait", "notify", "duplicate"}}


def unique_object(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_descriptor(path: Path) -> dict:
    document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    if not isinstance(document, dict) or document.get("schema") != 2:
        raise ValueError(f"{path}: expected system descriptor schema 2")
    return document


def check_manifest(manifest: dict) -> list[str]:
    problems: list[str] = []
    if not isinstance(manifest, dict):
        return ["manifest: expected an object"]

    def name(value, context: str, limit: int = 32) -> bool:
        valid = isinstance(value, str) and 0 < len(value) < limit and bool(
            re.fullmatch(r"[A-Za-z0-9_.-]+", value))
        if not valid:
            problems.append(f"{context}: invalid name {value!r}")
        return valid

    def records(key: str, limit: int) -> list[dict]:
        values = manifest.get(key, [])
        if not isinstance(values, list) or len(values) > limit:
            problems.append(f"{key}: expected at most {limit} records")
            return []
        result, seen = [], set()
        for entry in values:
            if not isinstance(entry, dict) or not name(entry.get("name"), key, 48 if key == "channels" else 32):
                problems.append(f"{key}: invalid record")
                continue
            if entry["name"] in seen:
                problems.append(f"{key}: a {key} name is declared more than once")
            seen.add(entry["name"])
            result.append(entry)
        return result

    def names(record: dict, key: str) -> list[str]:
        values = record.get(key, [])
        if not isinstance(values, list) or any(not name(v, key) for v in values):
            problems.append(f"service {record['name']}: {key} must be an array of names")
            return []
        if len(values) != len(set(values)):
            problems.append(f"service {record['name']}: duplicate {key}")
        return values

    services = records("services", 32)
    resources = {r["name"]: r for r in records("resources", 64)}
    channels = {c["name"]: c for c in records("channels", 128)}
    by_name = {s["name"]: s for s in services}
    for resource in resources.values():
        name(resource.get("boot"), "resource boot name")
    for channel in channels.values():
        kind = channel.get("kind")
        if not isinstance(kind, str) or kind not in RIGHTS:
            problems.append(f"channel {channel['name']}: unknown kind")
        elif kind == "ring":
            order = channel.get("order")
            if type(order) is not int or not 4 <= order <= 10:
                problems.append(f"channel {channel['name']}: order must be in [4, 10]")
        elif kind == "arena":
            size = channel.get("bytes")
            if type(size) is not int or not 4096 <= size <= 1048576 or size % 4096:
                problems.append(f"channel {channel['name']}: arena bytes must be page aligned in [4096, 1048576]")
        elif "order" in channel:
            problems.append(f"channel {channel['name']}: doorbells have no order")
    # The console's log multiplexor uses the shared log ABI geometry.
    if "init_log_ring" in channels and channels["init_log_ring"].get("order") != 6:
        problems.append("init_log_ring: must use log ABI order 6")
    for endpoint in records("endpoints", 64):
        rings = names(endpoint, "rings")
        members = [channels.get(r) for r in rings]
        if len(members) != 2 or any(not c or c.get("kind") != "ring" for c in members):
            problems.append(f"endpoint {endpoint['name']}: expected two declared rings")
        elif members[0].get("order") != members[1].get("order"):
            problems.append(f"endpoint {endpoint['name']}: rings must have equal orders")
    provided: dict[str, str] = {}
    dependencies: dict[str, list[str]] = {}
    for svc in services:
        for provision in names(svc, "provides"):
            if provision in provided:
                problems.append(f"service {svc['name']}: provides {provision!r} already provided by {provided[provision]}")
            provided[provision] = svc["name"]
    for svc in services:
        label = f"service {svc['name']}"
        if not isinstance(svc.get("principal"), str) or svc["principal"] not in PRINCIPALS:
            problems.append(f"{label}: unknown principal {svc.get('principal')!r}")
        if not isinstance(svc.get("restart"), str) or svc["restart"] not in RESTARTS:
            problems.append(f"{label}: unknown restart policy {svc.get('restart')!r}")
        name(svc.get("object"), label + " object")
        if "affinity" in svc and (type(svc["affinity"]) is not int or not 0 <= svc["affinity"] < 64):
            problems.append(f"{label}: affinity must name a CPU in [0, 63]")
        for used in names(svc, "uses"):
            if used not in provided:
                problems.append(f"{label}: uses {used!r}, which nothing provides")
        dependencies[svc["name"]] = names(svc, "start_after")
        for dep in dependencies[svc["name"]]:
            if dep not in by_name:
                problems.append(f"{label}: start_after unknown service {dep!r}")
        caps = names(svc, "caps")
        if len(",".join(caps)) >= 64:
            problems.append(f"{label}: capability alias list exceeds init storage")
        for cap in caps:
            if cap not in resources:
                problems.append(f"{label}: cap {cap!r} is not a declared resource")
        bindings = svc.get("bindings", [])
        if not isinstance(bindings, list) or len(bindings) > 32:
            problems.append(f"{label}: expected at most 32 bindings")
            continue
        boot_names = set()
        for binding in bindings:
            if not isinstance(binding, dict) or not name(binding.get("name"), label + " binding"):
                problems.append(f"{label}: invalid binding")
                continue
            boot_name = binding["name"]
            if boot_name in boot_names:
                problems.append(f"{label}: duplicate boot name {boot_name!r}")
            boot_names.add(boot_name)
            if "value" in binding:
                if type(binding["value"]) is not int or not 0 <= binding["value"] <= 0xffffffffffffffff or "channel" in binding or "boot" in binding:
                    problems.append(f"{label}: invalid value binding")
                continue
            if "boot" in binding:
                name(binding["boot"],label+" hardware boot name")
                if "channel" in binding or binding["boot"] not in {r["boot"] for r in resources.values()}:
                    problems.append(f"{label}: hardware binding must name a declared boot resource")
                rights = binding.get("rights")
                if not isinstance(rights,list) or not rights or len(rights)!=len(set(rights)) or any(r not in {"read","write","map","wait","notify","duplicate"} for r in rights):
                    problems.append(f"{label}: invalid hardware rights")
                continue
            channel_name = binding.get("channel")
            channel = channels.get(channel_name) if isinstance(channel_name, str) else None
            rights = binding.get("rights")
            kind = channel.get("kind") if channel else None
            allowed = RIGHTS.get(kind, set()) if isinstance(kind, str) else set()
            if not channel or not isinstance(rights, list) or not rights or any(
                    not isinstance(r, str) or r not in allowed for r in rights):
                problems.append(f"{label}: unknown channel or invalid binding rights")
            elif len(rights) != len(set(rights)):
                problems.append(f"{label}: duplicate binding rights")
    visiting, visited = set(), set()

    def visit(service: str) -> None:
        if service in visiting:
            problems.append(f"service {service}: dependency cycle")
            return
        if service in visited:
            return
        visiting.add(service)
        for dependency in dependencies.get(service, []):
            visit(dependency)
        visiting.remove(service)
        visited.add(service)

    for service in dependencies:
        visit(service)
    return problems


def load_profile(path: Path, profile: str) -> dict:
    descriptor = read_descriptor(path)
    profiles = descriptor.pop("profiles", {})
    selected = profiles.get(profile) if isinstance(profiles, dict) else None
    overrides = {}
    if isinstance(selected,dict):
        overrides = selected.get("overrides",{})
        selected = selected.get("services")
    if not isinstance(selected, list) or any(not isinstance(s, str) for s in selected) or len(selected) != len(set(selected)):
        raise ValueError(f"unknown or invalid system profile: {profile}")
    problems = check_manifest(descriptor)
    if problems:
        raise ValueError("\n".join(problems))
    services = {s["name"]: s for s in descriptor["services"]}
    if any(s not in services for s in selected):
        raise ValueError(f"{profile}: profile selects an unknown service")
    descriptor["services"] = [copy.deepcopy(services[s]) for s in selected]
    if len(selected)>24:
        raise ValueError(f"{profile}: selected services exceed registry capacity")
    for service in descriptor["services"]:
        if service["name"] in overrides:
            service.update(overrides[service["name"]])
    descriptor["checks"] = [c for c in descriptor.get("checks", []) if c["service"] in selected]
    problems = check_manifest(descriptor)
    if problems:
        raise ValueError("\n".join(problems))
    return descriptor


def channel_header(path: Path, output: Path) -> None:
    descriptor = read_descriptor(path)
    problems = check_manifest(descriptor)
    if problems:
        raise ValueError("\n".join(problems))
    channels = descriptor["channels"]
    for c in channels:
        if c["name"] == "count" or not re.fullmatch(r"[a-z][a-z0-9_]*", c["name"]):
            raise ValueError("channel identifiers must be lowercase C++ identifiers")
    lines = ["#pragma once", "// Generated channel identifiers. Policy remains in the signed manifest.",
             "enum class ChannelId : unsigned {"]
    lines += [f"    {c['name']}," for c in channels]
    lines += ["    count", "};", "struct ChannelName { const char* name; bool ring; unsigned arena_bytes; };",
              "inline constexpr ChannelName kChannelNames[] = {"]
    lines += [f'    {{"{c["name"]}", {"true" if c["kind"] == "ring" else "false"}, {c.get("bytes", 0)}}},' for c in channels]
    lines += ["};", ""]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--channel-header", type=Path, required=True)
    args = parser.parse_args()
    channel_header(args.descriptor, args.channel_header)
