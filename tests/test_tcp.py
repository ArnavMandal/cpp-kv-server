"""Run with: python3 tests/test_tcp.py ./kvserver (port 6379 must be free)."""
import pathlib
import socket
import subprocess
import sys
import time

server = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve())],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def connect():
    return socket.create_connection(('127.0.0.1', 6379), timeout=2)

def exchange(commands, expected):
    with connect() as client:
        client.sendall(commands)
        client.shutdown(socket.SHUT_WR)
        result = b''
        while True:
            chunk = client.recv(8192)
            if not chunk:
                break
            result += chunk
        assert result == expected, (result, expected)

try:
    # Wait for our process to announce successful bind/listen. Do not accidentally
    # test a different server that already occupies this port.
    assert server.stdout.readline().startswith(b'Listening'), server.stderr.read()
    with connect() as client:
        reader = client.makefile('rb')
        client.sendall(b'SET shared hel')
        time.sleep(0.02)
        client.sendall(b'lo\nGET shared\nEXIT\n')
        assert reader.read() == b'OK\nVALUE hello\nBYE\n'
        reader.close()
    exchange(b'GET shared\r\nSET shared updated\nGET shared\nDEL shared\nGET shared\n',
             b'VALUE hello\nOK\nVALUE updated\nDELETED\nNOT_FOUND\n')
    exchange(b'SET empty \nGET empty\nGET\nGET empty extra\n',
             b'OK\nVALUE \nERROR expected COMMAND key\nERROR unexpected argument\n')
    exchange(b'SET unfinished value', b'')
    exchange(b'GET unfinished\n', b'NOT_FOUND\n')
    value = b'x' * (4096 - len(b'SET limit '))
    exchange(b'SET limit ' + value + b'\nGET limit\n', b'OK\nVALUE ' + value + b'\n')
    exchange(b'x' * 4097, b'ERROR command too long\n')
    exchange(b'GET empty\nEXIT\nSET ignored value\n', b'VALUE \nBYE\n')
    exchange(b'GET ignored\n', b'NOT_FOUND\n')
    print('PASS: fragmented and batched commands, reconnects, CRLF, mutation,')
    print('empty values, invalid input, incomplete EOF, line limits, and EXIT.')
finally:
    server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
