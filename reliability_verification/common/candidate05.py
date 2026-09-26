"""Observed lifecycle interaction, excluded from the four defect/fix counts."""
import pathlib
import tempfile
from runner import ROOT, SUITE, Run, build_sources, udp_relay, save

def main():
    work=pathlib.Path(tempfile.mkdtemp(prefix='reliability-contract-'))
    print('Raw evidence:',work,flush=True)
    build=build_sources(ROOT,work);runs=[]
    for n in range(1,4):
        result=udp_relay(Run(work/str(n),build),'path-drop')
        result['run']=n
        result['observed']=result['receiver_natural_exit']==0 and result['sender_exit']==1 and result['data_sequences']==[1,1,1,1] and 'ACK retry limit exceeded' in result['error']
        runs.append(result);print(n,result['observed'],flush=True)
    save(SUITE/'candidate05-summary.json',{'classification':'Contract unclear','included_in_defect_count':False,'runs':runs,'observed':sum(r['observed'] for r in runs),'total':len(runs)})
    raise SystemExit(0 if all(r['observed'] for r in runs) else 1)
if __name__=='__main__':main()
