#!/usr/bin/env python3
"""Run an explicitly selected CTS case pattern on an already booted Ame phone.

Uses only the native CTS diagnostic; does not flash or own the serial port while
another watcher is active. QPA results, skipped cases and faults are preserved.
A grouped CTS artifact is development evidence, never a conformance result.
"""
from pathlib import Path
import argparse,importlib.util,json,re,time
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--case',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--timeout',type=int,default=210)
p.add_argument('--expected-run-tag',default='anne-panvk-cts-core-v15')
a=p.parse_args()
if not re.fullmatch(r'dEQP-VK\.[A-Za-z0-9_.*-]+',a.case):raise ValueError('invalid CTS pattern')
spec=importlib.util.spec_from_file_location('panvk_gate',ROOT/'scripts/anne/panvk-gate.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
a.output.mkdir(parents=True,exist_ok=True)
link=gate.open_console(15)
report={'pattern':a.case,'full_cts_run':False,'cases':[],'passed':False}
try:
    with (a.output/'console.log').open('wb') as log:
        shell=gate.Session(link,log)
        identity=shell.run('diag log',6)
        if not identity.strip():raise RuntimeError('Ame console did not respond to diag log; CTS was not started')
        if a.expected_run_tag and 'RUN tag='+a.expected_run_tag not in identity:raise RuntimeError('installed image does not match expected CTS run tag')
        result=shell.run('diag gpu cts '+a.case,a.timeout,re.compile(rb'GPU: CTS exit=(\d+) elapsed_ms=(\d+)'))
        match=re.search(r'GPU: CTS exit=(\d+) elapsed_ms=(\d+)',result)
        if match:
            report['exit_status']=int(match[1]);report['elapsed_ms']=int(match[2])
            qpa=shell.run('diag gpu cts-log',120,re.compile(rb'GPU: CTS (?:log end|log unavailable)'))
            (a.output/'cts.qpa.txt').write_text(qpa)
            report['log_complete']='GPU: CTS log end' in qpa
            for piece in re.split(r'#beginTestCaseResult\s+',qpa)[1:]:
                name=piece.splitlines()[0].strip()
                status=re.search(r'<Result\s+StatusCode="([^"]+)"',piece)
                report['cases'].append({'name':name,'status':status[1] if status else 'Incomplete'})
        else:report['error']='no CTS completion marker'
        trace=shell.run('diag log',7)
        report['fault_markers']=[s for s in gate.FAULTS if s in result+trace]
        statuses=[case['status'] for case in report['cases']]
        report['pass_count']=statuses.count('Pass');report['skip_count']=statuses.count('NotSupported')
        report['fail_count']=sum(s not in ('Pass','NotSupported') for s in statuses)
        report['no_failures']=bool(report.get('exit_status')==0 and statuses and not report['fail_count'] and not report['fault_markers'] and report.get('log_complete'))
        report['passed']=bool(report['no_failures'] and report['pass_count'])
        if report['no_failures'] and not report['pass_count']:report['error']='all selected CTS cases were unsupported; no case passed'
except Exception as error:
    report['error']=str(error);report['passed']=False
finally:link.close()
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='cases'},indent=2))
if report.get('fail_count'):
    print('Failures:',json.dumps([c for c in report['cases'] if c['status'] not in ('Pass','NotSupported')]))
raise SystemExit(0 if report['passed'] else 1)
