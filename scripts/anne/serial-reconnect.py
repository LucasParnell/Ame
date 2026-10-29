#!/usr/bin/env python3
"""Exercise Ame CDC port reopen while a log transfer is still outstanding.

No flashing or rebooting. Uses the serial port exclusively while it runs.
Each challenge requires shell output on its own line, not the echoed command.
"""
import argparse,importlib.util,json,re,time,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--expected-run-tag',required=True)
p.add_argument('--iterations',type=int,default=8)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--usb-state-on-failure',action='store_true')
a=p.parse_args()
if not 1<=a.iterations<=100:raise ValueError('iterations must be 1..100')
spec=importlib.util.spec_from_file_location('panvk_gate',ROOT/'scripts/anne/panvk-gate.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
a.output.mkdir(parents=True,exist_ok=True)
report={'passed':False,'requested_iterations':a.iterations,'completed_iterations':0}
link=None
try:
    with (a.output/'console.log').open('wb') as log:
        link=gate.open_console(15)
        shell=gate.Session(link,log)
        identity=shell.run('diag log',6)
        if 'RUN tag='+a.expected_run_tag not in identity:raise RuntimeError('run tag not verified')
        link.close();link=None
        for i in range(a.iterations):
            report['phase']='interrupt_log'
            link=gate.open_console(15)
            link.write(b'\x15diag log\r',time.monotonic()+3)
            end=time.monotonic()+3;received=bytearray()
            while len(received)<1024 and time.monotonic()<end:
                try:received.extend(link.read(min(end,time.monotonic()+1)))
                except TimeoutError:continue
            log.write(received);log.flush()
            if not received:raise RuntimeError('log did not begin before close')
            link.close();link=None
            report['phase']='reopen_challenge'
            link=gate.open_console(15)
            challenge=f'USB-REOPEN-{i+1}'
            marker=re.compile(rb'(?:\r?\n)'+challenge.encode()+rb'\r?\n')
            answer=gate.Session(link,log).run('\x15echo '+challenge,10,marker)
            if not marker.search(answer.encode()):raise RuntimeError('shell challenge did not complete after reopen')
            link.close();link=None
            report['completed_iterations']=i+1
            print(f'CDC reopen {i+1}/{a.iterations}: shell responded',flush=True)
        report['passed']=True;report['phase']='complete'
except Exception as error:
    report['error']=str(error)
    if a.usb_state_on_failure:
        # Observe the failed state before closing/restoring termios, which
        # would issue another CDC request and overwrite the useful evidence.
        captured=subprocess.run([sys.executable,str(ROOT/'scripts/anne/usb-state.py'),
            '--output',str(a.output/'failure-usb-state.json')],capture_output=True,
            text=True,timeout=20)
        (a.output/'failure-usb-state.log').write_text(captured.stdout+captured.stderr)
        report['usb_state_captured']=captured.returncode==0
finally:
    if link is not None:link.close()
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
raise SystemExit(0 if report['passed'] else 1)
