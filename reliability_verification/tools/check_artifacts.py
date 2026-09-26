"""Check source/evidence identity, local links, SVGs and deterministic generation."""
import ast
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
ROOT=pathlib.Path(__file__).resolve().parents[2];BASE=ROOT/'reliability_verification'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for path in BASE.rglob('*.py'):ast.parse(path.read_text(),filename=str(path))
checks=[]
for directory in sorted(BASE.glob('case-*')):
    before=json.loads((directory/'results/before-summary.json').read_text());after=json.loads((directory/'results/after-summary.json').read_text())
    assert before['harness_sha256']==after['harness_sha256']==digest(BASE/'common/runner.py')
    assert before['repetitions']==after['repetitions']==3
    assert before['verdict']=='REPRODUCED' and after['verdict']=='PASS'
    assert [(r['run'],r['variant']) for r in before['runs']]==[(r['run'],r['variant']) for r in after['runs']]
    for name,sha in before['source_sha256'].items():
        blob=subprocess.check_output(['git','show','v1.0.0:'+name],cwd=ROOT)
        assert hashlib.sha256(blob).hexdigest()==sha,name
    for name,sha in after['source_sha256'].items():assert digest(ROOT/name)==sha,name
    checks.append(directory.name)
for filename in ('regression-summary.json','guard-summary.json'):
    data=json.loads((BASE/filename).read_text());assert data['passed']==data['total'] and all(r['passed'] for r in data['runs'])
    if filename=='regression-summary.json':
        assert data['harness_sha256']==digest(BASE/'common/runner.py')
        for name,sha in data['source_sha256'].items():assert digest(ROOT/name)==sha,name
candidate=json.loads((BASE/'candidate05-summary.json').read_text());assert candidate['classification']=='Contract unclear' and candidate['included_in_defect_count'] is False
matrix=(BASE/'evidence-matrix.md').read_text().strip()
for doc in [ROOT/'README.md',BASE/'README.md']:assert matrix in doc.read_text()
checked_links=0
for doc in [ROOT/'README.md',*BASE.rglob('*.md')]:
    for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)',doc.read_text()):
        target=target.strip('<>').split('#')[0]
        if not target or re.match(r'[a-zA-Z]+:',target):continue
        assert (doc.parent/target).exists(),(str(doc),target)
        checked_links+=1
sources=json.loads((BASE/'images/sources.json').read_text())
for name,inputs in sources.items():
    path=BASE/'images'/name;tree=ET.fromstring(path.read_text());assert tree.tag.endswith('svg') and 100<path.stat().st_size<100000
    assert tree.find('{http://www.w3.org/2000/svg}title') is not None
    for source in inputs:assert (BASE/source).is_file()
chart=(BASE/'images/case03-timeout-before-after.svg').read_text()
case03_doc=(BASE/'case-03-partial-ack-deadline/README.md').read_text()
for phase in ('before','after'):
    data=json.loads((BASE/'case-03-partial-ack-deadline/results'/f'{phase}-summary.json').read_text())
    for r in data['runs']:
        if r['response_outcome_ms'] is not None:
            value=f"{r['response_outcome_ms']:.3f}"
            assert value in chart and value in case03_doc
            if r['variant']=='complete-late':assert value in matrix
paths=[*sorted((BASE/'images').glob('*')),ROOT/'README.md',BASE/'README.md',BASE/'evidence-matrix.md']
original={str(p):digest(p) for p in paths}
subprocess.run([sys.executable,str(BASE/'tools/generate_visuals.py')],cwd=ROOT,check=True)
assert original=={str(p):digest(p) for p in paths},'non-deterministic regeneration'
assert b'kAckDelayMs = 700;' in (ROOT/'fault_injector/tcp_proxy.cpp').read_bytes()
print(json.dumps({'case_evidence':checks,'source_hashes':'PASS','regression':'PASS','guards':'PASS','relative_links_checked':checked_links,'svg_files':len(sources),'regeneration_identical':True},indent=2))
