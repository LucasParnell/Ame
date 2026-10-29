#!/usr/bin/env python3
"""Localhost decoder for the panic-screen QR payload.

The panic screen carries a URL whose fragment is an 'AP' bundle: optional raw
LZMA1 body, unpadded base64url. Scanning it with a phone gets you a URL that
resolves nowhere, which is useless while debugging on a desk. This serves the
decoded report instead.

    scripts/panic-decode-server.py [--port 8765] [--host 127.0.0.1]

Then open http://127.0.0.1:8765/ and paste the scanned text, or hit

    http://127.0.0.1:8765/d?u=<urlencoded url>
    http://127.0.0.1:8765/d/<fragment>
    POST /decode   (body = url or fragment) -> JSON

The bundle format lives in exactly one place: tests/panic_bundle.py. This file
only wraps it, so the decoder the tests trust is the decoder you are reading.
"""
from __future__ import annotations

import argparse
import html
import json
import ssl
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import panic_bundle  # noqa: E402

def decode(text: str) -> dict:
    text = (text or '').strip()
    if not text:
        raise ValueError('nothing pasted')
    return panic_bundle.decode_bundle(text)


def _row(label: str, value: str) -> str:
    return f'<tr><th>{html.escape(label)}</th><td>{value}</td></tr>'


def render_report(r: dict) -> str:
    regs = r['regs']
    words = ' '.join(f'{v:016x}' for v in r['x']) or '<span class="dim">none captured</span>'
    frames = ' '.join(f'{v:016x}' for v in r['bt']) or '<span class="dim">no frames</span>'
    ratio = r['raw_len'] / r['compressed_len'] if r['compressed_len'] else 0.0

    rows = [
        _row('reason', f'<b class="reason">{html.escape(r["reason"])}</b>'),
        _row('core', str(r['core'])),
        _row('build', html.escape(r['build'])),
        _row('boot EL', str(r['boot_el'])),
        _row('esr', f'<code>{regs["esr"]:016x}</code>'),
        _row('far', f'<code>{regs["far"]:016x}</code>'),
        _row('elr', f'<code>{regs["elr"]:016x}</code>'),
        _row('spsr', f'<code>{regs["spsr"]:016x}</code>'),
        _row('sp / fp / lr', f'<code>{regs["sp"]:016x} {regs["fp"]:016x} {regs["lr"]:016x}</code>'),
        _row('x0..', f'<code>{words}</code>'),
        _row('backtrace', f'<code>{frames}</code>'),
        _row('hw flags', f'<code>{r["hw_flags"]:02x}</code> virtio={r["virtio"]}'),
        _row('pci', f'<code>{r["pci_base"]:016x}+{r["pci_size"]:x}</code>'),
        _row('ram', f'<code>{r["ram_base"]:016x}+{r["ram_size"]:x}</code>'),
        _row('cap denials', str(r['cap_denials'])),
        _row('bundle', f'{"lzma" if r["compressed"] else "raw"} '
                       f'{r["compressed_len"]} B on the wire, {r["raw_len"]} B expanded'
                       f'{f" ({ratio:.1f}x)" if ratio else ""}'),
    ]
    return '<table>' + ''.join(rows) + '</table>'


