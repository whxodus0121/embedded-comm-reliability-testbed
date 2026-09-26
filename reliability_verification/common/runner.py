"""Linux-only controlled reliability experiments; Python standard library only."""
import argparse
import hashlib
import json
import pathlib
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SUITE = ROOT / 'reliability_verification'
CASES = {1:'case-01-late-ack',2:'case-02-partial-frame',3:'case-03-partial-ack-deadline',4:'case-04-udp-stale-ack'}
TIMEOUT_MS = 1000
# 250ms scheduler allowance, far below the 3200ms completion stimulus.
MIN_MS, MAX_MS = 900, 1250

def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')

def packet(kind, seq, payload=b''):
    prefix=struct.pack('!HBBII',0xaa55,1,kind,seq,len(payload))
    return prefix+struct.pack('!I',zlib.crc32(prefix+payload)&0xffffffff)+payload

def decode(data):
    magic,version,kind,seq,size,crc=struct.unpack('!HBBIII',data[:16])
    assert (magic,version)==(0xaa55,1) and len(data)==16+size
    assert crc==zlib.crc32(data[:12]+data[16:])&0xffffffff
    return kind,seq

def exact(sock, size):
    result=b''
    while len(result)<size:
        part=sock.recv(size-len(result))
        if not part: raise EOFError('peer EOF')
        result+=part
    return result

def frame(sock):
    head=exact(sock,16)
    return decode(head+exact(sock,struct.unpack('!I',head[8:12])[0]))

class Run:
    def __init__(self, directory, build):
        self.directory=directory; directory.mkdir(parents=True)
        self.build=build; self.start=time.monotonic(); self.events=[]; self.processes=[]; self.threads=[]; self.lock=threading.Lock()
    def event(self, actor, message):
        with self.lock:
            self.events.append({'ms':round((time.monotonic()-self.start)*1000,3),'actor':actor,'message':message})
    def launch(self, name, *args):
        command=['stdbuf','-oL','-eL',str(self.build/name),*args]
        self.event('command',' '.join([name,*args]))
        p=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.processes.append((name,p))
        def drain(stream,label):
            with (self.directory/(name+'.'+label+'.log')).open('w') as log:
                for line in stream:
                    log.write(line); log.flush(); self.event(name+'.'+label,line.rstrip())
            stream.close()
        for label in ('stdout','stderr'):
            thread=threading.Thread(target=drain,args=(getattr(p,label),label));thread.start();self.threads.append(thread)
        return p
    def text(self, name, stream='stdout'):
        path=self.directory/(name+'.'+stream+'.log')
        return path.read_text() if path.exists() else ''
    def ready(self, name, marker):
        until=time.monotonic()+3
        while time.monotonic()<until:
            if marker in self.text(name):return
            time.sleep(.01)
        raise RuntimeError('readiness timeout: '+name)
    def finish(self):
        exits=[]
        for name,p in self.processes:
            external=p.poll() is None
            if external:
                p.terminate()
                try:p.wait(timeout=2)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=2)
            exits.append({'process':name,'exit_code':p.returncode,'external_cleanup':external})
        for thread in self.threads:thread.join(timeout=3)
        save(self.directory/'events.json',self.events)
        save(self.directory/'exits.json',exits)
        self.exits=exits
    def result(self, values):
        values.update(events=sorted(self.events,key=lambda e:e['ms']),exits=self.exits)
        return values

def build_sources(source, work):
    build=work/'build'
    commands=[['cmake','-S',str(source),'-B',str(build)],['cmake','--build',str(build),'-j2']]
    copied=work/'test-source';(copied/'udp').mkdir(parents=True);(copied/'fault_injector').mkdir()
    shutil.copytree(source/'protocol',copied/'protocol')
    variants=[('udp/receiver.cpp',b'kPort = 6000;',b'kPort = 6001;','udp_receiver_port6001'),('fault_injector/tcp_proxy.cpp',b'kAckDelayMs = 700;',b'kAckDelayMs = 1200;','tcp_proxy_1200')]
    for rel,old,new,name in variants:
        data=(source/rel).read_bytes();assert data.count(old)==1
        (copied/rel).write_bytes(data.replace(old,new))
        commands.append(['g++','-std=c++17',str(copied/rel),str(copied/'protocol/codec.cpp'),str(copied/'protocol/crc32.cpp'),'-o',str(build/name)])
    for i,cmd in enumerate(commands):
        result=subprocess.run(cmd,capture_output=True,text=True)
        (work/f'build-{i}.stdout.log').write_text(result.stdout);(work/f'build-{i}.stderr.log').write_text(result.stderr)
        if result.returncode:raise RuntimeError(result.stderr)
    save(work/'build-commands.json',commands)
    return build

