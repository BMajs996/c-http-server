"""Single-backend proxy checks with controlled HTTP wire fixtures."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
PROJECT = Path(__file__).resolve().parents[1]
LARGE = bytes(range(256)) * 32768

class Backend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    request_queue_size = 128  # Keep the fixture backlog above test concurrency.
    daemon_threads = True

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(4)
        try:
            data = b''
            while b'\r\n\r\n' not in data:
                part = self.request.recv(8192)
                if not part: return
                data += part
            headers, body = data.split(b'\r\n\r\n', 1)
            lines = headers.decode('latin1').split('\r\n')
            method, target, _ = lines[0].split(' ')
            fields = [line.split(':', 1) for line in lines[1:]]
            length = int(next((v.strip() for k,v in fields if k.lower() == 'content-length'), '0'))
            if target == '/early':
                self.request.sendall(b'HTTP/1.1 413 Too Large\r\nContent-Length: 0\r\n\r\n'); return
            while len(body) < length:
                part = self.request.recv(length-len(body))
                if not part: return
                body += part
            if target == '/slow-headers': time.sleep(.8)
            if target == '/stall-body':
                self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\nx'); time.sleep(.8); return
            if target == '/zero': self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n'); return
            if target == '/205': self.request.sendall(b'HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n'); return
            if target == '/fragmented-headers':
                wire=b'HTTP/1.1 200 OK\r\nX-Padding: '+b'x'*7900+b'\r\nContent-Length: 2\r\n\r\nok'
                for offset in range(0,len(wire),7): self.request.sendall(wire[offset:offset+7])
                return
            if target == '/chunked':
                wire = b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Sum\r\n\r\n3;foo="a;b"\r\nabc\r\n4\r\ndefg\r\n0\r\nX-Sum: yes\r\n\r\n'
                for b in wire: self.request.sendall(bytes([b]))
                return
            if target == '/eof': self.request.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nclose body'); return
            if target == '/bad': self.request.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n'); return
            if target == '/duplicate': self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx'); return
            if target == '/upgrade': self.request.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n'); return
            if target == '/bad-chunk': self.request.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n'); return
            if target == '/truncated': self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort'); return
            if target == '/oversized': self.request.sendall(b'HTTP/1.1 200 OK\r\nX: '+b'x'*8192+b'\r\n\r\n'); return
            if target == '/info': self.request.sendall(b'HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n')
            if target == '/info-loop': self.request.sendall(b'HTTP/1.1 100 Continue\r\n\r\n'*9); return
            if target == '/204': self.request.sendall(b'HTTP/1.1 204 No Content\r\n\r\n'); return
            if target == '/304': self.request.sendall(b'HTTP/1.1 304 Not Modified\r\nContent-Length: 17\r\nETag: "v"\r\n\r\n'); return
            if target in ['/large', '/slow-body']:
                self.request.sendall(f'HTTP/1.1 200 OK\r\nContent-Length: {len(LARGE)}\r\n\r\n'.encode())
                for offset in range(0,len(LARGE),32768):
                    self.request.sendall(LARGE[offset:offset+32768])
                    if target == '/slow-body': time.sleep(.002)
                return
            payload = json.dumps({'method':method,'target':target,'headers':[(k,v.strip()) for k,v in fields],'body':body.decode('latin1')}).encode()
            status = b'500 Internal Server Error' if target == '/500' else b'200 OK'
            wire = b'HTTP/1.1 '+status+b'\r\nContent-Length: '+str(len(payload)).encode()+b'\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nConnection: close, X-Secret\r\nX-Secret: remove\r\nContent-Type: application/json\r\n\r\n'
            self.request.sendall(wire)
            if method != 'HEAD': self.request.sendall(payload)
        except (OSError, ValueError): pass

class ProxyHarness(unittest.TestCase):
    backend_type = Backend
    handler_type = Handler
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        cls.backend = cls.backend_type(('127.0.0.1',0),cls.handler_type)
        cls.thread = threading.Thread(target=cls.backend.serve_forever,daemon=True);cls.thread.start()
        cls.backend_port = cls.backend.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.backend.shutdown();cls.backend.server_close();cls.thread.join();cls.temp.cleanup()

    def setUp(self):
        self.procs=[]
        self.proc,self.port=self.launch()

    def tearDown(self):
        for proc in self.procs:
            if proc.poll() is None: proc.terminate()
            _,err=proc.communicate(timeout=10)
            self.assertNotIn(b'ERROR: AddressSanitizer',err)
            self.assertNotIn(b'runtime error:',err)

    def launch(self, **options):
        with socket.socket() as probe: probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        settings={'port':port,'document_root':str(PROJECT/'public'),'access_log':'0','proxy_prefix':'/proxy/','upstream_address':'127.0.0.1','upstream_port':self.backend_port,'upstream_host':'backend.test:9000','upstream_response_timeout_ms':3000}
        settings.update(options)
        path=self.base/f'{port}.conf';path.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
        env={k:v for k,v in os.environ.items() if k not in ['NOTIFY_SOCKET','C_HTTP_ACCESS_LOG']}
        proc=subprocess.Popen([str(PROJECT/'http_server'),'--config',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        self.procs.append(proc)
        assert proc.stdout is not None
        self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
        return proc,port

    def request(self,path,method='GET',body=None,headers=None,port=None,context=None):
        client=http.client.HTTPSConnection('localhost',port or self.port,context=context,timeout=5) if context else http.client.HTTPConnection('127.0.0.1',port or self.port,timeout=5)
        try:
            client.request(method,path,body=body,headers=headers or {})
            response=client.getresponse();return response.status,response.getheaders(),response.read()
        finally: client.close()

    def raw(self,wire,port=None):
        with socket.create_connection(('127.0.0.1',port or self.port),timeout=5) as sock:
            sock.sendall(wire);data=b''
            while True:
                part=sock.recv(65536)
                if not part: break
                data+=part
            return data

class ProxyTests(ProxyHarness):
    def test_methods_paths_and_headers(self):
        for method in ['GET','HEAD','POST','PUT','PATCH','DELETE','OPTIONS']:
            status,headers,body=self.request('/proxy/items?q=%2F&x=1',method,None if method=='HEAD' else b'\x00binary\xff',{'Content-Type':'application/octet-stream','Connection':'close, X-Hop','X-Hop':'secret','Forwarded':'for=spoof','X-Forwarded-For':'spoof','X-Custom':'keep','Authorization':'Bearer backend'})
            self.assertEqual(status,200)
            self.assertEqual([v for k,v in headers if k.lower()=='set-cookie'],['a=1','b=2'])
            self.assertNotIn('x-secret',dict((k.lower(),v) for k,v in headers))
            if method=='HEAD': self.assertEqual(body,b'');continue
            data=json.loads(body);forwarded=dict((k.lower(),v) for k,v in data['headers'])
            self.assertEqual(data['method'],method);self.assertEqual(data['target'],'/items?q=%2F&x=1')
            self.assertEqual(data['body'],'\x00binaryÿ');self.assertEqual(forwarded['host'],'backend.test:9000')
            self.assertEqual(forwarded['x-custom'],'keep');self.assertNotIn('x-hop',forwarded)
            self.assertNotIn('forwarded',forwarded);self.assertEqual(forwarded['x-forwarded-for'],'127.0.0.1')
            self.assertEqual(forwarded['authorization'],'Bearer backend')

    def test_routing_and_request_limits(self):
        status,headers,_=self.request('/proxy?a=1');self.assertEqual(status,308);self.assertEqual(dict(headers)['Location'],'/proxy/?a=1')
        self.assertEqual(self.request('/proxy/')[0],200)
        query='q='+('x'*1800)
        status,headers,_=self.request('/proxy?'+query)
        self.assertEqual(status,308);self.assertEqual(dict(headers)['Location'],'/proxy/?'+query)
        self.assertEqual(self.request('/proxy-other')[0],404)
        self.assertEqual(self.request('/health')[0],200)
        self.assertEqual(self.request('/api/private/status')[0],401)
        self.assertEqual(self.request('/proxy/x','TRACE')[0],405)
        self.assertEqual(self.request('/proxy/x','POST',b'x'*16384)[0],200)
        self.assertEqual(self.request('/proxy/x','POST',b'x'*16385)[0],413)
        for header,status in [('Connection: Content-Length\r\n',400),('Upgrade: websocket\r\n',501),('Expect: 100-continue\r\n',417),('Transfer-Encoding: chunked\r\n',501)]:
            result=self.raw(f'GET /proxy/x HTTP/1.1\r\nHost: a\r\nConnection: close\r\n{header}\r\n'.encode())
            self.assertIn(f' {status} '.encode(),result)

    def test_response_framing_and_backend_errors(self):
        self.assertEqual(self.request('/proxy/chunked')[2],b'abcdefg')
        status,headers,body=self.request('/proxy/fragmented-headers')
        self.assertEqual(status,200);self.assertEqual(body,b'ok')
        self.assertEqual(len(dict(headers)['X-Padding']),7900)
        self.assertEqual(self.request('/proxy/eof')[2],b'close body')
        self.assertEqual(self.request('/proxy/204')[2],b'')
        self.assertEqual(self.request('/proxy/205')[2],b'')
        self.assertEqual(self.request('/proxy/zero')[2],b'')
        self.assertEqual(self.request('/proxy/304')[2],b'')
        self.assertEqual(self.request('/proxy/500')[0],500)
        self.assertEqual(self.request('/proxy/early','POST',b'x'*16384)[0],413)
        for path in ['bad','duplicate','upgrade','oversized','info-loop']:
            self.assertEqual(self.request('/proxy/'+path)[0],502,path)
        for path in ['bad-chunk','truncated']:
            with self.assertRaises(http.client.IncompleteRead): self.request('/proxy/'+path)

    def test_http10_and_informational(self):
        for path,expected in [('eof',b'close body'),('chunked',b'abcdefg')]:
            result=self.raw(f'GET /proxy/{path} HTTP/1.0\r\nConnection: keep-alive\r\n\r\n'.encode())
            self.assertIn(b'Connection: close',result);self.assertTrue(result.endswith(expected))
            self.assertNotIn(b'Transfer-Encoding',result)
        result=self.raw(b'GET /proxy/info HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
        self.assertNotIn(b'100 Continue',result);self.assertIn(b'103 Early Hints',result);self.assertIn(b'200 OK',result)
        result=self.raw(b'GET /proxy/info HTTP/1.0\r\n\r\n')
        self.assertNotIn(b'103 Early Hints',result);self.assertNotIn(b'100 Continue',result)
        self.assertIn(b'HTTP/1.0 200 OK',result)

    def test_pipeline_and_large_response(self):
        result=self.raw(b'GET /proxy/large HTTP/1.1\r\nHost: a\r\n\r\nGET /health HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
        _,rest=result.split(b'\r\n\r\n',1);self.assertEqual(rest[:len(LARGE)],LARGE)
        self.assertIn(b'200 OK',rest[len(LARGE):]);self.assertTrue(rest.endswith(b'{"status":"ok"}\n'))

    def test_timeouts_refusal_and_local_responsiveness(self):
        _,idle_port=self.launch(upstream_response_timeout_ms=2000,upstream_read_timeout_ms=100)
        started=time.monotonic()
        self.assertEqual(self.request('/proxy/slow-headers',port=idle_port)[0],504)
        self.assertLess(time.monotonic()-started,.7)
        _,port=self.launch(upstream_response_timeout_ms=150)
        started=time.monotonic();self.assertEqual(self.request('/proxy/slow-headers',port=port)[0],504)
        self.assertLess(time.monotonic()-started,1)
        self.assertEqual(self.request('/ready',port=port)[0],200)
        with self.assertRaises(http.client.IncompleteRead): self.request('/proxy/stall-body',port=port)
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1',0));bad_port=unavailable.getsockname()[1]
        _,port=self.launch(upstream_port=bad_port)
        self.assertEqual(self.request('/proxy/x',port=port)[0],502)
        self.assertEqual(self.request('/health',port=port)[0],200)

    def test_churn_and_slow_readers(self):
        with socket.create_connection(('127.0.0.1',self.port),timeout=5) as slow:
            slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
            slow.sendall(b'GET /proxy/large HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            def work(_): return self.request('/proxy/x')[0]
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                self.assertTrue(all(x==200 for x in pool.map(work,range(200))))
            self.assertEqual(self.request('/health')[0],200)
        time.sleep(.1)
        self.assertEqual(self.request('/ready')[0],200)

    def test_shutdown_during_active_proxy(self):
        with socket.create_connection(('127.0.0.1',self.port),timeout=5) as sock:
            sock.sendall(b'GET /proxy/slow-body HTTP/1.1\r\nHost: a\r\n\r\n')
            data=sock.recv(65536);self.proc.send_signal(signal.SIGTERM)
            while True:
                part=sock.recv(65536)
                if not part: break
                data+=part
            _,body=data.split(b'\r\n\r\n',1);self.assertEqual(body,LARGE)
        self.proc.wait(timeout=8)

    def test_disconnect_cleanup_and_metrics(self):
        before=int(next(line.split()[1] for line in self.request('/metrics')[2].decode().splitlines() if line.startswith('c_http_proxy_requests_total ')))
        sockets=[]
        for _ in range(20):
            sock=socket.create_connection(('127.0.0.1',self.port),timeout=3)
            sock.sendall(b'GET /proxy/large HTTP/1.1\r\nHost: a\r\n\r\n');sock.recv(1024);sockets.append(sock)
        for sock in sockets: sock.close()
        deadline=time.monotonic()+4
        while True:
            values={line.split()[0]:line.split()[1] for line in self.request('/metrics')[2].decode().splitlines() if line and not line.startswith('#')}
            if values['c_http_upstream_active_connections']=='0': break
            self.assertLess(time.monotonic(),deadline);time.sleep(.02)
        self.assertGreaterEqual(int(values['c_http_proxy_requests_total']),before+20)
        self.assertEqual(self.request('/ready')[0],200)

    def test_ipv6_upstream(self):
        class Backend6(Backend): address_family=socket.AF_INET6
        try: backend=Backend6(('::1',0),Handler)
        except OSError: self.skipTest('IPv6 loopback unavailable')
        thread=threading.Thread(target=backend.serve_forever,daemon=True);thread.start()
        try:
            _,port=self.launch(upstream_address='::1',upstream_port=backend.server_address[1],upstream_host='[::1]:9000')
            data=json.loads(self.request('/proxy/x',port=port)[2])
            self.assertEqual(dict((k.lower(),v) for k,v in data['headers'])['host'],'[::1]:9000')
        finally: backend.shutdown();backend.server_close();thread.join()

    def test_configuration_validation(self):
        valid={'port':self.port,'document_root':str(PROJECT/'public'),'access_log':'0','proxy_prefix':'/proxy/','upstream_address':'127.0.0.1','upstream_port':self.backend_port,'upstream_host':'a'}
        cases=[{'proxy_prefix':'/'},{'proxy_prefix':'/api/private/'},{'upstream_address':'localhost'},{'upstream_host':'bad host'},{'upstream_port':self.port},{'upstream_address':'0.0.0.0'},{'upstream_host':''},{'upstream_address':'::ffff:127.0.0.1','upstream_port':self.port}]
        for update in [{}]+cases:
            path=self.base/'check.conf';settings={**valid,**update};path.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
            result=subprocess.run([str(PROJECT/'http_server'),'--check-config',str(path)],capture_output=True,timeout=5,env={**os.environ,'C_HTTP_ACCESS_LOG':'0'})
            self.assertEqual(result.returncode==0,not bool(update),update)

    @unittest.skipIf(os.environ.get('C_HTTP_TEST_TLS')=='0','TLS build disabled')
    def test_https_client(self):
        cert=self.base/'cert.pem';key=self.base/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,capture_output=True)
        _,port=self.launch(tls_cert=str(cert),tls_key=str(key))
        context=ssl.create_default_context(cafile=str(cert))
        self.assertEqual(self.request('/proxy/large',port=port,context=context)[2],LARGE)
        data=json.loads(self.request('/proxy/x',port=port,context=context)[2])
        self.assertEqual(dict((k.lower(),v) for k,v in data['headers'])['x-forwarded-proto'],'https')

if __name__=='__main__': unittest.main(verbosity=2)