PAGE = """<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ame panic decoder</title>
<style>
  :root { color-scheme: dark; }
  body { background:#060e14; color:#c8d6e8; font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
         margin:0; padding:28px; }
  h1 { color:#ff6b6b; font-size:18px; letter-spacing:.08em; margin:0 0 4px; }
  p.sub { color:#8fa6c4; margin:0 0 20px; }
  textarea { width:100%%; box-sizing:border-box; height:88px; background:#0b1620; color:#eaf1ff;
             border:1px solid #1d3247; border-radius:6px; padding:10px; font:inherit; resize:vertical; }
  button { margin-top:10px; background:#14304a; color:#eaf1ff; border:1px solid #2b4d6d;
           border-radius:6px; padding:8px 16px; font:inherit; cursor:pointer; }
  button:hover { background:#1b3e5e; }
  table { border-collapse:collapse; margin-top:22px; width:100%%; }
  th { text-align:left; font-weight:400; color:#8fa6c4; padding:3px 14px 3px 0; white-space:nowrap;
       vertical-align:top; }
  td { padding:3px 0; word-break:break-all; }
  tr:nth-child(odd) td, tr:nth-child(odd) th { background:#0a141d; }
  code { color:#ffd9a0; }
  .reason { color:#ff6b6b; }
  .dim { color:#5b6b7d; }
  .err { color:#ff6b6b; margin-top:20px; white-space:pre-wrap; }
</style>
<h1>AME PANIC DECODER</h1>
<p class="sub">Paste what the QR reader shows &mdash; the whole URL or just the fragment.</p>
<textarea id="q" placeholder="https://ame.example/p#QVBAAQH0AQAA..."></textarea>
<button id="go">decode</button>
<div id="out"></div>
<script>
const q = document.getElementById('q'), out = document.getElementById('out');
async function decode() {
  out.innerHTML = '<p class="dim">decoding...</p>';
  try {
    const r = await fetch('/decode', {method:'POST', body:q.value});
    const j = await r.json();
    out.innerHTML = j.ok ? j.html : '<div class="err">' + j.error + '</div>';
  } catch (e) { out.innerHTML = '<div class="err">' + e + '</div>'; }
}
document.getElementById('go').onclick = decode;
q.addEventListener('keydown', e => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) decode(); });
// The QR puts the bundle in the fragment, which never reaches the server:
// https://ame.example/p#<bundle>. Read it here.
const fromHash = location.hash.slice(1);
const pre = fromHash || new URLSearchParams(location.search).get('u');
if (pre) { q.value = pre; decode(); }
</script>
"""


class Handler(BaseHTTPRequestHandler):
    server_version = 'ame-panic-decoder'
    sys_version = ''

    def _send(self, code: int, body: str, ctype: str) -> None:
        raw = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(raw)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(raw)

    def _fail(self, code: int, message: str, as_json: bool) -> None:
        if as_json:
            self._send(code, json.dumps({'ok': False, 'error': message}), 'application/json')
        else:
            self._send(code, f'<!doctype html><body style="background:#060e14;color:#ff6b6b;'
                             f'font:14px ui-monospace,monospace;padding:28px">'
                             f'{html.escape(message)}</body>', 'text/html; charset=utf-8')

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        # '/p' is the path the firmware's QR uses (kQrUrlPrefix); the bundle lives
        # in the fragment, which the browser never sends, so those requests must
        # get the page and let its script read location.hash.
        if parsed.path in ('/', '/index.html', '/p', '/p/'):
            self._send(200, PAGE, 'text/html; charset=utf-8')
            return

        text = ''
        if parsed.path == '/d':
            text = (parse_qs(parsed.query).get('u') or [''])[0]
            if not text:
                self._send(200, PAGE, 'text/html; charset=utf-8')
                return
        elif parsed.path.startswith('/d/'):
            text = unquote(parsed.path[3:])
        if not text:
            self._fail(400, 'usage: /d?u=<url>  or  /d/<fragment>  or  open /', False)
            return
        try:
            report = decode(text)
        except Exception as exc:  # noqa: BLE001 - any malformed paste is a 400
            self._fail(400, f'decode failed: {exc}', False)
            return
        self._send(200, '<!doctype html><meta charset="utf-8">'
                        '<body style="background:#060e14;color:#c8d6e8;font:14px ui-monospace,monospace;'
                        'padding:28px">' + render_report(report) + '</body>',
                   'text/html; charset=utf-8')

    def do_POST(self) -> None:
        if urlparse(self.path).path != '/decode':
            self._fail(404, 'not found', True)
            return
        length = min(int(self.headers.get('Content-Length') or 0), 1 << 20)
        text = self.rfile.read(length).decode('utf-8', 'replace')
        try:
            report = decode(text)
        except Exception as exc:  # noqa: BLE001
            self._fail(400, f'decode failed: {exc}', True)
            return
        self._send(200, json.dumps({'ok': True, 'report': report,
                                    'html': render_report(report)}), 'application/json')

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write('%s - %s\n' % (self.address_string(), fmt % args))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='127.0.0.1', help='bind address (default: loopback only)')
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--tls', action='store_true',
                    help='serve HTTPS; needs --cert and --key')
    ap.add_argument('--cert', default=str(ROOT / 'out/panic-decoder/cert.pem'))
    ap.add_argument('--key', default=str(ROOT / 'out/panic-decoder/key.pem'))
    args = ap.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    scheme = 'http'
    if args.tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        scheme = 'https'
    print(f'panic decoder listening on {scheme}://{args.host}:{args.port}/', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