def tcp_proxy_run(r,mode='delay-ack',late=True):
    proxy='tcp_proxy_1200' if late else 'tcp_fault_injector'
    try:
        receiver=r.launch('tcp_receiver');r.ready('tcp_receiver','listening')
        r.launch(proxy,mode);r.ready(proxy,'Fault mode:')
        sender=r.launch('tcp_sender');exitcode=sender.wait(timeout=15);time.sleep(.1)
    finally:r.finish()
    s=r.text('tcp_sender');recv=r.text('tcp_receiver');p=r.text(proxy)
    return r.result({'sender_exit':exitcode,'timeout_count':s.count('ACK timeout seq=1'),'retry_count':s.count('Sent DATA seq=1')-1,'duplicate_count':recv.count('Duplicate DATA seq=1'),'receiver_ack_count':recv.count('Sent ACK seq=1'),'forwarded_ack_count':p.count('[Receiver -> Sender] ACK seq=1'),'heartbeat_acks':[i for i in (2,3,4) if f'Received HEARTBEAT_ACK seq={i}' in s],'error':r.text('tcp_sender','stderr').strip(),'receiver_error':r.text('tcp_receiver','stderr').strip(),'receiver_survived':receiver.returncode==-15,'reconnected':'Reconnecting...' in s})

def partial_frame(r,kind):
    sizes={'header-zero':0,'header-partial':8,'payload-zero':16,'payload-partial':19}
    before_bytes=b'';follow=False;reason='';reply=b''
    try:
        receiver=r.launch('tcp_receiver');r.ready('tcp_receiver','listening')
        wire=packet(1,1,b'0123456789')[:sizes[kind]]
        with socket.create_connection(('127.0.0.1',5001),timeout=2) as c:
            if wire:c.sendall(wire)
            r.event('client',f'sent {len(wire)} bytes; shutdown write')
            c.shutdown(socket.SHUT_WR)
            try:reply=c.recv(1024)
            except ConnectionResetError:pass
        until=time.monotonic()+2
        while receiver.poll() is None and 'Waiting for connection' not in r.text('tcp_receiver') and time.monotonic()<until:time.sleep(.01)
        alive=receiver.poll() is None;exitcode=receiver.poll();before_bytes=r.text('tcp_receiver')
        try:
            with socket.create_connection(('127.0.0.1',5001),timeout=2) as c:
                c.sendall(packet(1,1,b'followup'));follow=frame(c)==(2,1)
                r.event('client','follow-up same-sequence DATA -> valid ACK')
        except OSError as e:reason=type(e).__name__;r.event('client',reason)
        time.sleep(.05)
    finally:r.finish()
    return r.result({'boundary':kind,'bytes_sent':sizes[kind],'receiver_survived':alive,'receiver_exit':exitcode,'incomplete_ack_bytes':len(reply),'incomplete_processed':'Received DATA' in before_bytes,'followup_ack':follow,'followup_processed':r.text('tcp_receiver').count('Received DATA seq=1')==1,'followup_duplicate':'Duplicate DATA' in r.text('tcp_receiver'),'followup_error':reason,'error':r.text('tcp_receiver','stderr').strip()})

def partial_ack(r,mode):
    listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',5000));listener.listen();listener.settimeout(3);c=None
    try:
        sender=r.launch('tcp_sender');c,_=listener.accept();c.settimeout(10);assert frame(c)==(1,1)
        base=time.monotonic();r.event('peer','DATA seq=1 received');time.sleep(.08);c.sendall(packet(2,1)[:1]);r.event('peer','ACK first byte sent')
        limit=3.2 if mode=='complete-late' else 6.0
        # Monitor until either natural bounded failure or the original stimulus time.
        while time.monotonic()-base<limit and sender.poll() is None:time.sleep(.01)
        exit_elapsed=(time.monotonic()-base)*1000 if sender.poll() is not None else None
        peer_eof=False
        if sender.poll() is not None:
            peer_eof=c.recv(1024)==b'';r.event('peer','connection discarded EOF='+str(peer_eof))
        if mode=='complete-late':
            time.sleep(max(0,base+3.2-time.monotonic()))
            try:c.sendall(packet(2,1)[1:]);r.event('peer','remaining ACK bytes send attempted at 3200ms')
            except OSError:r.event('peer','remaining ACK rejected by closed connection')
            if sender.poll() is None:
                for seq in (2,3,4):
                    assert frame(c)==(3,seq);c.sendall(packet(4,seq))
                sender.wait(timeout=3)
        alive=sender.poll() is None;natural_exit=sender.poll()
        r.event('observer',f'observation end: alive={alive}; natural_exit={natural_exit}')
    finally:
        r.finish()
        if c:c.close()
        listener.close()
    data=[e['ms'] for e in r.events if e['message'].startswith('Sent DATA seq=1')]
    ack=[e['ms'] for e in r.events if e['message']=='Received ACK seq=1']
    errors=[e['ms'] for e in r.events if e['actor']=='tcp_sender.stderr']
    measured=(errors[0]-data[0]) if errors else ((ack[0]-data[0]) if ack else None)
    s=r.text('tcp_sender')
    return r.result({'mode':mode,'response_outcome_ms':round(measured,3) if measured is not None else None,'observation_limit_ms':limit*1000,'natural_exit':natural_exit,'alive_at_observation_end':alive,'connection_discarded':peer_eof,'data_ack':bool(ack),'timeout_count':s.count('ACK timeout'),'retry_count':s.count('Sent DATA')-1,'error':r.text('tcp_sender','stderr').strip()})

