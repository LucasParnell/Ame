from pathlib import Path
import os
import base64
import re
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/mx'))
import system_descriptor as policy



def crypto_python():
    """Return a Python interpreter that can import cryptography."""
    for candidate in (sys.executable, str(ROOT / '.venv/bin/python3')):
        try:
            result = subprocess.run([candidate, '-c', 'import cryptography'],
                                    capture_output=True, text=True)
        except FileNotFoundError:
            continue
        if result.returncode == 0:
            return candidate
    return None


def read_aaguid():
    text = (ROOT / 'userland/usb/functions/fido2/attestation.h').read_text()
    match = re.search(r'kAaguid\s*\[16\]\s*=\s*\{([^}]*)\}', text, re.S)
    if not match:
        raise AssertionError('kAaguid[16] not found in attestation.h')
    values = [int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]{2}', match.group(1))]
    if len(values) != 16:
        raise AssertionError(f'expected 16 AAGUID bytes, found {len(values)}')
    return bytes(values)


def parse_attestation(text):
    values = {}
    for line in text.splitlines():
        key, separator, value = line.partition('=')
        if separator and key.startswith('attestation.'):
            values[key] = value
    return values


def build_probe(work, name, attestation_dir=None):
    probe = str(Path(work) / name)
    command = [
        'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I' + str(ROOT / 'tests/mochi_host'),
        '-I' + str(ROOT / 'shared/include'),
        '-I' + str(ROOT / 'userland/lib/include'),
        '-I' + str(ROOT / 'third_party/monocypher'),
        '-I' + str(ROOT / 'third_party/micro-ecc'),
        '-I' + str(ROOT / 'third_party/tiny-aes'),
        '-DAES256=1', '-DCBC=1', '-DECB=0', '-DCTR=0',
        '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
        '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
        '-DuECC_SUPPORT_COMPRESSED_POINT=0',
    ]
    if attestation_dir is not None:
        command += ['-I' + str(attestation_dir), '-DAME_FIDO2_ATTESTATION=1']
    command += [
        '-idirafter', str(ROOT / 'lib/include'), str(ROOT / 'tests/fido2_probe.cc'),
        str(ROOT / 'shared/format/integrity.cc'), '-x', 'c++',
        str(ROOT / 'lib/libk/printf.cc'),
        str(ROOT / 'third_party/monocypher/monocypher.c'),
        str(ROOT / 'third_party/monocypher/monocypher-ed25519.c'),
        str(ROOT / 'third_party/micro-ecc/uECC.c'),
        str(ROOT / 'third_party/tiny-aes/aes.c'),
        '-o', probe,
    ]
    return probe, subprocess.run(command, capture_output=True, text=True)


def verify_es256(work, prefix, public_key_hex, message_hex, signature_hex):
    point = bytes.fromhex('04' + public_key_hex)
    spki = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200') + point
    public = Path(work) / f'{prefix}.pub.pem'
    public.write_text('-----BEGIN PUBLIC KEY-----\n' + base64.b64encode(spki).decode() +
                      '\n-----END PUBLIC KEY-----\n')
    signature = Path(work) / f'{prefix}.sig.der'
    message = Path(work) / f'{prefix}.message'
    signature.write_bytes(bytes.fromhex(signature_hex))
    message.write_bytes(bytes.fromhex(message_hex))
    return subprocess.run([
        'openssl', 'dgst', '-sha256', '-verify', str(public),
        '-signature', str(signature), str(message),
    ], capture_output=True, text=True)


