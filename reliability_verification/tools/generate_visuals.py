"""Generate deterministic SVG evidence and Markdown tables from measured JSON."""
import html
import json
import pathlib
import textwrap
import xml.etree.ElementTree as ET

ROOT=pathlib.Path(__file__).resolve().parents[2]
BASE=ROOT/'reliability_verification';IMAGES=BASE/'images';IMAGES.mkdir(exist_ok=True)
NAMES=['case-01-late-ack','case-02-partial-frame','case-03-partial-ack-deadline','case-04-udp-stale-ack']
DATA={i:{phase:json.loads((BASE/name/'results'/(phase+'-summary.json')).read_text()) for phase in ('before','after')} for i,name in enumerate(NAMES,1)}
REG=json.loads((BASE/'regression-summary.json').read_text());GUARD=json.loads((BASE/'guard-summary.json').read_text());CAND=json.loads((BASE/'candidate05-summary.json').read_text())
INK='#14243a';MUTED='#52657a';BLUE='#1769aa';RED='#b9403d';GREEN='#18745a';BG='#f4f7fb'
def esc(s):return html.escape(str(s))
class SVG:
 def __init__(self,title,width=1200,height=700):
  self.w=width;self.h=height;self.items=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img"><title>{esc(title)}</title>',f'<rect width="{width}" height="{height}" fill="{BG}"/>']
 def text(self,x,y,text,size=22,color=INK,weight=400,anchor='start',family='Arial, sans-serif'):
  self.items.append(f'<text x="{x}" y="{y}" fill="{color}" font-family="{family}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{esc(text)}</text>')
 def rect(self,x,y,w,h,color='white',stroke='#d8e2ed'):
  self.items.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{color}" stroke="{stroke}"/>')
 def line(self,x,y,x2,y2,color=MUTED,dashed=False):
  self.items.append(f'<line x1="{x}" y1="{y}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="2"'+(' stroke-dasharray="6 5"' if dashed else '')+'/>')
 def arrow(self,x,y,x2,y2,color=BLUE):
  self.line(x,y,x2,y2,color);direction=1 if x2>=x else -1
  self.items.append(f'<path d="M {x2-direction*9} {y2-5} L {x2} {y2} L {x2-direction*9} {y2+5}" fill="none" stroke="{color}" stroke-width="2"/>')
 def wrap(self,x,y,text,width=85,size=20,color=MUTED):
  for line in textwrap.wrap(text,width):self.text(x,y,line,size,color);y+=size+8
  return y
 def save(self,name):
  self.items.append('</svg>');content='\n'.join(self.items)+'\n';ET.fromstring(content);(IMAGES/name).write_text(content)

def find(run,fragment,actor=None,n=0):
 values=[e for e in run['events'] if fragment in e['message'] and (actor is None or e['actor']==actor)]
 return values[n] if len(values)>n else None