def udp_relay(r,mode='stale'):
    front=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);front.bind(('127.0.0.1',6000));back=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);back.bind(('127.0.0.1',0))
    endpoint=None;held=None;pending=None;due=0;acks=0;seqs=[];ack3=False;first_drop=False
    try:
        receiver=r.launch('udp_receiver_port6001',*(['out-of-order'] if mode in ('stale','order') else (['drop-first-ack'] if mode=='internal-drop' else [])))
        r.ready('udp_receiver_port6001','listening')
        sender=r.launch('udp_sender',*(['out-of-order'] if mode in ('stale','order') else []));end=time.monotonic()+8;finished=None
        while time.monotonic()<end:
            if sender.poll() is not None and finished is None:finished=time.monotonic()
            if pending and time.monotonic()>=due:
                front.sendto(pending,endpoint);r.event('relay','actual ACK seq=3 forwarded unchanged');pending=None
            if finished and time.monotonic()-finished>.25 and not pending:break
            for sock in select.select([front,back],[],[],.01)[0]:
                data,address=sock.recvfrom(2048);kind,seq=decode(data)
                if sock is front:
                    assert kind==1
                    if endpoint is None:endpoint=address
                    assert endpoint==address
                    seqs.append(seq);r.event('relay',f'DATA seq={seq}; receiver_exit={receiver.poll()}')
                    back.sendto(data,('127.0.0.1',6001))
                    if mode=='stale' and seq==3:
                        assert held;front.sendto(held,endpoint);r.event('relay','held ACK seq=1 released unchanged');held=None
                else:
                    assert address==('127.0.0.1',6001) and kind==2;r.event('relay',f'valid ACK seq={seq} received hex={data.hex()}')
                    if mode=='path-drop' and not first_drop:
                        first_drop=True;receiver.wait(timeout=2);r.event('relay','ACK dropped; receiver exit=0');continue
                    if mode=='stale' and seq==1:
                        acks+=1
                        if acks==1:held=data;r.event('relay','first ACK seq=1 held');continue
                    if seq==3:ack3=True
                    if mode=='stale' and seq==3:pending=data;due=time.monotonic()+.15;continue
                    front.sendto(data,endpoint);r.event('relay',f'ACK seq={seq} forwarded unchanged')
        if sender.poll() is None:raise RuntimeError('UDP sender external deadline exceeded')
        exitcode=sender.returncode;receiver_exit=receiver.poll()
    finally:r.finish();front.close();back.close()
    s=r.text('udp_sender');recv=r.text('udp_receiver_port6001')
    return r.result({'sender_exit':exitcode,'receiver_natural_exit':receiver_exit,'data_sequences':seqs,'ack_sequences':[i for i in (1,3,2) if f'Received ACK seq={i}' in s],'timeout_count':s.count('ACK timeout'),'retry_count':len(seqs)-len(set(seqs)),'duplicate_count':recv.count('Duplicate DATA'),'ack3_generated':ack3,'error':r.text('udp_sender','stderr').strip()})

def case_verdict(case,result,phase):
    before=phase=='before'
    if case==1:
        stimulus=result['timeout_count']==1 and result['retry_count']==1 and result['duplicate_count']==1 and result['receiver_ack_count']==2 and result['forwarded_ack_count']==2
        return stimulus and ((result['sender_exit']==1 and 'unexpected response type' in result['error']) if before else (result['sender_exit']==0 and result['heartbeat_acks']==[2,3,4]))
    if case==2:
        partial=result['boundary'].endswith('partial')
        if before and partial:return result['receiver_exit']==1 and not result['followup_ack'] and 'during packet receive' in result['error']
        return result['receiver_survived'] and result['followup_ack'] and result['followup_processed'] and not result['followup_duplicate'] and not result['incomplete_processed'] and result['incomplete_ack_bytes']==0
    if case==3:
        if before:
            return (result['data_ack'] and result['response_outcome_ms']>3000 and result['retry_count']==0) if result['mode']=='complete-late' else (result['alive_at_observation_end'] and not result['data_ack'] and result['timeout_count']==0 and result['retry_count']==0)
        return result['natural_exit']==1 and result['connection_discarded'] and not result['data_ack'] and result['retry_count']==0 and 'partial response deadline exceeded' in result['error'] and MIN_MS<=result['response_outcome_ms']<=MAX_MS
    return result['timeout_count']==1 and result['retry_count']==1 and result['duplicate_count']==1 and result['ack3_generated'] and ((result['sender_exit']==1 and 'ACK sequence mismatch' in result['error'] and result['ack_sequences']==[1]) if before else (result['sender_exit']==0 and result['ack_sequences']==[1,3,2] and result['receiver_natural_exit']==0))

