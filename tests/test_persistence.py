"""End-to-end persistence tests; no dependencies beyond Python's standard library.

Run: python3 tests/test_persistence.py ./kvserver_multi
Each test uses a fresh temporary log file and an OS-selected free port.
"""
import pathlib
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


class Server:
    def __init__(self, aof, *extra):
        self.process = subprocess.Popen([binary, '0', '--aof', str(aof), *extra],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        bufsize=0)  # So select() sees every line.
        self.startup = []
        while True:
            assert select.select([self.process.stdout], [], [], 5)[0], 'Startup timed out'
            line = self.process.stdout.readline()
            assert line, ('Server exited', self.process.wait(), self.process.stderr.read())
            match = re.match(rb'Listening on 127\.0\.0\.1:(\d+) ', line)
            if match:
                self.port = int(match.group(1))
                return
            self.startup.append(line.decode())

    def exchange(self, commands, expected):
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as client:
            client.sendall(commands)
            client.shutdown(socket.SHUT_WR)
            chunks = []
            while chunk := client.recv(65536):
                chunks.append(chunk)
        actual = b''.join(chunks)
        assert actual == expected, (actual, expected)

    def crash(self):
        # SIGKILL: no destructors, no cleanup. Like a crash or `kill -9`.
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()
        errors = self.process.stderr.read()
        assert not errors, errors


def fails_to_start(aof):
    result = subprocess.run([binary, '0', '--aof', str(aof)], capture_output=True, timeout=5)
    assert result.returncode != 0, 'Server started with a corrupt log'
    return result.stderr.decode()


with tempfile.TemporaryDirectory() as directory:
    aof = pathlib.Path(directory) / 'data.aof'

    # Changes survive a crash; GETs and no-op DELs are not logged.
    server = Server(aof)
    assert server.startup == [f'Loaded 0 records from {aof}\n'], server.startup
    server.exchange(b'SET user:42 Arnav\nSET spaced   a  b  \nSET empty \nSET gone x\n'
                    b'DEL gone\nDEL missing\nGET user:42\n SET  tidy  value\r\n',
                    b'OK\nOK\nOK\nOK\nDELETED\nNOT_FOUND\nVALUE Arnav\nOK\n')
    server.crash()
    assert aof.read_bytes() == (b'SET user:42 Arnav\nSET spaced   a  b  \nSET empty \n'
                                b'SET gone x\nDEL gone\nSET tidy  value\n'), aof.read_bytes()
    server = Server(aof)
    assert server.startup == [f'Loaded 6 records from {aof}\n'], server.startup
    server.exchange(b'GET user:42\nGET spaced\nGET empty\nGET gone\nGET tidy\n',
                    b'VALUE Arnav\nVALUE   a  b  \nVALUE \nNOT_FOUND\nVALUE  value\n')
    server.exchange(b'SET user:42 Updated\n', b'OK\n')
    server.crash()
    server = Server(aof)
    server.exchange(b'GET user:42\n', b'VALUE Updated\n')
    server.crash()
    print('PASS: data survives SIGKILL; only real changes are logged')

    # A crash mid-write leaves a final record without '\n'. It is dropped, and
    # the file is cut so the next record is not glued onto the fragment.
    good = aof.read_bytes()
    aof.write_bytes(good + b'SET torn val')
    server = Server(aof)
    assert 'dropped 12 bytes' in server.startup[0], server.startup
    assert aof.read_bytes() == good
    server.exchange(b'GET torn\nSET after ok\n', b'NOT_FOUND\nOK\n')
    server.crash()
    server = Server(aof)
    server.exchange(b'GET torn\nGET after\nGET user:42\n', b'NOT_FOUND\nVALUE ok\nVALUE Updated\n')
    server.crash()
    print('PASS: torn final record is discarded and the log repaired')

    # Damage in the middle is not a crash artifact: refuse to start, keep the file.
    aof.write_bytes(b'SET a 1\nBOGUS\nSET b 2\n')
    message = fails_to_start(aof)
    assert 'corrupt record 2 at byte 8' in message, message
    assert aof.read_bytes() == b'SET a 1\nBOGUS\nSET b 2\n'
    aof.write_bytes(b'GET a\n')
    assert 'corrupt record 1' in fails_to_start(aof)
    print('PASS: corrupt log refuses to start and is left untouched')

    # Every fsync policy persists; everysec flushes from the idle event loop.
    for policy in ('always', 'everysec', 'no'):
        aof.unlink()
        server = Server(aof, '--fsync', policy)
        server.exchange(b'SET mode ' + policy.encode() + b'\n', b'OK\n')
        server.crash()
        server = Server(aof)
        server.exchange(b'GET mode\n', b'VALUE ' + policy.encode() + b'\n')
        server.crash()
    bad = subprocess.run([binary, '0', '--fsync', 'sometimes'], capture_output=True, timeout=5)
    assert bad.returncode != 0 and b'Usage' in bad.stderr
    print('PASS: always, everysec, and no fsync policies; invalid policy rejected')
