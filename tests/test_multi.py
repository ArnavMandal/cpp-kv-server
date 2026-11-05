"""End-to-end multi-client tests; no dependencies beyond Python's standard library.

Run: python3 tests/test_multi.py ./kvserver_multi
The server uses an OS-selected free port, so other local servers can stay running.
"""
import concurrent.futures
import pathlib
import re
import select
import socket
import struct
import subprocess
import sys

server = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), '0'],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def connect():
    return socket.create_connection(('127.0.0.1', port), timeout=3)


def receive_all(client):
    chunks = []
    while True:
        chunk = client.recv(65536)
        if not chunk:
            return b''.join(chunks)
        chunks.append(chunk)


def exchange(commands, expected):
    with connect() as client:
        client.sendall(commands)
        client.shutdown(socket.SHUT_WR)
        actual = receive_all(client)
        assert actual == expected, (actual[:200], expected[:200])


try:
    assert select.select([server.stdout], [], [], 5)[0], 'Server startup timed out'
    line = server.stdout.readline()
    match = re.match(rb'Listening on 127\.0\.0\.1:(\d+) ', line)
    assert match, ('Startup failed', line, server.poll())
    port = int(match.group(1))

    # One idle connection and one incomplete command must not block other clients.
    with connect() as idle, connect() as partial:
        partial.sendall(b'SET split hel')
        assert not select.select([partial], [], [], 0.05)[0], 'Executed incomplete line'
        exchange(b'SET shared Arnav\nGET shared\r\nGET split\n',
                 b'OK\nVALUE Arnav\nNOT_FOUND\n')
        partial.sendall(b'lo\nGET split\nEXIT\nSET ignored value\n')
        assert receive_all(partial) == b'OK\nVALUE hello\nBYE\n'
        exchange(b'GET split\nGET ignored\n', b'VALUE hello\nNOT_FOUND\n')
    print('PASS: idle clients, split input, separate buffers, shared store, EXIT')

    exchange(b'SET shared updated\nGET shared\nDEL shared\nDEL shared\n',
             b'OK\nVALUE updated\nDELETED\nNOT_FOUND\n')
    exchange(b'SET empty \nGET empty\nGET\nGET empty extra\nBOGUS key\n',
             b'OK\nVALUE \nERROR expected COMMAND key\nERROR unexpected argument\nERROR unknown command\n')
    exchange(b'SET spaces   hi  \nGET spaces\nSET unfinished value',
             b'OK\nVALUE   hi  \n')
    exchange(b'GET unfinished\n', b'NOT_FOUND\n')
    value = b'x' * (4096 - len(b'SET large '))
    exchange(b'SET large ' + value + b'\nGET large\n', b'OK\nVALUE ' + value + b'\n')
    exchange(b'x' * 4097, b'ERROR command too long\n')
    exchange(b'GET empty\n', b'VALUE \n')
    print('PASS: protocol regression, line limit, EOF with pending replies')

    # Queue ~4 MB of replies to a client with a small receive buffer. Leave it
    # unread while another client completes commands, then drain in exact order.
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        slow.settimeout(5)
        slow.connect(('127.0.0.1', port))
        slow.sendall(b'GET large\n' * 1000 + b'EXIT\n')
        assert select.select([slow], [], [], 2)[0], 'No output to slow client'
        for _ in range(10):
            exchange(b'SET live yes\nGET live\n', b'OK\nVALUE yes\n')
        with slow.makefile('rb') as reader:
            for _ in range(1000):
                assert reader.readline() == b'VALUE ' + value + b'\n'
            assert reader.readline() == b'BYE\n'
            assert reader.read() == b''
    print('PASS: slow reader isolation and ordered multi-megabyte response drain')

    def worker(index):
        key = ('client:' + str(index)).encode()
        exchange(b'SET ' + key + b' value\nGET ' + key + b'\nDEL ' + key + b'\n',
                 b'OK\nVALUE value\nDELETED\n')

    with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
        list(pool.map(worker, range(100)))
    for _ in range(10):
        with connect() as reset:
            reset.sendall(b'GET large\n' * 10)
            reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    exchange(b'GET live\n', b'VALUE yes\n')
    assert server.poll() is None, 'Server exited unexpectedly'
    print('PASS: concurrent clients, connection churn, abrupt peer resets')
finally:
    server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
    errors = server.stderr.read().decode(errors='replace')
    if errors:
        print(errors, file=sys.stderr)
        raise AssertionError('Server emitted diagnostics')
