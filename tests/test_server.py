"""Integration checks using only the Python standard library."""
import concurrent.futures
import hashlib
import http.client
from pathlib import Path
import socket
import os
import ssl
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

PROJECT = Path(__file__).resolve().parents[1]

class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        cls.root = cls.base / 'public'
        cls.root.mkdir()
        (cls.root / 'index.html').write_bytes(b'<h1>Hello</h1>')
        (cls.root / 'style.css').write_bytes(b'body { color: green; }')
        (cls.root / 'empty.txt').touch()
        (cls.root / 'space name.txt').write_bytes(b'space')
        (cls.root / 'docs').mkdir()
        (cls.root / 'docs' / 'index.html').write_bytes(b'docs')
        (cls.root / 'noindex').mkdir()
        cls.large = bytes(range(256)) * 32768
        (cls.root / 'large.bin').write_bytes(cls.large)
        (cls.root / '.env').write_bytes(b'secret')
        (cls.base / 'secret.txt').write_bytes(b'outside-secret')
        (cls.root / 'link.txt').symlink_to(cls.base / 'secret.txt')
        (cls.root / 'linked').symlink_to(cls.base, target_is_directory=True)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            cls.port = sock.getsockname()[1]
        cls.proc = subprocess.Popen([str(PROJECT / 'http_server'), str(cls.port), str(cls.root)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={**os.environ, 'C_HTTP_ACCESS_LOG': '0'})
        assert cls.proc.stdout is not None
        assert cls.proc.stdout.readline().startswith(b'Listening'), 'Server failed to start'

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        output, errors = cls.proc.communicate(timeout=12)
        assert b"runtime error:" not in errors and b"ERROR: AddressSanitizer" not in errors, errors.decode(errors="replace")
        cls.temp.cleanup()

    def request(self, path, method='GET', headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=8)
        try:
            connection.request(method, path, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_files_and_types(self):
        for path, expected, mime in [('/', b'<h1>Hello</h1>', 'text/html; charset=utf-8'),
                                     ('/style.css?v=1', b'body { color: green; }', 'text/css; charset=utf-8'),
                                     ('/docs/', b'docs', 'text/html; charset=utf-8'),
                                     ('/empty.txt', b'', 'text/plain; charset=utf-8'),
                                     ('/space%20name.txt', b'space', 'text/plain; charset=utf-8')]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(body, expected)
                self.assertEqual(int(headers['Content-Length']), len(body))
                self.assertEqual(headers['Content-Type'], mime)
                self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')

    def test_head_and_large_file(self):
        status, headers, body = self.request('/large.bin', 'HEAD')
        self.assertEqual(status, 200)
        self.assertEqual(body, b'')
        self.assertEqual(int(headers['Content-Length']), len(self.large))
        status, headers, body = self.request('/large.bin')
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).digest(), hashlib.sha256(self.large).digest())

    def test_forbidden_paths(self):
        for path in ['/../secret.txt', '/%2e%2e/secret.txt', '/%2e%2e%2fsecret.txt',
                     '/.env', '/%2eenv', '/link.txt', '/linked/secret.txt', '/docs/../index.html']:
            with self.subTest(path=path):
                self.assertEqual(self.request(path)[0], 403)

    def test_bad_encoding(self):
        for path in ['/%', '/%0', '/%GG', '/%00', '/%5csecret', '/%252e%252e/secret.txt']:
            with self.subTest(path=path):
                self.assertEqual(self.request(path)[0], 400)

    def test_errors_and_health(self):
        self.assertEqual(self.request('/missing')[0], 404)
        self.assertEqual(self.request('/noindex/')[0], 404)
        self.assertEqual(self.request('/', 'POST')[0], 405)
        self.assertEqual(self.request('/health')[2], b'{"status":"ok"}\n')
        self.assertEqual(self.request('/missing', 'HEAD')[2], b'')

    def test_fragmented_headers_and_limit(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            for fragment in [b'GET / HTTP/1.1\r\nHost: localhost\r', b'\n\r', b'\n']:
                sock.sendall(fragment)
                time.sleep(.01)
            self.assertIn(b'200 OK', sock.recv(4096))
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            sock.sendall(b'X' * 8192)
            self.assertIn(b'431 Request Header', sock.recv(4096))

    def test_concurrent_requests_with_stalled_client(self):
        with socket.create_connection(('127.0.0.1', self.port)) as slow:
            slow.sendall(b'GET /')
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                results = list(pool.map(lambda _: self.request('/style.css'), range(200)))
            self.assertTrue(all(result[0] == 200 for result in results))


    def raw_response(self, stream, head=False):
        status_line = stream.readline()
        self.assertTrue(status_line.startswith(b'HTTP/'), status_line)
        status = int(status_line.split()[1])
        headers = {}
        while True:
            line = stream.readline()
            if line == b'\r\n': break
            self.assertTrue(line, 'Premature close inside response headers')
            name, value = line.decode().split(':', 1)
            headers[name.lower()] = value.strip()
        body = b'' if head else stream.read(int(headers['content-length']))
        if not head: self.assertEqual(len(body), int(headers['content-length']))
        return status, headers, body

    def test_keep_alive_reuses_socket(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            with sock.makefile('rb') as stream:
                sock.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
                status, headers, body = self.raw_response(stream)
                self.assertEqual(headers['connection'], 'keep-alive')
                sock.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                self.assertEqual(self.raw_response(stream)[2], b'<h1>Hello</h1>')
                self.assertEqual(stream.read(1), b'')

    def test_pipeline_head_error_and_file(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            requests = [b'HEAD /large.bin', b'GET /missing', b'GET /style.css']
            sock.sendall(b''.join(r + b' HTTP/1.1\r\nHost: localhost\r\n' +
                        (b'Connection: close\r\n' if i == 2 else b'') + b'\r\n'
                        for i, r in enumerate(requests)))
            with sock.makefile('rb') as stream:
                self.assertEqual(self.raw_response(stream, head=True)[0], 200)
                self.assertEqual(self.raw_response(stream)[0], 404)
                self.assertEqual(self.raw_response(stream)[2], b'body { color: green; }')
                self.assertEqual(stream.read(1), b'')

    def test_framing_errors_close_without_processing_next_request(self):
        cases = [(b'', 400), (b'Host: a\r\nHost: b\r\n', 400),
                 (b'Host : a\r\n', 400), (b'Host: a b\r\n', 400),
                 (b'Host: a\r\nContent-Length: 1\r\n', 413),
                 (b'Host: a\r\nContent-Length: 0\r\nContent-Length: 0\r\n', 400),
                 (b'Host: a\r\nTransfer-Encoding: chunked\r\n', 501),
                 (b'Host: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n', 400),
                 (b'Host: a\r\nExpect: 100-continue\r\n', 417),
                 (b'Host: a\r\n bad: folded\r\n', 400),
                 (b'Host: a\r\nX-Test: bad\x00value\r\n', 400),
                 (b'Host: a\r\nConnection: close,\r\n', 400)]
        for headers, expected in cases:
            with self.subTest(headers=headers):
                with socket.create_connection(('127.0.0.1', self.port)) as sock:
                    sock.settimeout(8)
                    sock.sendall(b'GET / HTTP/1.1\r\n' + headers + b'\r\n' +
                                 b'GET /health HTTP/1.1\r\nHost: a\r\n\r\n')
                    with sock.makefile('rb') as stream:
                        status, h, body = self.raw_response(stream)
                        self.assertEqual(status, expected)
                        self.assertEqual(h['connection'], 'close')
                        self.assertEqual(stream.read(1), b'')

    def test_http10_and_connection_tokens(self):
        for headers, expected in [(b'', 'close'),
                                  (b'Connection: Keep-Alive\r\n', 'keep-alive'),
                                  (b'Connection: keep-alive, CLOSE\r\n', 'close')]:
            with socket.create_connection(('127.0.0.1', self.port)) as sock:
                sock.settimeout(8)
                sock.sendall(b'GET /health HTTP/1.0\r\n' + headers + b'\r\n')
                with sock.makefile('rb') as stream:
                    self.assertEqual(self.raw_response(stream)[1]['connection'], expected)

    def test_request_limit(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            with sock.makefile('rb') as stream:
                for i in range(100):
                    sock.sendall(b'GET /health HTTP/1.1\r\nHost: a\r\n\r\n')
                    status, headers, body = self.raw_response(stream)
                    self.assertEqual(headers['connection'], 'close' if i == 99 else 'keep-alive')
                self.assertEqual(stream.read(1), b'')

    def test_timeout_and_idle_expiry(self):
        with socket.create_connection(('127.0.0.1', self.port)) as partial, socket.create_connection(('127.0.0.1', self.port)) as idle:
            partial.settimeout(8); idle.settimeout(8)
            partial.sendall(b'GET /')
            idle.sendall(b'GET /health HTTP/1.1\r\nHost: a\r\n\r\n')
            with partial.makefile('rb') as part_stream, idle.makefile('rb') as idle_stream:
                self.assertEqual(self.raw_response(idle_stream)[0], 200)
                self.assertEqual(self.raw_response(part_stream)[0], 408)
                self.assertEqual(idle_stream.read(1), b'')

    def test_backpressure_and_disconnect(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
            sock.settimeout(8)
            sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            time.sleep(.1) # Allow the sender to encounter socket backpressure.
            self.assertEqual(self.request('/health')[0], 200)
            with sock.makefile('rb') as stream:
                self.assertEqual(hashlib.sha256(self.raw_response(stream)[2]).digest(),
                                 hashlib.sha256(self.large).digest())
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: a\r\n\r\n')
        self.assertEqual(self.request('/health')[0], 200)


    def test_hundreds_of_idle_clients_do_not_block_health(self):
        clients = []
        try:
            for _ in range(300):
                clients.append(socket.create_connection(('127.0.0.1', self.port), timeout=2))
            started = time.monotonic()
            self.assertEqual(self.request('/health')[0], 200)
            self.assertLess(time.monotonic() - started, 2)
        finally:
            for client in clients: client.close()

    def test_long_pipeline_continues_across_fsm_turns(self):
        with socket.create_connection(('127.0.0.1', self.port)) as sock:
            sock.settimeout(8)
            sock.sendall(b'GET /health HTTP/1.1\r\nHost: a\r\n\r\n' * 89 +
                         b'GET /health HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            with sock.makefile('rb') as stream:
                for _ in range(90):
                    self.assertEqual(self.raw_response(stream)[2], b'{"status":"ok"}\n')
                self.assertEqual(stream.read(1), b'')

    def test_descriptor_cleanup_after_connection_churn(self):
        fd_path = Path('/proc') / str(self.proc.pid) / 'fd'
        if not fd_path.is_dir():
            self.skipTest('Process descriptor table is unavailable in this PID namespace')
        time.sleep(.1)
        baseline = len(list(fd_path.iterdir()))
        for _ in range(150): self.assertEqual(self.request('/style.css')[0], 200)
        time.sleep(.2)
        self.assertLessEqual(len(list(fd_path.iterdir())), baseline)

    def test_connection_cap_and_recovery(self):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        proc = subprocess.Popen([str(PROJECT / 'http_server'), str(port), str(self.root), '2'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={**os.environ, 'C_HTTP_ACCESS_LOG': '0'})
        held = []
        try:
            assert proc.stdout is not None
            self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
            held = [socket.create_connection(('127.0.0.1', port), timeout=2) for _ in range(2)]
            time.sleep(.05)
            with socket.create_connection(('127.0.0.1', port), timeout=2) as extra:
                extra.settimeout(2)
                self.assertEqual(extra.recv(1), b'')
            held[0].close(); time.sleep(.05)
            c = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
            c.request('GET', '/health', headers={'Connection': 'close'})
            r = c.getresponse(); self.assertEqual(r.status, 200); r.read(); c.close()
            started = time.monotonic()
            proc.terminate(); out, errors = proc.communicate(timeout=2)
            self.assertLess(time.monotonic() - started, 2)
            self.assertNotIn(b'runtime error:', errors)
            self.assertNotIn(b'ERROR: AddressSanitizer', errors)
        finally:
            for client in held: client.close()
            if proc.poll() is None: proc.terminate(); proc.communicate(timeout=2)


    def test_etag_and_not_modified(self):
        status, headers, body = self.request('/style.css')
        etag = headers['ETag']
        self.assertTrue(etag.startswith('W/"'))
        self.assertIn('Last-Modified', headers)
        self.assertEqual(headers['Cache-Control'], 'no-cache')
        for condition in [etag, etag[2:], '*', '"different", ' + etag]:
            with self.subTest(condition=condition):
                status, cached, body = self.request('/style.css', headers={'If-None-Match': condition})
                self.assertEqual(status, 304); self.assertEqual(body, b'')
                self.assertEqual(cached['ETag'], etag)
        self.assertEqual(self.request('/style.css', headers={'If-None-Match':'"different"'})[0], 200)
        path = self.root / 'changing.txt'
        path.write_text('first')
        etag = self.request('/changing.txt')[1]['ETag']
        replacement = self.root / 'replacement.txt'; replacement.write_text('second version')
        replacement.replace(path)
        self.assertEqual(self.request('/changing.txt', headers={'If-None-Match':etag})[0], 200)
        path.unlink()

    def test_ranges(self):
        for value, expected, content_range in [('bytes=0-3', self.large[:4], f'bytes 0-3/{len(self.large)}'),
            ('bytes=-4', self.large[-4:], f'bytes {len(self.large)-4}-{len(self.large)-1}/{len(self.large)}'),
            (f'bytes={len(self.large)-4}-', self.large[-4:], f'bytes {len(self.large)-4}-{len(self.large)-1}/{len(self.large)}')]:
            with self.subTest(value=value):
                status, headers, body = self.request('/large.bin', headers={'Range':value})
                self.assertEqual(status,206); self.assertEqual(body,expected)
                self.assertEqual(headers['Content-Range'],content_range)
                self.assertEqual(int(headers['Content-Length']),len(expected))
        status, h, b = self.request('/style.css', headers={'Range':'bytes=0-999999'})
        self.assertEqual(status,206); self.assertEqual(b,b'body { color: green; }')
        for path, value in [('/large.bin',f'bytes={len(self.large)}-'),('/empty.txt','bytes=0-0'),('/style.css','bytes=-0')]:
            status,h,b=self.request(path,headers={'Range':value})
            self.assertEqual(status,416);self.assertTrue(h['Content-Range'].startswith('bytes */'))
        for value in ['bytes=4-2','bytes=x-y','items=0-1','bytes=0-1,3-4','bytes=999999999999999999999999999999-']:
            self.assertEqual(self.request('/style.css',headers={'Range':value})[0],200)

    def test_range_conditions_and_head(self):
        etag = self.request('/style.css')[1]['ETag']
        self.assertEqual(self.request('/style.css',headers={'Range':'bytes=0-2','If-None-Match':etag})[0],304)
        # Metadata ETags are weak, so If-Range conservatively returns full content.
        status,h,b=self.request('/style.css',headers={'Range':'bytes=0-2','If-Range':etag})
        self.assertEqual(status,200);self.assertEqual(b,b'body { color: green; }')
        status,h,b=self.request('/style.css','HEAD',headers={'Range':'bytes=0-2'})
        self.assertEqual(status,200);self.assertEqual(b,b'')
        self.assertEqual(int(h['Content-Length']),len(b'body { color: green; }'))

    def test_pipeline_cached_range_and_normal_response(self):
        etag = self.request('/style.css')[1]['ETag']
        with socket.create_connection(('127.0.0.1',self.port)) as sock:
            sock.settimeout(8)
            sock.sendall((f'GET /style.css HTTP/1.1\r\nHost: a\r\nIf-None-Match: {etag}\r\n\r\n'
                'GET /large.bin HTTP/1.1\r\nHost: a\r\nRange: bytes=0-2\r\n\r\n'
                'GET /health HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n').encode())
            with sock.makefile('rb') as stream:
                self.assertEqual(self.raw_response(stream,head=True)[0],304)
                self.assertEqual(self.raw_response(stream)[2],self.large[:3])
                status,h,b=self.raw_response(stream)
                self.assertEqual(status,200);self.assertNotIn('etag',h)
                self.assertEqual(b,b'{"status":"ok"}\n')

    def test_access_logging_json(self):
        import json
        log = self.base / 'access.jsonl'
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
        proc=subprocess.Popen([str(PROJECT/'http_server'),str(port),str(self.root)],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**os.environ,'C_HTTP_ACCESS_LOG':str(log)})
        try:
            assert proc.stdout is not None
            self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
            c.request('GET','/style.css',headers={'Connection':'close'})
            r=c.getresponse();r.read();c.close();time.sleep(.05)
        finally:proc.terminate();proc.communicate(timeout=2)
        records=[json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(records),1)
        self.assertEqual(records[0]['status'],200)
        self.assertEqual(records[0]['method'],'GET')
        self.assertEqual(records[0]['body_bytes_sent'],len(b'body { color: green; }'))
        self.assertTrue(records[0]['complete'])


    def launch_extra(self, env):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
        proc=subprocess.Popen([str(PROJECT/'http_server'),str(port),str(self.root)],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            env={**os.environ,'C_HTTP_ACCESS_LOG':'0',**env})
        assert proc.stdout is not None
        self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
        return proc,port

    def finish_extra(self, proc):
        proc.terminate();output,errors=proc.communicate(timeout=8)
        self.assertNotIn(b'runtime error:',errors)
        self.assertNotIn(b'ERROR: AddressSanitizer',errors)

    def test_delayed_disk_jobs_do_not_block_health(self):
        proc,port=self.launch_extra({'C_HTTP_TEST_IO_DELAY_MS':'300'})
        sockets=[]
        try:
            for _ in range(12):
                sock=socket.create_connection(('127.0.0.1',port));sockets.append(sock)
                sock.sendall(b'GET /style.css HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            time.sleep(.05)
            start=time.monotonic()
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
            c.request('GET','/health',headers={'Connection':'close'})
            r=c.getresponse();self.assertEqual(r.status,200);r.read();c.close()
            self.assertLess(time.monotonic()-start,.25)
        finally:
            for sock in sockets:sock.close()
            self.finish_extra(proc)

    def test_file_queue_saturation_retries_without_corruption(self):
        proc,port=self.launch_extra({'C_HTTP_TEST_IO_DELAY_MS':'10'})
        sockets=[]
        try:
            for _ in range(96):
                sock=socket.create_connection(('127.0.0.1',port));sock.settimeout(5);sockets.append(sock)
                sock.sendall(b'GET /style.css HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n')
            for sock in sockets:
                with sock.makefile('rb') as stream:
                    status,headers,body=self.raw_response(stream)
                    self.assertEqual(status,200);self.assertEqual(body,b'body { color: green; }')
        finally:
            for sock in sockets:sock.close()
            self.finish_extra(proc)

    def test_disconnect_during_file_open_and_read(self):
        proc,port=self.launch_extra({'C_HTTP_TEST_IO_DELAY_MS':'50'})
        try:
            for _ in range(40):
                sock=socket.create_connection(('127.0.0.1',port))
                sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: a\r\n\r\n');sock.close()
            time.sleep(.2)
            # Wait for file headers, then reset while the first async read is pending.
            import struct
            sock=socket.create_connection(('127.0.0.1',port));sock.settimeout(3)
            sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: a\r\n\r\n')
            self.assertIn(b'200 OK',sock.recv(2048))
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));sock.close()
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
            c.request('GET','/health',headers={'Connection':'close'});r=c.getresponse()
            self.assertEqual(r.status,200);r.read();c.close()
        finally:self.finish_extra(proc)

    def test_stalled_log_sink_does_not_block_health(self):
        fifo=self.base/'slow-logs.fifo';os.mkfifo(fifo)
        reader=os.open(fifo,os.O_RDONLY|os.O_NONBLOCK)
        proc,port=self.launch_extra({'C_HTTP_ACCESS_LOG':str(fifo)})
        try:
            def requests(count):
                c=http.client.HTTPConnection('127.0.0.1',port,timeout=4)
                try:
                    for _ in range(count):
                        c.request('GET','/health');r=c.getresponse()
                        self.assertEqual(r.status,200);r.read()
                finally:c.close()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(requests,[200]*8))
            started=time.monotonic();requests(1)
            self.assertLess(time.monotonic()-started,2)
            # Release backpressure, then verify drops are observable in JSON.
            data=b'';deadline=time.monotonic()+2
            while time.monotonic()<deadline:
                try:data+=os.read(reader,65536)
                except BlockingIOError:time.sleep(.005)
            requests(1);time.sleep(.05)
            while True:
                try:
                    chunk=os.read(reader,65536)
                    if not chunk:break
                    data+=chunk
                except BlockingIOError:break
            import json
            records=[json.loads(line) for line in data.splitlines()]
            self.assertTrue(any(record.get('dropped_logs',0)>0 for record in records))
        finally:
            self.finish_extra(proc);os.close(reader);fifo.unlink()


    def metric_values(self, body):
        values={}
        for line in body.decode().splitlines():
            if line and not line.startswith('#'):
                name,value=line.rsplit(' ',1);values[name]=float(value)
        return values

    def test_metrics_counts_and_head(self):
        status,h,body=self.request('/metrics')
        self.assertEqual(status,200)
        self.assertIn('version=0.0.4',h['Content-Type'])
        before=self.metric_values(body)
        self.assertGreaterEqual(before['c_http_active_connections'],1)
        self.assertEqual(before['c_http_connection_limit'],1024)
        self.assertEqual(before['c_http_file_job_limit'],64)
        self.assertEqual(before['c_http_log_queue_limit'],1024)
        self.request('/health');self.request('/missing')
        after=self.metric_values(self.request('/metrics')[2])
        self.assertGreaterEqual(after['c_http_responses_completed_total']-before['c_http_responses_completed_total'],3)
        self.assertGreaterEqual(after['c_http_responses_total{status_class="4xx"}']-before['c_http_responses_total{status_class="4xx"}'],1)
        self.assertGreater(after['c_http_body_bytes_sent_total'],before['c_http_body_bytes_sent_total'])
        self.assertGreater(after['c_http_file_jobs_submitted_total'],before['c_http_file_jobs_submitted_total'])
        self.assertEqual(after['c_http_response_duration_seconds_count'],after['c_http_responses_completed_total'])
        self.assertEqual(after['c_http_response_duration_seconds_bucket{le="+Inf"}'],after['c_http_responses_completed_total'])
        status,h,body=self.request('/metrics','HEAD');self.assertEqual(status,200);self.assertEqual(body,b'')
        self.assertGreater(int(h['Content-Length']),0)

    def test_metrics_live_file_queue(self):
        proc,port=self.launch_extra({'C_HTTP_TEST_IO_DELAY_MS':'200'})
        clients=[]
        try:
            for _ in range(8):
                s=socket.create_connection(('127.0.0.1',port));clients.append(s)
                s.sendall(b'GET /style.css HTTP/1.1\r\nHost: a\r\n\r\n')
            time.sleep(.05)
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=2);c.request('GET','/metrics')
            r=c.getresponse();values=self.metric_values(r.read());c.close()
            self.assertGreater(values['c_http_file_jobs_running'],0)
            self.assertGreater(values['c_http_file_jobs_queued'],0)
            self.assertLessEqual(values['c_http_file_jobs_outstanding'],64)
        finally:
            for client in clients:client.close()
            self.finish_extra(proc)

    def test_graceful_shutdown_finishes_active_response(self):
        proc,port=self.launch_extra({'C_HTTP_TEST_IO_DELAY_MS':'150'})
        sock=socket.create_connection(('127.0.0.1',port));sock.settimeout(4)
        try:
            # Second pipelined request must not be started during drain.
            sock.sendall(b'GET /style.css HTTP/1.1\r\nHost: a\r\n\r\n'
                         b'GET /health HTTP/1.1\r\nHost: a\r\n\r\n')
            time.sleep(.05);proc.terminate();time.sleep(.12)
            with self.assertRaises(OSError):socket.create_connection(('127.0.0.1',port),timeout=.2)
            with sock.makefile('rb') as stream:
                status,h,body=self.raw_response(stream)
                self.assertEqual(status,200);self.assertEqual(body,b'body { color: green; }')
                self.assertEqual(h['connection'],'close');self.assertEqual(stream.read(1),b'')
            output,errors=proc.communicate(timeout=3)
            self.assertEqual(proc.returncode,0);self.assertNotIn(b'ERROR: AddressSanitizer',errors)
        finally:
            sock.close()
            if proc.poll() is None:self.finish_extra(proc)

    def test_graceful_shutdown_closes_idle_and_partial_clients(self):
        proc,port=self.launch_extra({});clients=[]
        try:
            for partial in [False,True]:
                s=socket.create_connection(('127.0.0.1',port));s.settimeout(2);clients.append(s)
                if partial:s.sendall(b'GET /')
            start=time.monotonic();proc.terminate();proc.communicate(timeout=2)
            self.assertLess(time.monotonic()-start,1)
            for client in clients:
                try:self.assertEqual(client.recv(1),b'')
                except ConnectionResetError:pass
        finally:
            for client in clients:client.close()
            if proc.poll() is None:self.finish_extra(proc)

    def test_graceful_shutdown_deadline_and_second_signal(self):
        for force in [False,True]:
            with self.subTest(force=force):
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
                proc=subprocess.Popen([str(PROJECT/'http_server'),str(port),str(self.root),'1024','300'],
                    stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**os.environ,'C_HTTP_ACCESS_LOG':'0'})
                assert proc.stdout is not None
                self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
                sock=socket.create_connection(('127.0.0.1',port));sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
                try:
                    sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: a\r\n\r\n')
                    time.sleep(.1);started=time.monotonic();proc.terminate()
                    if force:time.sleep(.05);proc.terminate()
                    proc.communicate(timeout=3)
                    elapsed=time.monotonic()-started
                    self.assertLess(elapsed,2)
                    if not force:self.assertGreater(elapsed,.2)
                    self.assertEqual(proc.returncode,0)
                finally:
                    sock.close()
                    if proc.poll() is None:self.finish_extra(proc)


    def launch_config(self, settings):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        path=self.base/f'config-{port}.conf'
        base={'port':port,'document_root':str(self.root),'access_log':'0'};base.update(settings)
        path.write_text(''.join(f'{key} = {value}\n' for key,value in base.items()))
        env={k:v for k,v in os.environ.items() if k!='C_HTTP_ACCESS_LOG'}
        proc=subprocess.Popen([str(PROJECT/'http_server'),'--config',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        assert proc.stdout is not None
        line=proc.stdout.readline();self.assertTrue(line.startswith(b'Listening'),line)
        return proc,port

    def request_extra(self,port,path,headers=None,context=None):
        c=http.client.HTTPSConnection('localhost',port,context=context,timeout=5) if context else http.client.HTTPConnection('127.0.0.1',port,timeout=5)
        try:
            c.request('GET',path,headers=headers or {});r=c.getresponse();return r.status,dict(r.getheaders()),r.read()
        finally:c.close()

    def test_configuration_validation_and_limits(self):
        proc,port=self.launch_config({'max_connections':12,'file_workers':2,'file_job_limit':8,'log_queue_limit':16,'max_requests':2})
        try:
            values=self.metric_values(self.request_extra(port,'/metrics')[2])
            self.assertEqual(values['c_http_connection_limit'],12)
            self.assertEqual(values['c_http_file_job_limit'],8)
            self.assertEqual(values['c_http_log_queue_limit'],16)
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=3)
            for i in range(2):
                c.request('GET','/health');r=c.getresponse();r.read()
                self.assertEqual(r.getheader('Connection'),'close' if i==1 else 'keep-alive')
            c.close()
        finally:self.finish_extra(proc)
        for contents in ['unknown=1','port=0','file_workers=17','port=8080\nport=8081','tls_cert=x','cache_bytes=-1']:
            path=self.base/'invalid.conf';path.write_text(contents+'\n')
            result=subprocess.run([str(PROJECT/'http_server'),'--config',str(path)],capture_output=True,timeout=3)
            self.assertNotEqual(result.returncode,0)
            self.assertIn(b'configuration',result.stderr)

    def test_cache_hits_ranges_and_ttl_invalidation(self):
        path=self.root/'cached.txt';path.write_bytes(b'old version')
        proc,port=self.launch_config({'cache_bytes':65536,'cache_max_file_bytes':65536,'cache_entries':4,'cache_ttl_ms':100})
        try:
            status,h,body=self.request_extra(port,'/cached.txt');self.assertEqual(body,b'old version');tag=h['ETag']
            status,h,body=self.request_extra(port,'/cached.txt',{'Range':'bytes=0-2'})
            self.assertEqual(status,206);self.assertEqual(body,b'old')
            self.assertEqual(self.request_extra(port,'/cached.txt',{'If-None-Match':tag})[0],304)
            values=self.metric_values(self.request_extra(port,'/metrics')[2])
            self.assertGreaterEqual(values['c_http_cache_hits_total'],2)
            self.assertLessEqual(values['c_http_cache_bytes'],65536)
            replacement=self.root/'replacement.txt';replacement.write_bytes(b'new version');replacement.replace(path)
            time.sleep(.15)
            status,h,body=self.request_extra(port,'/cached.txt',{'If-None-Match':tag})
            self.assertEqual(status,200);self.assertEqual(body,b'new version');self.assertNotEqual(h['ETag'],tag)
            self.assertGreaterEqual(self.metric_values(self.request_extra(port,'/metrics')[2])['c_http_cache_expirations_total'],1)
        finally:self.finish_extra(proc);path.unlink()

    def test_cache_eviction_and_large_file_bypass(self):
        proc,port=self.launch_config({'cache_bytes':16384,'cache_max_file_bytes':1024,'cache_entries':1,'cache_ttl_ms':60000})
        try:
            self.request_extra(port,'/style.css');self.request_extra(port,'/index.html')
            self.assertEqual(self.request_extra(port,'/style.css')[2],b'body { color: green; }')
            values=self.metric_values(self.request_extra(port,'/metrics')[2])
            self.assertGreaterEqual(values['c_http_cache_evictions_total'],2)
            self.assertLessEqual(values['c_http_cache_entries'],1)
            self.assertLessEqual(values['c_http_cache_bytes'],16384)
            self.assertEqual(hashlib.sha256(self.request_extra(port,'/large.bin')[2]).digest(),hashlib.sha256(self.large).digest())
        finally:self.finish_extra(proc)

    def test_cached_large_body_and_range_across_write_chunks(self):
        content=bytes(range(256))*1024
        path=self.root/'cached-large.bin';path.write_bytes(content)
        proc,port=self.launch_config({'cache_bytes':1048576,'cache_max_file_bytes':1048576,
                                      'cache_entries':4,'cache_ttl_ms':60000})
        try:
            self.assertEqual(self.request_extra(port,'/cached-large.bin')[2],content)
            start,end=31000,180000
            status,headers,body=self.request_extra(port,'/cached-large.bin',
                {'Range':f'bytes={start}-{end}'})
            self.assertEqual(status,206)
            self.assertEqual(headers['Content-Range'],f'bytes {start}-{end}/{len(content)}')
            self.assertEqual(body,content[start:end+1])
            self.assertEqual(self.request_extra(port,'/cached-large.bin')[2],content)
            values=self.metric_values(self.request_extra(port,'/metrics')[2])
            self.assertGreaterEqual(values['c_http_cache_hits_total'],2)
            self.assertLessEqual(values['c_http_cache_bytes'],1048576)
        finally:self.finish_extra(proc);path.unlink()

    def test_uncached_plain_file_uses_sendfile_for_ranges_and_full_body(self):
        proc,port=self.launch_config({'cache_bytes':0})
        try:
            before=self.metric_values(self.request_extra(port,'/metrics')[2])
            start,end=32761,163900
            status,headers,body=self.request_extra(port,'/large.bin',
                {'Range':f'bytes={start}-{end}'})
            self.assertEqual(status,206)
            self.assertEqual(headers['Content-Range'],f'bytes {start}-{end}/{len(self.large)}')
            self.assertEqual(body,self.large[start:end+1])
            after_range=self.metric_values(self.request_extra(port,'/metrics')[2])
            # Only the file-open job is needed; body reads bypass the disk pool.
            self.assertEqual(after_range['c_http_file_jobs_submitted_total']-
                             before['c_http_file_jobs_submitted_total'],1)
            self.assertEqual(self.request_extra(port,'/large.bin')[2],self.large)
            after_full=self.metric_values(self.request_extra(port,'/metrics')[2])
            self.assertEqual(after_full['c_http_file_jobs_submitted_total']-
                             after_range['c_http_file_jobs_submitted_total'],1)
        finally:self.finish_extra(proc)

    def tls_files(self):
        if os.environ.get('C_HTTP_TEST_TLS')=='0':self.skipTest('HTTP-only build')
        if not shutil.which('openssl'):self.skipTest('OpenSSL CLI unavailable for temporary test certificates')
        cert=self.base/'test-cert.pem';key=self.base/'test-key.pem'
        if not cert.exists():
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                '-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost',
                '-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,capture_output=True)
        context=ssl.create_default_context(cafile=str(cert));context.set_alpn_protocols(['http/1.1'])
        return cert,key,context

    def test_https_verified_certificate_keep_alive_ranges(self):
        cert,key,context=self.tls_files()
        proc,port=self.launch_config({'tls_cert':str(cert),'tls_key':str(key),'cache_bytes':65536})
        try:
            c=http.client.HTTPSConnection('localhost',port,context=context,timeout=5)
            try:
                c.request('GET','/health')
                r=c.getresponse()
                self.assertEqual(r.status,200)
                r.read()
                assert c.sock is not None
                self.assertEqual(c.sock.selected_alpn_protocol(),'http/1.1')

                c.request('GET','/style.css')
                r=c.getresponse()
                tag=r.getheader('ETag')
                assert tag is not None
                self.assertEqual(r.read(),b'body { color: green; }')

                c.request('GET','/style.css',headers={'Range':'bytes=0-3'})
                r=c.getresponse()
                self.assertEqual(r.status,206)
                self.assertEqual(r.read(),b'body')

                c.request('GET','/style.css',headers={'If-None-Match':tag,'Connection':'close'})
                r=c.getresponse()
                self.assertEqual(r.status,304)
                self.assertEqual(r.read(),b'')
            finally:
                c.close()
            values=self.metric_values(self.request_extra(port,'/metrics',context=context)[2])
            self.assertEqual(values['c_http_tls_enabled'],1);self.assertGreaterEqual(values['c_http_tls_handshakes_total'],2)
        finally:self.finish_extra(proc)

    def test_https_pipeline_large_file_and_disconnect(self):
        cert,key,context=self.tls_files();proc,port=self.launch_config({'tls_cert':str(cert),'tls_key':str(key)})
        try:
            with socket.create_connection(('127.0.0.1',port)) as plain:
                with context.wrap_socket(plain,server_hostname='localhost') as sock:
                    sock.settimeout(5)
                    sock.sendall(b'GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n'
                                 b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                    time.sleep(.1)
                    with sock.makefile('rb') as stream:
                        self.assertEqual(hashlib.sha256(self.raw_response(stream)[2]).digest(),hashlib.sha256(self.large).digest())
                        self.assertEqual(self.raw_response(stream)[2],b'{"status":"ok"}\n')
                        self.assertEqual(stream.read(1),b'')
            # An incomplete TLS handshake must not stall another client.
            with socket.create_connection(('127.0.0.1',port)):
                self.assertEqual(self.request_extra(port,'/health',context=context)[0],200)
            self.assertEqual(self.request_extra(port,'/health',context=context)[0],200)
            values=self.metric_values(self.request_extra(port,'/metrics',context=context)[2])
            self.assertGreater(values['c_http_file_jobs_submitted_total'],1)
        finally:self.finish_extra(proc)

    def test_https_graceful_shutdown_and_certificate_rejection(self):
        cert,key,context=self.tls_files()
        proc,port=self.launch_config({'tls_cert':str(cert),'tls_key':str(key),'access_log':'0'})
        try:
            wrong=ssl.create_default_context()
            with self.assertRaises(ssl.SSLError):self.request_extra(port,'/health',context=wrong)
            self.assertEqual(self.request_extra(port,'/health',context=context)[0],200)
        finally:self.finish_extra(proc)
        # Delay file preparation so shutdown begins before headers are sent.
        env_before=os.environ.get('C_HTTP_TEST_IO_DELAY_MS');os.environ['C_HTTP_TEST_IO_DELAY_MS']='100'
        try:proc,port=self.launch_config({'tls_cert':str(cert),'tls_key':str(key)})
        finally:
            if env_before is None:os.environ.pop('C_HTTP_TEST_IO_DELAY_MS',None)
            else:os.environ['C_HTTP_TEST_IO_DELAY_MS']=env_before
        plain=socket.create_connection(('127.0.0.1',port));sock=context.wrap_socket(plain,server_hostname='localhost');sock.settimeout(4)
        try:
            sock.sendall(b'GET /style.css HTTP/1.1\r\nHost: localhost\r\n\r\n');time.sleep(.03);proc.terminate()
            with sock.makefile('rb') as stream:
                status,h,body=self.raw_response(stream)
                self.assertEqual(status,200);self.assertEqual(body,b'body { color: green; }');self.assertEqual(stream.read(1),b'')
            proc.communicate(timeout=3);self.assertEqual(proc.returncode,0)
        finally:
            sock.close()
            if proc.poll() is None:self.finish_extra(proc)

if __name__ == '__main__':
    unittest.main(verbosity=2)
