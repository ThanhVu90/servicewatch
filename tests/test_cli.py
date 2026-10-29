"""Real loopback sockets; no internet endpoint or DNS dependency."""
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())

@contextlib.contextmanager
def listener(family=socket.AF_INET):
    host = '::1' if family == socket.AF_INET6 else '127.0.0.1'
    with socket.socket(family, socket.SOCK_STREAM) as server:
        server.bind((host, 0))
        server.listen(128)
        server.settimeout(0.05)
        stopped = threading.Event()
        def accept():
            while not stopped.is_set():
                try:
                    conn, _ = server.accept()
                    conn.close()
                except socket.timeout:
                    pass
        worker = threading.Thread(target=accept)
        worker.start()
        try:
            yield host, server.getsockname()[1]
        finally:
            stopped.set()
            worker.join(timeout=2)
            assert not worker.is_alive()

class CLI(unittest.TestCase):
    def run_cli(self, content, *args):
        with tempfile.TemporaryDirectory() as d:
            config = Path(d) / 'targets.csv'
            config.write_bytes(content.encode() if isinstance(content, str) else content)
            return subprocess.run([EXE, '--config', str(config), '--timeout-ms', '100', '--interval-ms', '0', *args],capture_output=True, timeout=15)

    def rows(self, result):
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_connected(self):
        with listener() as (host, port):
            r=self.run_cli(f'web,{host},{port}')
            self.assertEqual(r.returncode,0,r.stderr)
            row=self.rows(r)[0]
            self.assertEqual((row['state'],row['previous_state'],row['changed'],row['reachable']),('UP','UNKNOWN',True,True))
            self.assertEqual(row['reason'],'connected')
            self.assertGreaterEqual(row['elapsed_ms'],0)

    def test_refused_and_failure_threshold(self):
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1',0))  # Bound but not listening: cannot be stolen by another process.
            r=self.run_cli(f'down,127.0.0.1,{reserved.getsockname()[1]}','--rounds','3')
            self.assertEqual(r.returncode,1,r.stderr)
            rows=self.rows(r)
            self.assertEqual([x['state'] for x in rows],['UNKNOWN','DOWN','DOWN'])
            self.assertEqual([x['changed'] for x in rows],[False,True,False])
            self.assertTrue(all(not x['reachable'] for x in rows))

    def test_initial_unknown_is_not_success(self):
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1',0))
            r=self.run_cli(f'x,127.0.0.1,{reserved.getsockname()[1]}')
            self.assertEqual(r.returncode,1)
            self.assertEqual(self.rows(r)[0]['state'],'UNKNOWN')

    def test_recovery_threshold(self):
        with listener() as (host,port):
            r=self.run_cli(f'x,{host},{port}','--rounds','3','--recovery-threshold','2')
            self.assertEqual(r.returncode,0)
            self.assertEqual([x['state'] for x in self.rows(r)],['UNKNOWN','UP','UP'])
            self.assertEqual([x['consecutive_successes'] for x in self.rows(r)],[1,2,2])

    def test_parallel_order_and_rounds(self):
        with listener() as (host,port):
            config='\n'.join(f'service-{i},{host},{port}' for i in range(16))
            r=self.run_cli(config,'--workers','4','--rounds','4')
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual([(x['round'],x['name']) for x in self.rows(r)],[(k,f'service-{i}') for k in range(1,5) for i in range(16)])

    def test_ipv6(self):
        try:
            with listener(socket.AF_INET6) as (host,port):
                r=self.run_cli(f'v6,{host},{port}')
                self.assertEqual(r.returncode,0,r.stderr)
        except OSError as error:
            self.skipTest(f'IPv6 loopback unavailable: {error}')

    def test_config_rejections(self):
        cases=['','x,localhost,80','x,127.0.0.1,0','x,127.0.0.1,65536','x,127.0.0.1,+80','x,127.0.0.1,80,x','x,127.0.0.1,80\nx,::1,80','bad"name,127.0.0.1,80','x,fe80::1%eth0,80',b'x,127.0.0.1,80\0',b'x,127.0.0.1,80\x1b',b'x,127.0.0.1,80\xff','#'*257,'\n'*65537,'\n'.join(f'x{i},127.0.0.1,80' for i in range(65))]
        for content in cases:
            with self.subTest(content=repr(content[:40])):
                r=self.run_cli(content)
                self.assertEqual(r.returncode,2,r.stderr)
                self.assertEqual(r.stdout,b'')

    def test_comments_crlf_whitespace(self):
        with listener() as (host,port):
            r=self.run_cli(f'#comment\r\n\r\n x , {host} , {port}\r\n')
            self.assertEqual(r.returncode,0,r.stderr)

    def test_invalid_options(self):
        for args in [[],['--config'],['--workers','0'],['--rounds','101'],['--timeout-ms','30001'],['--workers','17'],['--interval-ms','-1'],['--failure-threshold','0'],['--recovery-threshold','11'],['--rounds','999999999999999'],['--wat','1'],['--rounds','1','--rounds','2']]:
            with self.subTest(args=args):
                r=subprocess.run([EXE,*args],capture_output=True,timeout=5)
                self.assertEqual(r.returncode,2)
                self.assertEqual(r.stdout,b'')

    def test_missing_file(self):
        with tempfile.TemporaryDirectory() as d:
            r=subprocess.run([EXE,'--config',str(Path(d)/'absent')],capture_output=True,timeout=5)
            self.assertEqual(r.returncode,3)

    def test_help_version(self):
        for flag in ['--help','--version']:
            r=subprocess.run([EXE,flag],capture_output=True,timeout=5)
            self.assertEqual(r.returncode,0)
            self.assertIn(b'ServiceWatch',r.stdout)

    @unittest.skipUnless(os.path.exists('/dev/full'),'POSIX only')
    def test_output_failure(self):
        with listener() as (host,port), tempfile.TemporaryDirectory() as d, open('/dev/full','wb') as out:
            p=Path(d)/'targets.csv';p.write_text(f'x,{host},{port}')
            r=subprocess.run([EXE,'--config',str(p)],stdout=out,stderr=subprocess.PIPE,timeout=5)
            self.assertEqual(r.returncode,3)

if __name__=='__main__':unittest.main(verbosity=2)