def sequence(case,phase):
 run=DATA[case][phase]['runs'][0];steps=[]
 def add(fragment,actor,src,dst,label=None,n=0):
  e=find(run,fragment,actor,n)
  if e:steps.append((e['ms'],src,dst,label or e['message']))
 if case==1:
  sender='tcp_sender.stdout';receiver='tcp_receiver.stdout';proxy='tcp_proxy_1200.stdout'
  add('Sent DATA seq=1 attempt=1',sender,0,1,'DATA seq=1 (attempt 1)')
  add('Received DATA seq=1',receiver,2,2,'DATA seq=1 processed')
  add('[DELAY',proxy,1,1)
  add('ACK timeout seq=1',sender,0,0)
  add('Sent DATA seq=1 attempt=2',sender,0,1,'DATA seq=1 retry')
  add('[Receiver -> Sender] ACK seq=1',proxy,1,0,'First delayed ACK seq=1',0)
  add('Received ACK seq=1',sender,0,0,'DATA completed')
  add('Duplicate DATA',receiver,2,2,'Duplicate DATA: processing suppressed')
  add('[Receiver -> Sender] ACK seq=1',proxy,1,0,'Second ACK seq=1',1)
  add('Sent HEARTBEAT seq=2',sender,0,1,'HEARTBEAT seq=2')
  add('Ignored stale response',sender,0,0)
  add('Sender error','tcp_sender.stderr',0,0)
  add('[Receiver -> Sender] HEARTBEAT_ACK seq=2',proxy,1,0,'Current HEARTBEAT_ACK seq=2')
  add('Received HEARTBEAT_ACK seq=2',sender,0,0,'Current response accepted')
  footer=f"Sender exit {run['sender_exit']}; heartbeat ACKs: {run['heartbeat_acks']}; retries: {run['retry_count']}"
 else:
  relay='relay';sender='udp_sender.stdout'
  add('DATA seq=1;',relay,0,2,'DATA seq=1',0)
  add('first ACK seq=1 held',relay,2,1,'First ACK seq=1 held')
  add('ACK timeout seq=1',sender,0,0)
  add('DATA seq=1;',relay,0,2,'DATA seq=1 retry',1)
  add('ACK seq=1 forwarded unchanged',relay,1,0,'Second ACK seq=1 forwarded')
  add('Received ACK seq=1',sender,0,0,'DATA seq=1 completed')
  add('DATA seq=3;',relay,0,2,'DATA seq=3')
  add('held ACK seq=1 released',relay,1,0,'Old ACK seq=1 released')
  add('valid ACK seq=3 received',relay,2,1,'Current ACK seq=3 generated (CRC valid)')
  add('Ignored stale ACK',sender,0,0)
  add('UDP Sender error','udp_sender.stderr',0,0)
  add('actual ACK seq=3 forwarded',relay,1,0,'Current ACK seq=3 forwarded (+150ms hold)')
  add('Received ACK seq=3',sender,0,0,'Current response accepted')
  add('Received ACK seq=2',sender,0,0,'Final DATA seq=2 completed')
  footer=f"Sender exit {run['sender_exit']}; accepted ACKs: {run['ack_sequences']}"
 steps.sort(key=lambda x:x[0]);height=205+len(steps)*55
 title=f"CASE {case:02d} | {phase.upper()}";s=SVG(title,width=1300,height=height)
 s.text(40,45,title,30,weight=700);s.text(40,76,'Measured run 1 | event observation order; not packet-capture micro-order',18,MUTED)
 xs=[275,675,1075]
 for x,name in zip(xs,['Sender','Fault Injector' if case==1 else 'Test relay','Receiver']):
  s.rect(x-110,95,220,42);s.text(x,123,name,22,weight=700,anchor='middle');s.line(x,142,x,height-70,'#c7d2e0',True)
 for i,(ms,src,dst,label) in enumerate(steps):
  y=175+i*55;s.text(20,y,f'{ms:.0f}ms',15,MUTED)
  if src!=dst:
   s.arrow(xs[src],y+6,xs[dst],y+6);s.text((xs[src]+xs[dst])/2,y-4,label,17,anchor='middle')
  else:
   center=xs[src];s.rect(center-175,y-24,350,34,'#e7eef7');s.text(center,y-2,label,16,RED if 'error' in label else INK,anchor='middle')
 s.text(40,height-26,footer,20,RED if phase=='before' else GREEN,700);s.save(f'case{case:02d}-{phase}.svg')

for c in (1,4):
 for phase in ('before','after'):sequence(c,phase)

for phase in ('before','after'):
 data=DATA[2][phase];s=SVG('CASE 02 | '+phase.upper(),height=720)
 s.text(40,48,'CASE 02 | '+phase.upper(),30,weight=700)
 chain=['Partial frame + EOF','Connection-local EOF','Close client socket; accept again','Next DATA / ACK succeeds'] if phase=='after' else ['Partial frame + EOF','runtime_error escapes connection handler','main catch: Receiver exit 1','Next connection refused']
 for i,label in enumerate(chain):
  y=92+i*78;s.rect(70,y,1060,58);s.text(600,y+36,label,24,anchor='middle')
  if i<3:s.line(600,y+59,600,y+77,BLUE)
 s.text(70,440,'Boundary',22,weight=700);s.text(570,440,'Survived',22,weight=700);s.text(835,440,'Follow-up ACK',22,weight=700)
 for i,boundary in enumerate(['header-zero','header-partial','payload-zero','payload-partial']):
  rows=[r for r in data['runs'] if r['boundary']==boundary];y=483+i*48
  s.text(70,y,boundary,22);s.text(570,y,f"{sum(r['receiver_survived'] for r in rows)}/{len(rows)}",22);s.text(835,y,f"{sum(r['followup_ack'] for r in rows)}/{len(rows)}",22)
 s.text(70,695,'Incomplete frames were not processed or acknowledged; same-sequence follow-up tested.',18,MUTED);s.save(f'case02-{phase}.svg')

