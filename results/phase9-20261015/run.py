"""Reproduce the local Phase 9 comparison; run after an optimized TLS=1 build."""
import hashlib
import json
import os
from pathlib import Path
import platform
import ssl
import subprocess
import sys

PROJECT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(PROJECT/'tests'),str(PROJECT/'tools')]
from test_upstream import UpstreamTLSTests, PersistentBackend, PersistentHandler
from benchmark import run, summary

class BenchmarkFixture(UpstreamTLSTests):
    backend_type=PersistentBackend
    handler_type=PersistentHandler

def main():
    output=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent
    output.mkdir(parents=True,exist_ok=True)
    environment={'platform':platform.platform(),'python':platform.python_version(),
                 'openssl':ssl.OPENSSL_VERSION,'cpu_count':os.cpu_count(),
                 'binary_sha256':hashlib.sha256((PROJECT/'http_server').read_bytes()).hexdigest(),
                 'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0],
                 'build':'make CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -O2"'}
    fixture=BenchmarkFixture()
    fixture.setUpClass()
    fixture.procs=[]
    results={}
    try:
        for protocol in ['http','https','mtls']:
            backend=fixture.backend_port if protocol=='http' else fixture.tls_backend(mtls=protocol=='mtls')
            for pooling in [0,1]:
                name=protocol+('-pool' if pooling else '-fresh');trials=[]
                for trial in range(3):
                    settings={'upstream_pool_enabled':pooling,'max_requests':10000,
                              'upstream_response_timeout_ms':30000}
                    port=fixture.launch(**settings)[1] if protocol=='http' else fixture.tls_proxy(backend,'client' if protocol=='mtls' else None,**settings)
                    proc=fixture.procs[-1]
                    result=run('127.0.0.1',port,'/proxy/id',16,1500,20,True)
                    metrics=fixture.request('/metrics',port=port)[2].decode()
                    result['upstream_metrics']={line.split()[0]:float(line.split()[1]) for line in metrics.splitlines() if line.startswith('c_http_upstream_')}
                    result['process_snapshot']={'fds':len(list(Path(f'/proc/{proc.pid}/fd').iterdir())),
                                                'rss_kib':int(next(line for line in Path(f'/proc/{proc.pid}/status').read_text().splitlines() if line.startswith('VmRSS:')).split()[1])}
                    trials.append(result)
                    proc.terminate();_,errors=proc.communicate(timeout=5)
                    if proc.returncode or errors:raise RuntimeError(errors.decode())
                    if result['errors'] or result['warmup_errors']:raise RuntimeError(result)
                    print(name,trial+1,result['successful_requests_per_s'],flush=True)
                payload={'environment':environment,'protocol':protocol,'pool_enabled':pooling,
                         'configuration':{'cache_bytes':0,'access_log':'0','max_requests':10000,
                                          'upstream_pool_max_connections':64,'upstream_pool_idle_limit':16,
                                          'upstream_pool_max_requests':1000,'upstream_response_timeout_ms':30000},
                         'trials':trials,'summary':summary(trials)}
                (output/(name+'.json')).write_text(json.dumps(payload,indent=2)+'\n')
                results[name]=payload['summary']
        (output/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
    finally:
        fixture.tearDown();fixture.doCleanups();fixture.tearDownClass()

if __name__=='__main__':main()
