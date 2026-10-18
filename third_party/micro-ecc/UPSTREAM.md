# micro-ecc

Upstream: https://github.com/kmackay/micro-ecc

Revision: `541b3a78026420a3e369c4c9281c396b5e531113`

License: BSD-2-Clause (LICENSE.txt). Sources are unmodified.
Only secp256r1 is enabled for Ame FIDO2; signing uses uECC's deterministic HMAC-SHA256 nonce implementation.
Upstream documents two differences from RFC6979 in uECC.c.

SHA-256:

- `LICENSE.txt`: `ffd8b033d2df7568c25a98866bd92b1656f7da82d7f813c0b2eb85ec36611193`
- `curve-specific.inc`: `9e9d8d6771731dce2e084a4469d0dd2e9ac6245c8773766198a83f82d24b6313`
- `platform-specific.inc`: `e0f036628b47fb7180bfaeb0903a41edbbcda8e1ef99098456eb3b1cdcb4531e`
- `types.h`: `61bd255c0dca4c69dfc779cc5c1fe1cbff47039d4bb7f37578a88f63b7fca466`
- `uECC.c`: `e9db3217c2e8aacb2e1930004a9236b7502d4f0d44fd5796f45e3923f61eaeba`
- `uECC.h`: `9d480aa6920155f68503064bebefb815a82aab318ab305efdcf04674d2a9cba7`
- `uECC_vli.h`: `a8ff7957855e6520674cad4bcaf8f3e752d40f6c3f7501915c3504c777be1e7a`
