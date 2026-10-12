from pathlib import Path
import copy
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/mx'))
import system_descriptor as policy
import mksystem


class SystemDescriptor(unittest.TestCase):
    def setUp(self):
        self.path = ROOT / 'userland/system.json'
        self.manifest = policy.load_profile(self.path, 'qemu-virt')

    def test_profiles_and_generated_compatibility_view(self):
        switch = policy.load_profile(self.path, 'switch-t210')
        self.assertEqual([s['name'] for s in switch['services']],
                         ['nameservice', 'io.console', 'io.framebuffer', 'tty.fb0'])
        self.assertEqual(switch['checks'], [])
        self.assertNotIn('dango.manager', mksystem.legacy_view(switch))
        self.assertEqual(policy.check_manifest(switch), [])
        self.assertEqual(len(self.manifest['services']), 16)

    def test_anne_bootstrap_profile_does_not_require_el2_or_qemu_devices(self):
        anne = policy.load_profile(self.path, 'huawei-anne')
        self.assertEqual([s['name'] for s in anne['services']],
                         ['nameservice', 'io.console'])
        self.assertEqual(anne['checks'], [])
        self.assertEqual(policy.check_manifest(anne), [])
        self.assertNotIn('dango.manager', mksystem.legacy_view(anne))
        self.assertEqual({cap for service in anne['services'] for cap in service['caps']},
                         {'uart'})

    def test_fixed_grants_and_actual_cpu_placement_are_preserved(self):
        services = {s['name']: s for s in self.manifest['services']}
        self.assertEqual(services['io.console']['affinity'], 1)
        self.assertEqual(services['dango.manager']['affinity'], 1)
        ping = {b['name']: b for b in services['demo.ping']['bindings']}
        self.assertEqual(ping['ipc.done']['rights'], ['notify', 'duplicate'])
        fs = {b['name']: b for b in services['io.fs']['bindings']}
        self.assertEqual(fs['blk.req.doorbell']['rights'], ['notify'])
        self.assertEqual(fs['blk.reply.doorbell']['rights'], ['wait'])

    def test_cycle_rejected(self):
        self.manifest['services'][1]['start_after'] = ['nameservice']
        self.assertTrue(any('cycle' in p for p in policy.check_manifest(self.manifest)))

    def test_bad_binding_types_and_authority_are_rejected(self):
        binding = self.manifest['services'][1]['bindings'][0]
        for key, value in [('channel', []), ('channel', 'absent'), ('rights', ['vm_create']),
                           ('rights', ['read', 'read']), ('name', 'x' * 32), ('rights', [['read']])]:
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(self.manifest)
                bad['services'][1]['bindings'][0] = {**binding, key: value}
                self.assertTrue(policy.check_manifest(bad))

    def test_malformed_records_fail_without_type_errors(self):
        for key, value in [('principal', []), ('restart', {}), ('affinity', True), ('affinity', 64)]:
            with self.subTest(key=key):
                bad = copy.deepcopy(self.manifest)
                bad['services'][0][key] = value
                self.assertTrue(policy.check_manifest(bad))
        self.manifest['channels'][0]['kind'] = []
        self.assertTrue(policy.check_manifest(self.manifest))

    def test_channel_geometry_and_duplicate_bindings_are_rejected(self):
        for order in (3, 11, True):
            bad = copy.deepcopy(self.manifest)
            bad['channels'][1]['order'] = order
            self.assertTrue(policy.check_manifest(bad))
        bindings = self.manifest['services'][1]['bindings']
        bindings.append(copy.deepcopy(bindings[0]))
        self.assertTrue(policy.check_manifest(self.manifest))

    def test_duplicate_json_keys_rejected(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'invalid.json'
            path.write_text('{"schema":2,"schema":2}')
            with self.assertRaisesRegex(ValueError, 'duplicate JSON key'):
                policy.read_descriptor(path)

    def test_channel_header_contains_identifiers_not_grants(self):
        with tempfile.TemporaryDirectory() as work:
            header = Path(work) / 'channels.h'
            policy.channel_header(self.path, header)
            data = header.read_text()
            self.assertIn('enum class ChannelId', data)
            self.assertNotIn('RIGHT_', data)
            self.assertNotIn('affinity', data)

    def test_endpoint_pair_geometry_rejected(self):
        channel = next(c for c in self.manifest['channels'] if c['name'] == 'blk_req_ring')
        channel['order'] = 10
        self.assertTrue(any('equal orders' in p for p in policy.check_manifest(self.manifest)))
