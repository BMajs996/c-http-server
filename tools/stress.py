"""Stress a running local server: idle clients, slow readers, then request churn."""
import argparse, json, socket, time, ssl
from benchmark import run

def main():
    p=argparse.ArgumentParser();p.add_argument('--host',default='127.0.0.1');p.add_argument('--port',type=int,default=8080)
    p.add_argument('--idle',type=int,default=300);p.add_argument('--slow',type=int,default=8)
    p.add_argument('--slow-path',default='/');p.add_argument('--requests',type=int,default=2000)
    p.add_argument('--concurrency',type=int,default=32);p.add_argument('--https',action='store_true');p.add_argument('--ca-file')
    a=p.parse_args()
    if not 0<=a.idle<=1000 or not 0<=a.slow<=128 or not 1<=a.requests<=100000 or not 1<=a.concurrency<=256:p.error('Invalid workload bounds')
    context=ssl.create_default_context(cafile=a.ca_file) if a.https else None
    held=[]
    try:
        for i in range(a.idle+a.slow):
            s=socket.create_connection((a.host,a.port),timeout=3)
            if context:
                try:s=context.wrap_socket(s,server_hostname=a.host)
                except Exception:s.close();raise
            held.append(s)
            if i>=a.idle:
                s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
                path=a.slow_path
                if not path.startswith('/') or '\r' in path or '\n' in path:raise ValueError('Invalid slow path')
                s.sendall(f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode('ascii'))
        started=time.perf_counter()
        result=run(a.host,a.port,'/health',a.concurrency,a.requests,0,False,context)
        result.update({'idle_connections':a.idle,'slow_readers':a.slow,'slow_path':a.slow_path,
                       'note':'Idle sockets can expire after 5 seconds; use a large slow-path file to create sustained backpressure.'})
        print(json.dumps(result,indent=2));return int(result['errors']!=0)
    finally:
        for s in held:s.close()
if __name__=='__main__':raise SystemExit(main())