class Fido2(unittest.TestCase):
    def test_registration_signing_transport_and_host_activation(self):
        with tempfile.TemporaryDirectory(prefix='ame-fido2-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'tests/mochi_host'),
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'userland/lib/include'),
                '-I' + str(ROOT / 'third_party/monocypher'),
                '-I' + str(ROOT / 'third_party/micro-ecc'),
                '-I' + str(ROOT / 'third_party/tiny-aes'),
                '-DAES256=1', '-DCBC=1', '-DECB=0', '-DCTR=0',
                '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
                '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
                '-DuECC_SUPPORT_COMPRESSED_POINT=0',
                '-idirafter', str(ROOT / 'lib/include'), str(ROOT / 'tests/fido2_probe.cc'),
                str(ROOT / 'shared/format/integrity.cc'), '-x', 'c++',
                str(ROOT / 'lib/libk/printf.cc'),
                str(ROOT / 'third_party/monocypher/monocypher.c'),
                str(ROOT / 'third_party/monocypher/monocypher-ed25519.c'),
                str(ROOT / 'third_party/micro-ecc/uECC.c'),
                str(ROOT / 'third_party/tiny-aes/aes.c'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('fido2 registration, signing and lazy USB activation probe ok', result.stdout)
            # Verify the new signer independently through OpenSSL with the RFC6979 public key.
            signature = next(line.split('=', 1)[1] for line in result.stdout.splitlines()
                             if line.startswith('es256.der='))
            point = bytes.fromhex('0460FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6'
                                  '7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299')
            spki = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200') + point
            public = Path(work) / 'public.pem'
            public.write_text('-----BEGIN PUBLIC KEY-----\n' + base64.b64encode(spki).decode() +
                              '\n-----END PUBLIC KEY-----\n')
            sig, message = Path(work) / 'signature.der', Path(work) / 'message'
            sig.write_bytes(bytes.fromhex(signature)); message.write_bytes(b'sample')
            verified = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public),
                                       '-signature', str(sig), str(message)], capture_output=True, text=True)
            self.assertEqual(verified.returncode, 0, verified.stderr)



    def test_packed_attestation_self_and_certificate(self):
        python = crypto_python()
        if not python:
            self.skipTest('cryptography is not available')
        aaguid = read_aaguid()
        with tempfile.TemporaryDirectory(prefix='ame-fido2-attest-') as work:
            work = Path(work)
            seed = work / 'seed.bin'
            seed.write_bytes(bytes([1, 2, 3, 4, 5, 6, 7, 8]) + bytes(24))
            ca_key, ca_cert = work / 'ca.key', work / 'ca.pem'
            header, leaf = work / 'attestation_cert.h', work / 'leaf.der'

            def run_attestation(*args):
                return subprocess.run(
                    [python, str(ROOT / 'scripts/fido2/attestation.py'), *map(str, args)],
                    capture_output=True, text=True)

            result = run_attestation('init-ca', '--key', ca_key, '--cert', ca_cert)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = run_attestation(
                'issue', '--seed', seed,
                '--aaguid-header', ROOT / 'userland/usb/functions/fido2/attestation.h',
                '--ca-key', ca_key, '--ca-cert', ca_cert,
                '--out-header', header, '--out-cert', leaf)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = run_attestation(
                'verify', '--cert', leaf, '--ca', ca_cert,
                '--aaguid-header', ROOT / 'userland/usb/functions/fido2/attestation.h')
            self.assertEqual(result.returncode, 0, result.stderr)

            probe, result = build_probe(work, 'probe-self')
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('fido2 registration, signing and lazy USB activation probe ok', result.stdout)
            values = parse_attestation(result.stdout)
            self.assertEqual(values.get('attestation.fmt'), 'packed')
            self.assertEqual(values.get('attestation.aaguid'), aaguid.hex())
            self.assertNotIn('attestation.cert', values)
            verified = verify_es256(
                work, 'self', values['attestation.public'],
                values['attestation.authdata'] + values['attestation.clienthash'],
                values['attestation.sig'])
            self.assertEqual(verified.returncode, 0, verified.stderr)

            probe, result = build_probe(work, 'probe-cert', work)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            values = parse_attestation(result.stdout)
            self.assertEqual(values.get('attestation.fmt'), 'packed')
            self.assertEqual(values.get('attestation.aaguid'), aaguid.hex())
            self.assertIn('attestation.cert', values)
            verified = verify_es256(
                work, 'cert', values['attestation.public'],
                values['attestation.authdata'] + values['attestation.clienthash'],
                values['attestation.sig'])
            self.assertEqual(verified.returncode, 0, verified.stderr)

            leaf_pem = work / 'leaf.pem'
            result = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-in', str(leaf),
                                     '-out', str(leaf_pem)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(['openssl', 'verify', '-CAfile', str(ca_cert), str(leaf_pem)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            checker = work / 'check_cert.py'
            checker.write_text("""import sys
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID, ObjectIdentifier
cert = x509.load_der_x509_certificate(open(sys.argv[1], 'rb').read())
ca = x509.load_pem_x509_certificate(open(sys.argv[2], 'rb').read())
aaguid = bytes.fromhex(sys.argv[3])
assert cert.version == x509.Version.v3
for oid in (NameOID.COUNTRY_NAME, NameOID.ORGANIZATION_NAME,
            NameOID.ORGANIZATIONAL_UNIT_NAME, NameOID.COMMON_NAME):
    assert cert.subject.get_attributes_for_oid(oid)
assert cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca is False
usage = cert.extensions.get_extension_for_class(x509.KeyUsage).value
assert usage.digital_signature and not usage.key_cert_sign
value = cert.extensions.get_extension_for_oid(
    ObjectIdentifier('1.3.6.1.4.1.45724.1.1.4')).value
assert value.value == b'\x04\x10' + aaguid
ca.public_key().verify(cert.signature, cert.tbs_certificate_bytes,
                       ec.ECDSA(hashes.SHA256()))
""")
            result = subprocess.run([python, str(checker), str(leaf), str(ca_cert), aaguid.hex()],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            message = values['attestation.authdata'] + values['attestation.clienthash']
            tampered = ('00' if message[:2] != '00' else '01') + message[2:]
            verified = verify_es256(work, 'tampered', values['attestation.public'],
                                    tampered, values['attestation.sig'])
            self.assertNotEqual(verified.returncode, 0)

    def test_private_seed_creation_and_header_validation(self):
        script = ROOT / 'scripts/fido2/seed.py'
        with tempfile.TemporaryDirectory() as work:
            seed, header = Path(work) / 'seed', Path(work) / 'header.h'
            def run(*args):
                return subprocess.run([sys.executable, str(script), *map(str, args)],
                                      capture_output=True, text=True)
            self.assertEqual(run('--create', seed).returncode, 0)
            saved = seed.read_bytes()
            self.assertEqual(len(saved), 32)
            self.assertNotEqual(saved, bytes(32))
            if os.name == 'posix':
                self.assertEqual(stat.S_IMODE(seed.stat().st_mode), 0o600)
            self.assertNotEqual(run('--create', seed).returncode, 0)
            self.assertEqual(saved, seed.read_bytes())
            self.assertEqual(run('--header', header, '--seed', seed).returncode, 0)
            self.assertIn('MX_FIDO2_SEED_VERSION,1,', header.read_text())
            self.assertEqual(run('--header', header).returncode, 0)
            self.assertIn('MX_FIDO2_SEED_VERSION,0,', header.read_text())
            seed.write_bytes(bytes(32))
            self.assertNotEqual(run('--header', header, '--seed', seed).returncode, 0)
            seed.write_bytes(b'short')
            self.assertNotEqual(run('--header', header, '--seed', seed).returncode, 0)

    def test_policy_keeps_fido_cold_and_grants_only_private_inputs(self):
        path = ROOT / 'userland/system.json'
        manifest = policy.load_profile(path, 'huawei-anne-usb')
        services = {s['name']: s for s in manifest['services']}
        fido = services['usb.fn.fido2']
        self.assertEqual(fido['activation'], 'usb-host')
        self.assertEqual(fido['start_after'], ['io.usb.gadget', 'tty.fb0', 'io.fs'])
        self.assertEqual(fido['uses'], ['fs'])
        self.assertEqual(fido['caps'], [])
        bindings = {b['name']: b for b in fido['bindings']}
        for name in ('fido2.seed', 'presence.mmio'):
            self.assertEqual(bindings[name]['rights'], ['read', 'map'])
        for service in services.values():
            if service is not fido:
                self.assertNotIn('usb.fn.fido2', service['start_after'])
                self.assertFalse(any(b.get('boot') == 'fido2.seed' for b in service['bindings']))
        for profile in ('qemu-virt', 'huawei-anne', 'switch-t210'):
            self.assertNotIn('usb.fn.fido2', [s['name'] for s in policy.load_profile(path, profile)['services']])
        fido['activation'] = []
        self.assertTrue(policy.check_manifest(manifest))
        fido['activation'] = 'usb-host'
        services['msh']['start_after'].append('usb.fn.fido2')
        self.assertTrue(any('host-activated' in issue for issue in policy.check_manifest(manifest)))


if __name__ == '__main__':
    unittest.main()
