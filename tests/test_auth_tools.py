"""Credential generation, independent signing fixtures, and benchmark metadata checks."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
PROJECT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PROJECT/'tools'))
from auth_client import auth_headers, load_credential
from benchmark import workload

class AuthToolTests(unittest.TestCase):
    def test_generation_private_files_and_refusal_to_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            for kind in ['token','hmac']:
                client=Path(tmp)/(kind+'.auth-client');server=Path(tmp)/(kind+'.credentials')
                command=[sys.executable,str(PROJECT/'tools/create_credentials.py'),'--kind',kind,'--id',kind+'-a',
                         '--client-file',str(client),'--server-file',str(server)]
                result=subprocess.run(command,capture_output=True,check=True)
                credential=load_credential(client)
                self.assertEqual(client.stat().st_mode&0o777,0o600)
                self.assertEqual(server.stat().st_mode&0o777,0o600)
                self.assertNotIn(credential['secret'].encode(),result.stdout+result.stderr)
                expected=hashlib.sha256(bytes.fromhex(credential['secret'])).hexdigest() if kind=='token' else credential['secret']
                self.assertEqual(server.read_text(),f'{kind} {kind}-a {expected}\n')
                original=client.read_bytes()
                result=subprocess.run(command,capture_output=True)
                self.assertNotEqual(result.returncode,0);self.assertEqual(client.read_bytes(),original)
                client.chmod(0o644)
                with self.assertRaises(ValueError):load_credential(client)
            # Failure to create the second file rolls back only the newly created first file.
            client=Path(tmp)/'rollback.auth-client'
            result=subprocess.run([sys.executable,str(PROJECT/'tools/create_credentials.py'),'--kind','token','--id','a',
                    '--client-file',str(client),'--server-file',str(server)],capture_output=True)
            self.assertNotEqual(result.returncode,0);self.assertFalse(client.exists())

    def test_independent_signature_fixture_and_fresh_nonces(self):
        credential={'kind':'hmac','id':'sign-a','secret':bytes(range(32)).hex()}
        headers=auth_headers(credential,'POST','/api/private/echo?x=1',b'{}','application/json',timestamp=1000,nonce='0'*32)
        self.assertEqual(headers['X-Auth-Signature'],'66cf805d1ffa3b0e0b667a201c06c636f8ea4642291d5d72c9da862afc4172bf')
        a=auth_headers(credential,'POST','/api/private/echo',b'{}','application/json')
        b=auth_headers(credential,'POST','/api/private/echo',b'{}','application/json')
        self.assertNotEqual(a['X-Auth-Nonce'],b['X-Auth-Nonce'])
        with self.assertRaises(ValueError):auth_headers(credential,'POST','/bad\npath',b'{}','application/json')

    def test_benchmark_workload_compatibility(self):
        old={'scheme':'http','path':'/health'}
        new={**old,'method':'GET','authentication':None,'body_sha256':None,'body_length':0}
        self.assertEqual(workload(old),workload(new))
        self.assertNotEqual(workload(new),workload({**new,'authentication':'token'}))
        self.assertNotEqual(workload(new),workload({**new,'method':'POST','body_sha256':'digest','body_length':2}))

if __name__=='__main__':unittest.main()
