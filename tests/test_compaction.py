"""End-to-end log compaction tests; no dependencies beyond Python's standard library.

Run: python3 tests/test_compaction.py ./kvserver_multi
Each test uses a fresh temporary log file and an OS-selected free port.
"""
import os
import pathlib
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())


class Server:
    def __init__(self, *args, delay_ms=None):
        env = dict(os.environ)
        if delay_ms is not None:
            env['KV_TEST_COMPACT_DELAY_MS'] = str(delay_ms)
        self.process = subprocess.Popen([binary, '0', *map(str, args)], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        bufsize=0)  # So select() sees every line.
        while True:
            line = self.read_line()
            match = re.match(r'Listening on 127\.0\.0\.1:(\d+) ', line)
            if match:
                self.port = int(match.group(1))
                return

    def read_line(self, timeout=5):
        assert select.select([self.process.stdout], [], [], timeout)[0], 'No output'
        line = self.process.stdout.readline().decode()
        assert line, ('Server exited', self.process.wait(), self.process.stderr.read())
        return line

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def exchange(self, commands, expected):
        with self.connect() as client:
            client.sendall(commands)
            client.shutdown(socket.SHUT_WR)
            chunks = []
            while chunk := client.recv(65536):
                chunks.append(chunk)
        actual = b''.join(chunks)
        assert actual == expected, (actual[:300], expected[:300])

    def wait_compacted(self):
        line = self.read_line()
        assert line.startswith('Compacted '), line
        return line

    def crash(self):
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()
        errors = self.process.stderr.read()
        assert not errors, errors


def records(aof):
    """Log lines as a sorted list: a snapshot's order is not defined."""
    data = aof.read_bytes()
    assert data.endswith(b'\n') or not data
    return sorted(data.splitlines())


with tempfile.TemporaryDirectory() as directory:
    aof = pathlib.Path(directory) / 'data.aof'

    # Without a log there is nothing to compact.
    server = Server()
    server.exchange(b'COMPACT\n', b'ERROR persistence disabled\n')
    server.crash()

    # History collapses to one SET per live key; deleted keys disappear.
    server = Server('--aof', aof)
    history = b''.join(b'SET counter %d\n' % n for n in range(500))
    server.exchange(history + b'SET name Arnav\nSET gone x\nDEL gone\nSET spaced  a b \n',
                    b'OK\n' * 502 + b'DELETED\nOK\n')
    before = aof.stat().st_size
    server.exchange(b'COMPACT\n', b'COMPACTING\n')
    assert f'{before} -> ' in server.wait_compacted()
    assert records(aof) == [b'SET counter 499', b'SET name Arnav', b'SET spaced  a b ']
    assert not (pathlib.Path(directory) / 'data.aof.rewrite').exists()
    server.exchange(b'SET after compact\n', b'OK\n')  # Appends go to the new file.
    server.crash()
    server = Server('--aof', aof)
    server.exchange(b'GET counter\nGET name\nGET gone\nGET spaced\nGET after\n',
                    b'VALUE 499\nVALUE Arnav\nNOT_FOUND\nVALUE  a b \nVALUE compact\n')
    server.crash()
    print('PASS: COMPACT rewrites history as current state and survives a crash')

    # The server keeps serving while the child works. Changes made meanwhile
    # are not in the child's snapshot; the parent must carry them over.
    aof.write_bytes(b'SET keep 1\nSET old 1\nSET change 1\n')
    server = Server('--aof', aof, delay_ms=700)
    server.exchange(b'COMPACT\n', b'COMPACTING\n')
    started = time.monotonic()
    server.exchange(b'COMPACT\nSET new 2\nDEL old\nSET change 2\nGET keep\n',
                    b'ERROR compaction already running\nOK\nDELETED\nOK\nVALUE 1\n')
    with server.connect() as leaving:  # The child must not hold client sockets.
        leaving.sendall(b'EXIT\n')
        assert leaving.recv(100) == b'BYE\n'
        assert leaving.recv(100) == b''
    assert time.monotonic() - started < 0.5, 'Server was blocked by compaction'
    server.wait_compacted()
    assert records(aof) == [b'DEL old', b'SET change 1', b'SET change 2',
                            b'SET keep 1', b'SET new 2', b'SET old 1'], records(aof)
    server.crash()
    server = Server('--aof', aof)
    server.exchange(b'GET keep\nGET old\nGET change\nGET new\n',
                    b'VALUE 1\nNOT_FOUND\nVALUE 2\nVALUE 2\n')
    server.crash()
    print('PASS: clients are served during compaction; concurrent changes are kept')

    # A crash mid-compaction leaves the old log intact and a stray temp file,
    # which the next start removes.
    aof.write_bytes(b'SET survivor yes\n')
    server = Server('--aof', aof, delay_ms=2000)
    server.exchange(b'COMPACT\nSET during crash\n', b'COMPACTING\nOK\n')
    server.crash()  # The orphaned child finishes later; its file is ignored.
    temp = pathlib.Path(directory) / 'data.aof.rewrite'
    assert temp.exists()
    server = Server('--aof', aof)
    assert not temp.exists()
    server.exchange(b'GET survivor\nGET during\n', b'VALUE yes\nVALUE crash\n')
    server.crash()
    print('PASS: crash during compaction keeps the old log; leftover temp removed')

    # Automatic compaction: once the log doubles past --compact-min it is rewritten,
    # so endlessly overwriting one key keeps the file small.
    aof.unlink()
    server = Server('--aof', aof, '--compact-min', 1000)
    for n in range(20):
        server.exchange(b''.join(b'SET hot %d\n' % (n * 100 + i) for i in range(100)),
                        b'OK\n' * 100)
    assert server.wait_compacted()
    time.sleep(0.3)  # Let any compaction triggered by the final batch finish.
    assert aof.stat().st_size < 3000, aof.stat().st_size  # 20 batches wrote ~22 KB.
    server.crash()
    server = Server('--aof', aof)
    server.exchange(b'GET hot\n', b'VALUE 1999\n')
    server.crash()
    bad = subprocess.run([binary, '--compact-min', 'lots'], capture_output=True, timeout=5)
    assert bad.returncode != 0 and b'Usage' in bad.stderr
    print('PASS: automatic compaction keeps a hot key\'s log bounded')
