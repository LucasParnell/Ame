from pathlib import Path
import importlib.util
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_caps", ROOT / "scripts/mx/check-caps.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def base_manifest():
    return {
        "resources": [{"boot": "uart.mmio", "name": "uart"}, {"boot": "vm.root", "name": "vm"}],
        "channels": [{"kind": "ring", "name": "log_ring", "order": 6}],
        "services": [
            {"name": "io.console", "object": "io.console", "principal": "native_trusted",
             "restart": "always", "caps": ["uart"], "provides": ["console"],
             "uses": [], "start_after": []},
            {"name": "demo", "object": "demo", "principal": "native_sandboxed",
             "restart": "never", "caps": [], "provides": [], "uses": ["console"],
             "start_after": ["io.console"]},
        ],
    }


class CheckCaps(unittest.TestCase):
    def test_consistent_manifest_passes(self):
        self.assertEqual(load_checker().check_manifest(base_manifest()), [])

    def test_dangling_use_is_reported(self):
        m = base_manifest()
        m["services"][1]["uses"] = ["ghost"]
        problems = load_checker().check_manifest(m)
        self.assertTrue(any("uses 'ghost', which nothing provides" in p for p in problems), problems)

    def test_undeclared_cap_is_reported(self):
        m = base_manifest()
        m["services"][1]["caps"] = ["net"]
        problems = load_checker().check_manifest(m)
        self.assertTrue(any("cap 'net' is not a declared resource" in p for p in problems), problems)

    def test_duplicate_provide_and_name_are_reported(self):
        m = base_manifest()
        m["services"].append(dict(m["services"][0]))  # duplicate service name
        problems = load_checker().check_manifest(m)
        self.assertTrue(any("declared more than once" in p for p in problems), problems)
        self.assertTrue(any("already provided" in p for p in problems), problems)

    def test_unknown_principal_and_restart_are_reported(self):
        m = base_manifest()
        m["services"][0]["principal"] = "root"
        m["services"][0]["restart"] = "sometimes"
        problems = load_checker().check_manifest(m)
        self.assertTrue(any("unknown principal" in p for p in problems), problems)
        self.assertTrue(any("unknown restart policy" in p for p in problems), problems)


if __name__ == "__main__":
    unittest.main()
