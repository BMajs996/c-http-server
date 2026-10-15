"""Persistent backend fixtures for bounded pool and upstream TLS behavior."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import signal
import ssl
import subprocess
import socketserver
import threading
import time
import unittest
from test_proxy import Backend, ProxyHarness, PROJECT, LARGE

class PersistentHandler(socketserver.StreamRequestHandler):
    def handle(self):
        with self.server.counter_lock:
            self.server.accepts+=1;identity=self.server.accepts
        self.request.settimeout(5)
        try:
            while True:
                line=self.rfile.readline(8193)
                if not line: return
                method,target,_=line.decode().strip().split(' ')
                headers=http.client.parse_headers(self.rfile)
                body=self.rfile.read(int(headers.get('Content-Length','0')))
                if target=='/slow': time.sleep(.5)
                if target=='/read-stall': time.sleep(.5)
                if target=='/chunked':
                    self.wfile.write(f'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Connection: {identity}\r\n\r\n3\r\nabc\r\n0\r\n\r\n'.encode());self.wfile.flush();continue
                if target=='/dribble':
                    self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n')
                    for _ in range(20):self.wfile.write(b'x');self.wfile.flush();time.sleep(.04)
                    continue
                payload=json.dumps({'connection':identity,'body':body.decode('latin1'),'method':method,'client_certificate':(self.request.getpeercert() or {}).get('subject',[]) if isinstance(self.request,ssl.SSLSocket) else [],'sni':getattr(self.request,'observed_sni',None),'forwarded_proto':headers.get('X-Forwarded-Proto')}).encode()
                close=target in ['/close','/eof','/truncated'] or headers.get('Connection','').lower()=='close'
                version='HTTP/1.0' if target=='/http10' else 'HTTP/1.1'
                status=204 if target=='/204' else 304 if target=='/304' else 200
                length=100 if target=='/truncated' else 17 if target=='/304' else len(payload)
                if target in ['/extra','/late-extra']: payload=b'ok';length=2
                if target=='/large':payload=LARGE;length=len(payload)
                self.wfile.write(f'{version} {status} OK\r\nX-Connection: {identity}\r\n'.encode())
                if target!='/eof' and status!=204:self.wfile.write(f'Content-Length: {length}\r\n'.encode())
                self.wfile.write(f'Connection: {"close" if close else "keep-alive"}\r\n\r\n'.encode())
                if method!='HEAD' and status not in [204,304]:self.wfile.write(b'x' if target=='/truncated' else payload)
                if target=='/extra':self.wfile.write(b'UNSOLICITED')
                self.wfile.flush()
                if target=='/hold-close':time.sleep(.4)
                if target=='/late-extra':time.sleep(.05);self.wfile.write(b'UNSOLICITED');self.wfile.flush();return
                if close:return
        except (OSError,ValueError):pass

class PersistentBackend(Backend):
    def __init__(self,*args,**kwargs):
        self.accepts=0;self.counter_lock=threading.Lock();super().__init__(*args,**kwargs)
    def get_request(self):
        sock,address=super().get_request();sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        return sock,address

class UpstreamTests(ProxyHarness):
    backend_type=PersistentBackend
    handler_type=PersistentHandler
    def launch(self,**options):
        return super().launch(**{'upstream_pool_enabled':1,**options})
    def identity(self,path='/id',port=None):
        status,headers,body=self.request('/proxy'+path,port=port)
        self.assertEqual(status,200,body)
        return int(dict(headers)['X-Connection'])
    def metrics(self,port=None):
        return {line.split()[0]:float(line.split()[1]) for line in self.request('/metrics',port=port)[2].decode().splitlines() if line and not line.startswith('#')}
    def wait_metric(self,name,value,port=None):
        deadline=time.monotonic()+3
        while self.metrics(port).get(name)!=value:
            self.assertLess(time.monotonic(),deadline,name);time.sleep(.02)

    def test_reuse_across_client_connections(self):
        identities=[self.identity() for _ in range(30)]
        self.assertEqual(len(set(identities)),1)
        values=self.metrics();self.assertEqual(values['c_http_upstream_connections_opened_total'],1)
        self.assertEqual(values['c_http_upstream_connections_reused_total'],29)
        self.assertEqual(values['c_http_upstream_pool_idle'],1)

    def test_pool_off_preserves_fresh_connections(self):
        _,port=self.launch(upstream_pool_enabled=0)
        self.assertNotEqual(self.identity(port=port),self.identity(port=port))
        self.assertEqual(self.metrics(port)['c_http_upstream_pool_idle'],0)

    def test_capacity_and_idle_limit(self):
        _,port=self.launch(upstream_pool_max_connections=2,upstream_pool_idle_limit=1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            requests=[executor.submit(self.request,'/proxy/slow',port=port) for _ in range(2)]
            self.wait_metric('c_http_upstream_pool_leased',2,port)
            started=time.monotonic();self.assertEqual(self.request('/proxy/id',port=port)[0],503)
            self.assertLess(time.monotonic()-started,.3)
            self.assertEqual(self.request('/ready',port=port)[0],200)
            for future in requests:self.assertEqual(future.result()[0],200)
        self.wait_metric('c_http_upstream_pool_idle',1,port)
        self.assertLessEqual(self.metrics(port)['c_http_upstream_pool_total'],2)

    def test_expiry_age_and_request_cap(self):
        for options in [{'upstream_pool_idle_timeout_ms':80},{'upstream_pool_max_age_ms':80}]:
            _,port=self.launch(**options)
            first=self.identity(port=port);time.sleep(.25)
            self.assertNotEqual(first,self.identity(port=port))
        _,port=self.launch(upstream_pool_max_requests=2)
        first=self.identity(port=port);self.assertEqual(first,self.identity(port=port))
        self.assertNotEqual(first,self.identity(port=port))

    def test_nonreusable_responses(self):
        for path in ['/close','/eof','/http10','/extra']:
            first=self.identity(path)
            self.assertNotEqual(first,self.identity(),path)
        with self.assertRaises(http.client.IncompleteRead):self.request('/proxy/truncated')
        self.assertEqual(self.request('/ready')[0],200)

    def test_bodyless_responses_and_idle_data(self):
        first=self.identity()
        for path,method in [('/id','HEAD'),('/204','GET'),('/304','GET')]:
            status,headers,body=self.request('/proxy'+path,method)
            self.assertEqual(body,b'');self.assertEqual(int(dict(headers)['X-Connection']),first)
        self.assertEqual(first,self.identity())
        status,headers,body=self.request('/proxy/chunked')
        self.assertEqual((status,body),(200,b'abc'));self.assertEqual(int(dict(headers)['X-Connection']),first)
        extra=self.identity('/late-extra');self.wait_metric('c_http_upstream_pool_idle',0)
        self.assertNotEqual(extra,self.identity())

    def test_read_idle_is_independent_of_total(self):
        _,port=self.launch(upstream_read_timeout_ms=80,upstream_response_timeout_ms=2000)
        started=time.monotonic();self.assertEqual(self.request('/proxy/read-stall',port=port)[0],504)
        self.assertLess(time.monotonic()-started,.5)
        self.assertEqual(self.request('/ready',port=port)[0],200)

    def test_idle_close_and_churn(self):
        first=self.identity();self.assertEqual(self.metrics()['c_http_upstream_pool_idle'],1)
        # Backend restarts/idle closes are observed without a new client request.
        time.sleep(5.2)
        self.wait_metric('c_http_upstream_pool_idle',0)
        self.assertNotEqual(first,self.identity())
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            self.assertTrue(all(status==200 for status in executor.map(lambda _:self.request('/proxy/id')[0],range(200))))
        values=self.metrics();self.assertLessEqual(values['c_http_upstream_pool_idle'],16)
        self.assertLessEqual(values['c_http_upstream_pool_total'],64)

    def test_total_deadline_overrides_read_progress(self):
        _,port=self.launch(upstream_read_timeout_ms=150,upstream_response_timeout_ms=250)
        started=time.monotonic()
        with self.assertRaises(http.client.IncompleteRead):self.request('/proxy/dribble',port=port)
        self.assertLess(time.monotonic()-started,.7)
        self.assertEqual(self.request('/ready',port=port)[0],200)

    @unittest.skipUnless(os.environ.get('C_HTTP_TEST_TLS')=='0','HTTP-only build check')
    def test_http_only_rejects_upstream_tls(self):
        path=self.base/'unsupported-tls.conf'
        path.write_text('document_root='+str(PROJECT/'public')+'\nproxy_prefix=/proxy/\nupstream_address=127.0.0.1\nupstream_port=9000\nupstream_host=a\nupstream_tls=1\nupstream_tls_server_name=localhost\nupstream_tls_ca_file=/unused\n')
        result=subprocess.run([str(PROJECT/'http_server'),'--check-config',str(path)],capture_output=True)
        self.assertNotEqual(result.returncode,0);self.assertIn(b'TLS=1',result.stderr)

def certificates(base):
    def run(*args):subprocess.run(['openssl',*args],check=True,capture_output=True)
    for ca in ['server-ca','client-ca','wrong-ca']:
        run('req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(base/(ca+'.key')),'-out',str(base/(ca+'.pem')),'-days','2','-subj','/CN='+ca,'-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign')
    for name,ca,purpose,days in [('server','server-ca','serverAuth','1'),('expired-server','server-ca','serverAuth','-1'),('client','client-ca','clientAuth','1'),('wrong-client','wrong-ca','clientAuth','1'),('expired-client','client-ca','clientAuth','-1')]:
        run('req','-new','-newkey','rsa:2048','-nodes','-keyout',str(base/(name+'.key')),'-out',str(base/(name+'.csr')),'-subj','/CN='+name)
        ext=base/(name+'.ext');ext.write_text('basicConstraints=CA:FALSE\nextendedKeyUsage='+purpose+'\nsubjectAltName=DNS:backend.test,DNS:localhost,IP:127.0.0.1\n')
        if days=='-1':
            index=base/(name+'.index');index.touch()
            serial=base/(name+'.serial');serial.write_text('1000\n')
            config=base/(name+'.conf')
            config.write_text(f'[ca]\ndefault_ca=issuer\n[issuer]\ndatabase={index}\nserial={serial}\nnew_certs_dir={base}\ncertificate={base/(ca+".pem")}\nprivate_key={base/(ca+".key")}\ndefault_md=sha256\npolicy=policy\n[policy]\ncommonName=supplied\n')
            run('ca','-batch','-config',str(config),'-in',str(base/(name+'.csr')),'-out',str(base/(name+'.pem')),'-startdate','20200101000000Z','-enddate','20200102000000Z','-extfile',str(ext))
        else:
            run('x509','-req','-in',str(base/(name+'.csr')),'-CA',str(base/(ca+'.pem')),'-CAkey',str(base/(ca+'.key')),'-CAcreateserial','-out',str(base/(name+'.pem')),'-days',days,'-extfile',str(ext))
    run('pkey','-in',str(base/'client.key'),'-aes-256-cbc','-passout','pass:test-password','-out',str(base/'encrypted-client.key'))

class TLSBackend(PersistentBackend):
    def __init__(self,address,handler,context):
        self.context=context;super().__init__(address,handler)
    def get_request(self):
        sock,address=super().get_request()
        return self.context.wrap_socket(sock,server_side=True,do_handshake_on_connect=False),address

@unittest.skipIf(os.environ.get('C_HTTP_TEST_TLS')=='0','TLS build disabled')
class UpstreamTLSTests(ProxyHarness):
    @classmethod
    def setUpClass(cls):
        super().setUpClass();certificates(cls.base)
    def tls_backend(self,mtls=False,cert='server'):
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.base/(cert+'.pem')),str(self.base/(cert+'.key')))
        context.set_alpn_protocols(['http/1.1'])
        def sni(sock,name,ctx):sock.observed_sni=name
        context.set_servername_callback(sni)
        if mtls:
            context.verify_mode=ssl.CERT_REQUIRED;context.load_verify_locations(str(self.base/'client-ca.pem'))
        backend=TLSBackend(('127.0.0.1',0),PersistentHandler,context)
        thread=threading.Thread(target=backend.serve_forever,daemon=True);thread.start()
        self.addCleanup(lambda:(backend.shutdown(),backend.server_close(),thread.join()))
        return backend.server_address[1]
    def tls_proxy(self,backend,client=None,**options):
        settings={'upstream_port':backend,'upstream_tls':1,'upstream_tls_server_name':'backend.test','upstream_tls_ca_file':str(self.base/'server-ca.pem')}
        if client:settings.update(upstream_tls_client_cert=str(self.base/(client+'.pem')),upstream_tls_client_key=str(self.base/(client+'.key')))
        return self.launch(**{**settings,**options})[1]
    def result(self,port,path='/id',context=None):
        status,headers,body=self.request('/proxy'+path,port=port,context=context)
        self.assertEqual(status,200,body);return json.loads(body)
    def test_verified_https_without_pool(self):
        port=self.tls_proxy(self.tls_backend())
        first=self.result(port);second=self.result(port)
        self.assertNotEqual(first['connection'],second['connection'])
        self.assertEqual(first['sni'],'backend.test');self.assertEqual(first['forwarded_proto'],'http')
        self.assertEqual(first['client_certificate'],[])
    def test_mtls_without_pool(self):
        backend=self.tls_backend(mtls=True)
        port=self.tls_proxy(backend,'client')
        data=self.result(port);self.assertIn('client',str(data['client_certificate']))
        for client in [None,'wrong-client','expired-client']:
            port=self.tls_proxy(backend,client)
            self.assertEqual(self.request('/proxy/id',port=port)[0],502,client)
    def test_server_identity_and_trust(self):
        backend=self.tls_backend()
        for options in [{'upstream_tls_server_name':'wrong.test'},{'upstream_tls_ca_file':str(self.base/'wrong-ca.pem')},{'upstream_tls_server_name':'127.0.0.2'}]:
            port=self.tls_proxy(backend,**options)
            self.assertEqual(self.request('/proxy/id',port=port)[0],502,options)
        port=self.tls_proxy(backend,upstream_tls_server_name='127.0.0.1')
        self.assertIsNone(self.result(port)['sni'])
        port=self.tls_proxy(self.tls_backend(cert='expired-server'))
        self.assertEqual(self.request('/proxy/id',port=port)[0],502)
    def test_handshake_deadline(self):
        class Stall(socketserver.BaseRequestHandler):
            def handle(self):time.sleep(.8)
        backend=Backend(('127.0.0.1',0),Stall);thread=threading.Thread(target=backend.serve_forever,daemon=True);thread.start()
        try:
            port=self.tls_proxy(backend.server_address[1],upstream_handshake_timeout_ms=80,upstream_response_timeout_ms=2000)
            started=time.monotonic();self.assertEqual(self.request('/proxy/id',port=port)[0],504)
            self.assertLess(time.monotonic()-started,.7)
        finally:backend.shutdown();backend.server_close();thread.join()
    def test_tls_configuration_validation(self):
        defaults={'document_root':str(PROJECT/'public'),'access_log':'0','proxy_prefix':'/proxy/','upstream_address':'127.0.0.1','upstream_port':9000,'upstream_host':'a','upstream_tls':1,'upstream_tls_server_name':'backend.test','upstream_tls_ca_file':str(self.base/'server-ca.pem')}
        cases=[{'upstream_tls':0},{'upstream_tls_ca_file':''},{'upstream_tls_server_name':''},{'upstream_tls_server_name':'bad name'},{'upstream_tls_client_cert':str(self.base/'client.pem')},{'upstream_tls_client_cert':str(self.base/'client.pem'),'upstream_tls_client_key':str(self.base/'server.key')},{'upstream_tls_client_cert':str(self.base/'client.pem'),'upstream_tls_client_key':str(self.base/'encrypted-client.key')},{'upstream_tls_ca_file':str(self.base/'missing')}]
        for changes in cases:
            path=self.base/'tls-check.conf';path.write_text(''.join(f'{k}={v}\n' for k,v in {**defaults,**changes}.items()))
            result=subprocess.run([str(PROJECT/'http_server'),'--check-config',str(path)],capture_output=True,timeout=3)
            self.assertNotEqual(result.returncode,0,changes);self.assertNotIn(b'Enter PEM pass phrase',result.stderr)
    def test_https_and_mtls_pool_reuse(self):
        for mtls in [False,True]:
            port=self.tls_proxy(self.tls_backend(mtls=mtls),'client' if mtls else None,upstream_pool_enabled=1)
            results=[self.result(port) for _ in range(30)]
            self.assertEqual(len({data['connection'] for data in results}),1)
            status,headers,body=self.request('/proxy/chunked',port=port)
            self.assertEqual((status,body),(200,b'abc'));self.assertEqual(int(dict(headers)['X-Connection']),results[0]['connection'])
            payload=bytes(range(64))*250
            status,_,body=self.request('/proxy/id','POST',body=payload,port=port)
            self.assertEqual(status,200);data=json.loads(body)
            self.assertEqual(data['body'],payload.decode('latin1'));self.assertEqual(data['connection'],results[0]['connection'])
            if mtls:self.assertIn('client',str(results[0]['client_certificate']))
        with self.assertRaises(http.client.IncompleteRead):self.request('/proxy/eof',port=port)
    def test_downstream_tls_is_independent(self):
        port=self.tls_proxy(self.tls_backend(mtls=True),'client',upstream_pool_enabled=1,tls_cert=str(self.base/'server.pem'),tls_key=str(self.base/'server.key'))
        context=ssl.create_default_context(cafile=str(self.base/'server-ca.pem'))
        data=self.result(port,context=context)
        self.assertEqual(data['forwarded_proto'],'https');self.assertIn('client',str(data['client_certificate']))
    def test_tls_pool_expiry_close_and_backpressure(self):
        port=self.tls_proxy(self.tls_backend(mtls=True),'client',upstream_pool_enabled=1,upstream_pool_idle_timeout_ms=80)
        first=self.result(port)['connection'];time.sleep(.3)
        self.assertNotEqual(first,self.result(port)['connection'])
        self.assertEqual(self.request('/proxy/close',port=port)[0],200)
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            self.assertTrue(all(x==200 for x in executor.map(lambda _:self.request('/proxy/id',port=port)[0],range(100))))
        self.assertEqual(self.request('/ready',port=port)[0],200)

    def test_large_tls_response_and_slow_client(self):
        port=self.tls_proxy(self.tls_backend(mtls=True),'client',upstream_pool_enabled=1,upstream_read_timeout_ms=100)
        with socket.create_connection(('127.0.0.1',port),timeout=5) as sock:
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
            sock.sendall(b'GET /proxy/large HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            time.sleep(.3)  # Downstream backpressure must pause backend read inactivity.
            response=http.client.HTTPResponse(sock);response.begin()
            self.assertEqual(response.status,200);self.assertEqual(response.read(),LARGE)
        self.assertEqual(self.result(port)['method'],'GET')

    def test_tls_idle_unsolicited_data(self):
        port=self.tls_proxy(self.tls_backend(),upstream_pool_enabled=1)
        status,headers,_=self.request('/proxy/late-extra',port=port)
        self.assertEqual(status,200);first=int(dict(headers)['X-Connection']);time.sleep(.2)
        self.assertNotEqual(first,self.result(port)['connection'])

    def test_tls_drain_and_closing_capacity(self):
        port=self.tls_proxy(self.tls_backend(),upstream_pool_enabled=1,upstream_pool_max_connections=1,upstream_pool_idle_limit=0)
        self.result(port,'/hold-close')
        # Delay the backend's next read so close_notify remains outstanding.
        self.assertEqual(self.request('/proxy/id',port=port)[0],503)
        deadline=time.monotonic()+2
        while self.request('/proxy/id',port=port)[0]==503:
            self.assertLess(time.monotonic(),deadline);time.sleep(.02)
        proc,port=self.launch(upstream_tls=1,upstream_tls_server_name='backend.test',upstream_tls_ca_file=str(self.base/'server-ca.pem'),upstream_port=self.tls_backend(mtls=True),upstream_tls_client_cert=str(self.base/'client.pem'),upstream_tls_client_key=str(self.base/'client.key'),upstream_pool_enabled=1)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future=executor.submit(self.request,'/proxy/slow',port=port);time.sleep(.1)
            started=time.monotonic();proc.send_signal(signal.SIGTERM)
            self.assertEqual(future.result()[0],200);self.assertEqual(proc.wait(timeout=3),0)
            self.assertLess(time.monotonic()-started,2)

if __name__=='__main__':unittest.main(verbosity=2)