s=SVG('CASE 03 | Full response deadline',height=660)
s.text(40,48,'CASE 03 | Full response deadline',30,weight=700)
s.text(40,81,'Before: ACK accepted late. After: explicit failure + connection discarded.',21,MUTED)
limit=DATA[3]['after']['configured_timeout_ms'];left=245;scale=780/3600
for tick in range(0,3501,500):
 x=left+tick*scale;s.line(x,135,x,430,'#d8e2ed');s.text(x,456,str(tick),17,MUTED,anchor='middle')
for phase,base in [('before',150),('after',300)]:
 rows=[r for r in DATA[3][phase]['runs'] if r['variant']=='complete-late']
 for i,r in enumerate(rows):
  value=r['response_outcome_ms'];y=base+i*44;s.text(40,y+23,f'{phase.title()} / Run {r["run"]}',22);s.rect(left,y,value*scale,30,RED if phase=='before' else GREEN,'none');s.text(left+value*scale+10,y+23,f'{value:.3f}',19)
x=left+limit*scale;s.line(x,120,x,430,BLUE,True);s.text(x,113,f'{limit}ms configured deadline',19,BLUE,700,anchor='middle');s.text(700,487,'Observed response outcome (ms)',20,MUTED,anchor='middle')
b=[r for r in DATA[3]['before']['runs'] if r['variant']=='never-complete'];a=[r for r in DATA[3]['after']['runs'] if r['variant']=='never-complete']
s.rect(40,515,1120,112);s.text(65,548,f"No-completion case: {sum(r['alive_at_observation_end'] for r in b)}/{len(b)} waiting at {int(b[0]['observation_limit_ms'])}ms before",22,RED)
s.text(65,584,'After outcomes (ms): '+', '.join(f"{r['response_outcome_ms']:.3f}" for r in a),22,GREEN)
s.text(65,611,'Bounded fail-fast policy; this chart does not claim successful automatic DATA recovery.',17,MUTED);s.save('case03-timeout-before-after.svg')

