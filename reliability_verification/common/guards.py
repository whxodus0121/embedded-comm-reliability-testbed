"""Additional protocol-policy and non-renewing-deadline regression checks."""
import json
import pathlib
import select
import socket
import tempfile
import time
from runner import ROOT, SUITE, Run, build_sources, packet, frame, decode, save, MIN_MS, MAX_MS

def tcp_guard(build,work,mode):
    r=Run(work/('tcp-'+mode),build);ls=socket.socket();ls.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);ls.bind(('127.0.0.1',5000));ls.listen();ls.settimeout(4);c=None;new=None
    try:
        sender=r.launch('tcp_sender');c,_=ls.accept();c.settimeout(4);assert frame(c)==(1,1)
        if mode in ('stale-deadline','heartbeat-partial'):
            c.sendall(packet(2,1));assert frame(c)==(3,2);base=time.monotonic();r.event('peer','HEARTBEAT seq=2 received')
            if mode=='heartbeat-partial':
                c.sendall(packet(4,2)[:1]);sender.wait(timeout=2);elapsed=(time.monotonic()-base)*1000
                eof=c.recv(1024)==b'';passed=sender.returncode==1 and eof and MIN_MS<=elapsed<=MAX_MS
            else:
                until=time.monotonic()+2;closed=False
                while time.monotonic()<until:
                    if select.select([ls],[],[],0)[0]:break
                    try:c.sendall(packet(2,1));r.event('peer','stale ACK seq=1 sent')
                    except OSError:closed=True
                    time.sleep(.06)
                new,_=ls.accept();new.settimeout(4);elapsed=(time.monotonic()-base)*1000
                assert frame(new)==(3,2);new.sendall(packet(4,2))
                for seq in (3,4):assert frame(new)==(3,seq);new.sendall(packet(4,seq))
                sender.wait(timeout=2);passed=sender.returncode==0 and MIN_MS<=elapsed<=MAX_MS
        else:
            wire={'wrong-type':packet(1,1),'unknown-sequence':packet(2,0),'payload':packet(2,1,b'x'),'wrong-crc':packet(2,1)[:-1]+bytes([packet(2,1)[-1]^1])}[mode]
            c.sendall(wire);sender.wait(timeout=2);elapsed=None;passed=sender.returncode==1
    finally:
        r.finish()
        if c:c.close()
        if new:new.close()
        ls.close()
    error=r.text('tcp_sender','stderr')
    if mode=='wrong-crc':passed &= 'CRC mismatch' in error
    if mode in ('wrong-type','payload'):passed &= 'invalid response packet' in error
    if mode=='unknown-sequence':passed &= 'unexpected response type or sequence' in error
    if mode=='heartbeat-partial':passed &= 'partial response deadline exceeded' in error
    if mode=='stale-deadline':passed &= 'HEARTBEAT timeout seq=2' in r.text('tcp_sender')
    return r.result({'scenario':'tcp-'+mode,'passed':passed,'outcome_ms':round(elapsed,3) if elapsed else None,'error':error.strip()})

def udp_guard(build,work,mode):
    r=Run(work/('udp-'+mode),build);sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(('127.0.0.1',6000));sock.settimeout(3)
    try:
        sender=r.launch('udp_sender','out-of-order');data,addr=sock.recvfrom(2048);assert decode(data)==(1,1)
        if mode=='stale-deadline':
            sock.sendto(packet(2,1),addr);data,addr=sock.recvfrom(2048);assert decode(data)==(1,3);sock.sendto(packet(2,3),addr)
            data,addr=sock.recvfrom(2048);assert decode(data)==(1,2);base=time.monotonic();r.event('peer','DATA seq=2 received')
            until=base+2
            while time.monotonic()<until:
                sock.sendto(packet(2,3),addr);r.event('peer','stale ACK seq=3 sent while expecting seq=2')
                if select.select([sock],[],[],.06)[0]:break
            data,addr=sock.recvfrom(2048);assert decode(data)==(1,2);elapsed=(time.monotonic()-base)*1000;r.event('peer','retry DATA seq=2 received');sock.sendto(packet(2,2),addr)
            sender.wait(timeout=2);passed=sender.returncode==0 and MIN_MS<=elapsed<=MAX_MS
        else:
            wire={'wrong-type':packet(4,1),'unknown-sequence':packet(2,0),'payload':packet(2,1,b'x'),'wrong-crc':packet(2,1)[:-1]+bytes([packet(2,1)[-1]^1])}[mode]
            sock.sendto(wire,addr);sender.wait(timeout=2);elapsed=None;passed=sender.returncode==1
    finally:r.finish();sock.close()
    error=r.text('udp_sender','stderr')
    if mode=='wrong-crc':passed &= 'CRC mismatch' in error
    if mode in ('wrong-type','payload'):passed &= 'expected empty ACK packet' in error
    if mode=='unknown-sequence':passed &= 'ACK sequence mismatch' in error
    if mode=='stale-deadline':passed &= 'ACK timeout seq=2' in r.text('udp_sender') and 'Received ACK seq=2' in r.text('udp_sender')
    return r.result({'scenario':'udp-'+mode,'passed':passed,'outcome_ms':round(elapsed,3) if elapsed else None,'error':error.strip()})

def main():
    work=pathlib.Path(tempfile.mkdtemp(prefix='reliability-guards-'));print('Raw evidence:',work,flush=True);build=build_sources(ROOT,work);results=[]
    for mode in ('wrong-type','unknown-sequence','payload','wrong-crc','stale-deadline','heartbeat-partial'):
        x=tcp_guard(build,work,mode);results.append(x);print(x['scenario'],x['passed'],flush=True)
    for mode in ('wrong-type','unknown-sequence','payload','wrong-crc','stale-deadline'):
        x=udp_guard(build,work,mode);results.append(x);print(x['scenario'],x['passed'],flush=True)
    save(SUITE/'guard-summary.json',{'configured_timeout_ms':1000,'tolerance_ms':[MIN_MS,MAX_MS],'total':len(results),'passed':sum(x['passed'] for x in results),'runs':results})
    raise SystemExit(0 if all(x['passed'] for x in results) else 1)
if __name__=='__main__':main()