def main(default_case=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase',choices=['before','after'],required=True)
    parser.add_argument('--case',choices=['all','1','2','3','4','regression'],default=str(default_case) if default_case else 'all')
    parser.add_argument('--repeat',type=int,default=3)
    parser.add_argument('--source',type=pathlib.Path,default=ROOT)
    parser.add_argument('--output',type=pathlib.Path,default=SUITE)
    args=parser.parse_args();assert args.repeat>=1
    work=pathlib.Path(tempfile.mkdtemp(prefix='reliability-verification-'));print('Raw evidence:',work,flush=True)
    build=build_sources(args.source.resolve(),work)
    hashes={str(p.relative_to(args.source)):hashlib.sha256(p.read_bytes()).hexdigest() for directory in ('tcp','udp','protocol','fault_injector') for p in sorted((args.source/directory).iterdir()) if p.is_file()}
    metadata={'schema_version':1,'phase':args.phase,'configured_timeout_ms':TIMEOUT_MS,'timing_tolerance_ms':[MIN_MS,MAX_MS],'source_sha256':hashes,'harness_sha256':hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),'baseline_tag':'v1.0.0','repetitions':args.repeat}
    failures=0
    if args.case=='regression':
        results=[]
        for mode in ('none','drop-ack','delay-ack','corrupt-data','disconnect'):
            x=tcp_proxy_run(Run(work/('regression-tcp-'+mode),build),mode,False);x['scenario']='tcp-'+mode
            x['passed']=(x['sender_exit']==1 and 'CRC mismatch' in x['receiver_error']) if mode=='corrupt-data' else (x['sender_exit']==0 and x['heartbeat_acks']==[2,3,4])
            if mode=='drop-ack':x['passed'] &= x['retry_count']==1 and x['duplicate_count']==1
            if mode=='delay-ack':x['passed'] &= x['retry_count']==0
            if mode=='disconnect':x['passed'] &= x['reconnected']
            results.append(x)
        for mode in ('normal','internal-drop','order'):
            x=udp_relay(Run(work/('regression-udp-'+mode),build),mode);x['scenario']='udp-'+mode;x['passed']=x['sender_exit']==0 and x['receiver_natural_exit']==0
            if mode=='internal-drop':x['passed'] &= x['retry_count']==1 and x['duplicate_count']==1
            if mode=='order':x['passed'] &= x['data_sequences']==[1,3,2] and x['ack_sequences']==[1,3,2]
            results.append(x)
        save(args.output/'regression-summary.json',dict(metadata,runs=results,passed=sum(x['passed'] for x in results),total=len(results)))
        failures=sum(not x['passed'] for x in results)
    else:
        for case in ([1,2,3,4] if args.case=='all' else [int(args.case)]):
            results=[]
            for n in range(1,args.repeat+1):
                variants=['header-zero','header-partial','payload-zero','payload-partial'] if case==2 else (['complete-late','never-complete'] if case==3 else ['main'])
                for variant in variants:
                    run=Run(work/f'case-{case}-{n}-{variant}',build)
                    x=tcp_proxy_run(run) if case==1 else (partial_frame(run,variant) if case==2 else (partial_ack(run,variant) if case==3 else udp_relay(run)))
                    x.update(run=n,variant=variant,matched_expected_behavior=case_verdict(case,x,args.phase));results.append(x)
                    print(case,n,variant,x['matched_expected_behavior'],flush=True)
            failed=sum(not x['matched_expected_behavior'] for x in results);failures+=failed
            save(args.output/CASES[case]/'results'/(args.phase+'-summary.json'),dict(metadata,case=CASES[case],runs=results,matched=len(results)-failed,total=len(results),verdict=('REPRODUCED' if args.phase=='before' else 'PASS') if not failed else 'FAIL'))
    print('Failures:',failures,flush=True)
    raise SystemExit(1 if failures else 0)

if __name__=='__main__':main()