s=SVG('AI-assisted reliability verification',height=810)
s.text(45,55,'Reliability verification beyond isolated faults',34,weight=700)
s.text(45,94,'AI-assisted hypotheses. Conclusions established by controlled experiments.',23,MUTED)
steps=[('01','Human-designed Phase 1-4','drop / delay / CRC / reconnect'),('02','AI-assisted analysis','source + protocol + coverage'),('03',f"{len(DATA)+1} hypotheses investigated",f"{len(DATA)} defect cases + 1 contract unclear"),('04','Production fixes','deadline / stale responses / EOF'),('05','Same experiments rerun',f"Before / After: {DATA[1]['after']['repetitions']} runs per condition"),('06','Regression verification',f"{REG['passed']}/{REG['total']} scenarios + {GUARD['passed']}/{GUARD['total']} policy checks")]
for i,(num,title,detail) in enumerate(steps):
 x=45+(i%3)*385;y=140+(i//3)*165;s.rect(x,y,355,135);s.text(x+20,y+33,num,22,BLUE,700);s.text(x+20,y+69,title,22,weight=700);s.wrap(x+20,y+99,detail,30,18)
labels=['Late ACK / retry -> stale response','Partial frame -> Receiver exit','Partial ACK -> deadline violation','UDP stale ACK -> later request exit']
for i,label in enumerate(labels):
 y=520+i*53;s.text(55,y,f'CASE {i+1:02d}',20,BLUE,700);s.text(185,y,label,22);s.text(1130,y,'After: '+DATA[i+1]['after']['verdict'],22,GREEN,700,anchor='end')
s.text(55,757,'CASE 05: ACK loss + one-shot UDP Receiver; reproduced, not counted as a defect.',21,MUTED)
s.text(55,788,'Evidence: executable tests -> measured JSON -> generated diagrams',20,MUTED);s.save('portfolio-overview.svg')
s=SVG('Verification evidence flow',height=850);s.text(40,48,'From hypothesis to reviewable evidence',30,weight=700)
flow=['Existing human-designed Phase 1-4 fault checks','AI-assisted source / protocol / coverage analysis','Controlled reproduction + observable assertions','Classify: four defects; one contract-ambiguous candidate','Root cause -> minimal production fixes','Same inputs -> Before / After comparison',f"Regression: {REG['passed']}/{REG['total']}; policy checks: {GUARD['passed']}/{GUARD['total']}",'Structured JSON -> generated SVG -> documented results']
for i,label in enumerate(flow):
 y=90+i*90;s.rect(60,y,1080,62);s.text(600,y+39,label,24,anchor='middle')
 if i<7:s.line(600,y+63,600,y+89,BLUE)
s.save('verification-flow.svg')

# Curated panels use exact recorded log strings, not reconstructed terminal text.
for c in (1,2,4):
 r=DATA[c]['before']['runs'][1 if c==2 else 0]
 logs=[e for e in r['events'] if (e['actor'].endswith('stderr') or any(token in e['message'] for token in ('ACK timeout','attempt=2','Received ACK','Sent HEARTBEAT seq=2','ConnectionRefusedError')))]
 s=SVG(f'CASE {c:02d} | Recorded evidence',height=160+len(logs)*42);s.text(40,48,f'CASE {c:02d} | Recorded Before evidence',28,weight=700)
 for i,e in enumerate(logs):s.text(40,104+i*42,e['message'],19,INK,family='monospace')
 s.text(40,s.h-25,'Exact selected log lines from before-summary.json, run '+str(r['run']),18,MUTED);s.save(f'case{c:02d}-evidence.svg')

before_a=[r['response_outcome_ms'] for r in DATA[3]['before']['runs'] if r['variant']=='complete-late']
after_a=[r['response_outcome_ms'] for r in DATA[3]['after']['runs'] if r['variant']=='complete-late']
matrix='''| Case | Before | Root cause / fix | After |
|---|---|---|---|
'''
def count(case,phase,predicate):
 runs=DATA[case][phase]['runs']
 return f"{sum(bool(predicate(r)) for r in runs)}/{len(runs)}"
rows=[
 ('01 Late ACK', 'Heartbeat failure '+count(1,'before',lambda r:r['sender_exit']==1), 'Completed-response matching; one absolute deadline', 'Exit 0 + Heartbeat 2/3/4 '+count(1,'after',lambda r:r['sender_exit']==0 and r['heartbeat_acks']==[2,3,4])),
 ('02 Partial frame', 'Partial EOF receiver exits: '+str(sum(r['receiver_exit']==1 for r in DATA[2]['before']['runs']))+'; zero-byte EOF controls survive', 'EOF stays connection-local; discard incomplete frame', 'Follow-up DATA/ACK '+count(2,'after',lambda r:r['followup_ack'] and r['followup_processed'])),
 ('03 Partial ACK', ', '.join(f'{x:.3f}' for x in before_a)+' ms ACK success', 'Deadline-aware receive; partial connection discarded', ', '.join(f'{x:.3f}' for x in after_a)+' ms explicit failure'),
 ('04 UDP stale ACK', 'ACK mismatch / exit 1 '+count(4,'before',lambda r:r['sender_exit']==1), 'Completed-sequence history; fixed deadline', 'ACK 1/3/2 + exit 0 '+count(4,'after',lambda r:r['sender_exit']==0 and r['ack_sequences']==[1,3,2]))]
assert all(DATA[c]['before']['verdict']=='REPRODUCED' and DATA[c]['after']['verdict']=='PASS' for c in DATA)
for row in rows:matrix+='| '+' | '.join(row)+' |\n'
(BASE/'evidence-matrix.md').write_text(matrix)
for path in [ROOT/'README.md',BASE/'README.md']:
 if path.exists():
  text=path.read_text();start='<!-- evidence-matrix:start -->';end='<!-- evidence-matrix:end -->'
  if start in text:
   a=text.index(start)+len(start);b=text.index(end,a);path.write_text(text[:a]+'\n'+matrix+text[b:])
manifest={'case01-before.svg':['case-01-late-ack/results/before-summary.json'],'case01-after.svg':['case-01-late-ack/results/after-summary.json'],'case02-before.svg':['case-02-partial-frame/results/before-summary.json'],'case02-after.svg':['case-02-partial-frame/results/after-summary.json'],'case03-timeout-before-after.svg':['case-03-partial-ack-deadline/results/before-summary.json','case-03-partial-ack-deadline/results/after-summary.json'],'case04-before.svg':['case-04-udp-stale-ack/results/before-summary.json'],'case04-after.svg':['case-04-udp-stale-ack/results/after-summary.json']}
all_sources=[f'{name}/results/{phase}-summary.json' for name in NAMES for phase in ('before','after')]+['regression-summary.json','guard-summary.json','candidate05-summary.json']
for name in ['portfolio-overview.svg','verification-flow.svg']:manifest[name]=all_sources
for c in (1,2,4):manifest[f'case{c:02d}-evidence.svg']=[f'{NAMES[c-1]}/results/before-summary.json']
(IMAGES/'sources.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'Generated {len(manifest)} SVGs from recorded results.')
