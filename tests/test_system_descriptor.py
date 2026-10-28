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
        self.assertEqual(len(self.manifest['services']), 18)

    def test_anne_bootstrap_profile_does_not_require_el2_or_qemu_devices(self):
        anne = policy.load_profile(self.path, 'huawei-anne')
        self.assertEqual([s['name'] for s in anne['services']],
                         ['nameservice', 'io.console'])
        self.assertEqual(anne['checks'], [])
        self.assertEqual(policy.check_manifest(anne), [])
        self.assertNotIn('dango.manager', mksystem.legacy_view(anne))
        self.assertEqual({cap for service in anne['services'] for cap in service['caps']},
                         {'uart'})

    def test_native_usb_profile_has_separate_hardware_and_input_owners(self):
        anne=policy.load_profile(self.path,'huawei-anne-usb')
        self.assertEqual(policy.check_manifest(anne),[])
        services={s['name']:s for s in anne['services']}
        self.assertEqual(services['dango.manager']['caps'], ['vm'])
        self.assertEqual(services['dango.manager']['affinity'], 1)
        self.assertIn('dango.auth_probe', services)
        self.assertNotIn('drv.usb.xhci',services)
        self.assertEqual(services['io.usb']['start_after'],['drv.usb.dwc2'])
        self.assertEqual(services['msh']['uses'],['fs'])
        self.assertIn('io.fs',services['msh']['start_after'])
        self.assertEqual(services['io.fs']['start_after'],['drv.blk.emmc'])
        self.assertEqual(services['io.fs']['caps'],['blk'])
        fs_bindings={b['name']:b for b in services['io.fs']['bindings']}
        emmc_bindings={b['name']:b for b in services['drv.blk.emmc']['bindings']}
        self.assertEqual(emmc_bindings['emmc.partition']['value'],56)
        self.assertEqual(fs_bindings['blk.req.doorbell']['rights'],['notify'])
        self.assertEqual(fs_bindings['blk.reply.doorbell']['rights'],['wait'])
        self.assertEqual(fs_bindings['blk.buffer']['channel'],emmc_bindings['blk.buffer']['channel'])
        self.assertNotIn('drv.blk.virtio',services)
        hardware={name:{b['boot'] for b in svc['bindings'] if 'boot' in b} for name,svc in services.items()}
        self.assertEqual(hardware['drv.usb.dwc2'],{'dwc2.mmio','dwc2.irq','usb.fence'})
        self.assertEqual(hardware['drv.usb.anne-phy'],{'usb.phy.mmio','usb.phy.control'})
        self.assertFalse(hardware['drv.usb.hid'])
        self.assertFalse(hardware['dev.usb-input'])
        for name in ('drv.typec.fusb301', 'io.usb.role'):
            bindings = {b['name']: b for b in services[name]['bindings']}
            self.assertEqual(bindings['usb.device_only']['value'], 0)
        sources=[b['channel'] for b in services['ui.input']['bindings'] if b['name'] in {'hid.input.ring','test.input.ring'}]
        self.assertEqual(len(set(sources)),2)

    def test_fixed_grants_and_actual_cpu_placement_are_preserved(self):
        services = {s['name']: s for s in self.manifest['services']}
        self.assertEqual(services['io.console']['affinity'], 1)
        self.assertEqual(services['dango.manager']['affinity'], 1)
        ping = {b['name']: b for b in services['demo.ping']['bindings']}
        self.assertEqual(ping['ipc.done']['rights'], ['notify', 'duplicate'])
        fs = {b['name']: b for b in services['io.fs']['bindings']}
        self.assertEqual(fs['blk.req.doorbell']['rights'], ['notify'])
        self.assertEqual(fs['blk.reply.doorbell']['rights'], ['wait'])

    def test_usb_service_authority_and_arena_bounds(self):
        services = {s['name']: s for s in self.manifest['services']}
        self.assertEqual(services['io.usb']['start_after'], ['drv.usb.xhci'])
        self.assertEqual(services['drv.usb.hid']['start_after'], ['io.usb'])
        self.assertEqual(services['drv.usb.hid']['caps'], [])
        self.assertTrue(all(b['name'].startswith('usb.controller.')
                            for b in services['drv.usb.xhci']['bindings']))
        self.assertFalse(any('xhci' in b['name'] for b in services['drv.usb.hid']['bindings']))
        arena = next(c for c in self.manifest['channels'] if c['name'] == 'usb_controller_payload')
        self.assertEqual(arena['kind'], 'arena')
        for size in (0, 1, True, 4097, 1048577):
            bad = copy.deepcopy(self.manifest)
            next(c for c in bad['channels'] if c['name'] == arena['name'])['bytes'] = size
            self.assertTrue(policy.check_manifest(bad))

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

    def test_fs_session_requires_provider_before_client(self):
        manifest = policy.load_profile(self.path, 'huawei-anne-gpu')
        services = {svc['name']: svc for svc in manifest['services']}
        manager = services['dango.manager']
        self.assertEqual(manager['uses'], ['fs'])
        manager['start_after'] = ['io.console']
        self.assertTrue(any('FS session requires' in p for p in policy.check_manifest(manifest)))
        # A transitive dependency is sufficient too.
        manager['start_after'] = ['msh']
        self.assertEqual(policy.check_manifest(manifest), [])
