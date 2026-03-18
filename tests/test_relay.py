"""
End-to-end relay test for the OmniDesk24 signaling server.

Wire protocol:
  [4B magic=OMND][2B version=1][2B type=HELLO(1)][4B length][JSON payload]

Test flow:
  1. Start signaling server (passed via --port, default 9800)
  2. "Host" client connects and registers as ID "AAAAAAAA"
  3. "Viewer" client connects and registers as ID "BBBBBBBB"
  4. Viewer sends connect_request targeting "AAAAAAAA"
  5. Assert host receives connect_request with correct from_id
  6. Host sends connect_accept back to viewer
  7. Assert viewer receives connect_accept
  8. Test connect to unknown ID → expect user_offline
  9. Test duplicate registration (reconnect scenario)
  10. Test heartbeat round-trip
"""

import json
import socket
import struct
import sys
import time
import threading
import subprocess
import os
import signal
import argparse

# -- Protocol constants --------------------------------------------------------
MAGIC   = b'OMND'          # 0x4F4D4E44
VERSION = 1
TYPE_HELLO = 1

HEADER_SIZE = 12            # 4+2+2+4


def pack_msg(payload: dict) -> bytes:
    """Encode a JSON payload into the OmniDesk wire format."""
    body = json.dumps(payload).encode()
    hdr  = MAGIC
    hdr += struct.pack('>HHI', VERSION, TYPE_HELLO, len(body))
    return hdr + body


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('Server closed connection unexpectedly')
        buf += chunk
    return buf


def recv_msg(sock: socket.socket, timeout: float = 5.0) -> dict:
    """Block until one complete message arrives, return parsed JSON."""
    sock.settimeout(timeout)
    hdr = recv_exact(sock, HEADER_SIZE)
    magic   = hdr[0:4]
    version = struct.unpack('>H', hdr[4:6])[0]
    _type   = struct.unpack('>H', hdr[6:8])[0]
    length  = struct.unpack('>I', hdr[8:12])[0]
    if magic != MAGIC:
        raise ValueError(f'Bad magic: {magic!r}')
    if version != VERSION:
        raise ValueError(f'Bad version: {version}')
    body = recv_exact(sock, length)
    return json.loads(body.decode())


# -- Client helper -------------------------------------------------------------
class SignalingClient:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=5)

    def send(self, payload: dict):
        self.sock.sendall(pack_msg(payload))

    def recv(self, timeout: float = 5.0) -> dict:
        return recv_msg(self.sock, timeout)

    def register(self, user_id: str, local_host: str = '127.0.0.1', local_port: int = 0):
        self.send({
            'type':       'register',
            'user_id':    user_id,
            'local_host': local_host,
            'local_port': str(local_port),
        })
        resp = self.recv()
        assert resp.get('type') == 'register_ok', \
            f'Expected register_ok, got: {resp}'
        return resp

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# -- Test runner ---------------------------------------------------------------
PASS = 'PASS'
FAIL = 'FAIL'

results = []

def run_test(name: str, fn):
    try:
        fn()
        print(f'  [{PASS}] {name}')
        results.append((name, True))
    except Exception as e:
        print(f'  [{FAIL}] {name}')
        print(f'          {e}')
        results.append((name, False))


def make_clients(host, port):
    h = SignalingClient(host, port)
    v = SignalingClient(host, port)
    return h, v


# -- Individual tests ----------------------------------------------------------
def test_register_ok(host, port):
    c = SignalingClient(host, port)
    c.register('TTTTTTTT')
    c.close()


def test_register_invalid_id(host, port):
    c = SignalingClient(host, port)
    c.send({'type': 'register', 'user_id': 'SHORT', 'local_host': '', 'local_port': '0'})
    resp = c.recv()
    assert resp.get('type') == 'register_fail', f'Expected register_fail, got: {resp}'
    c.close()


def test_connect_request_relayed(host, port):
    """Viewer sends connect_request; host must receive it with correct from_id."""
    hst, vwr = make_clients(host, port)
    hst.register('HHHHHHHH')
    vwr.register('VVVVVVVV')

    vwr.send({'type': 'connect_request', 'from_id': 'VVVVVVVV', 'target_id': 'HHHHHHHH'})
    msg = hst.recv(timeout=5)

    assert msg.get('type') == 'connect_request', \
        f'Host expected connect_request, got: {msg}'
    assert msg.get('from_id') == 'VVVVVVVV', \
        f'Wrong from_id: {msg.get("from_id")}'

    hst.close()
    vwr.close()


def test_connect_accept_relayed(host, port):
    """Host accepts; viewer must receive connect_accept with correct from_id."""
    hst, vwr = make_clients(host, port)
    hst.register('HHHHHHHH')
    vwr.register('VVVVVVVV')

    # Viewer requests connection
    vwr.send({'type': 'connect_request', 'from_id': 'VVVVVVVV', 'target_id': 'HHHHHHHH'})
    hst.recv(timeout=5)  # consume the relayed request

    # Host accepts
    hst.send({'type': 'connect_accept', 'from_id': 'HHHHHHHH', 'target_id': 'VVVVVVVV'})
    msg = vwr.recv(timeout=5)

    assert msg.get('type') == 'connect_accept', \
        f'Viewer expected connect_accept, got: {msg}'
    assert msg.get('from_id') == 'HHHHHHHH', \
        f'Wrong from_id in accept: {msg.get("from_id")}'

    hst.close()
    vwr.close()


def test_connect_reject_relayed(host, port):
    """Host rejects; viewer must receive connect_reject."""
    hst, vwr = make_clients(host, port)
    hst.register('HHHHHHHH')
    vwr.register('VVVVVVVV')

    vwr.send({'type': 'connect_request', 'from_id': 'VVVVVVVV', 'target_id': 'HHHHHHHH'})
    hst.recv(timeout=5)

    hst.send({'type': 'connect_reject', 'from_id': 'HHHHHHHH',
              'target_id': 'VVVVVVVV', 'reason': 'busy'})
    msg = vwr.recv(timeout=5)

    assert msg.get('type') == 'connect_reject', \
        f'Viewer expected connect_reject, got: {msg}'
    assert msg.get('from_id') == 'HHHHHHHH', \
        f'Wrong from_id in reject: {msg.get("from_id")}'

    hst.close()
    vwr.close()


def test_user_offline(host, port):
    """Connecting to unknown ID → server sends user_offline back to requester."""
    vwr = SignalingClient(host, port)
    vwr.register('VVVVVVVV')

    vwr.send({'type': 'connect_request', 'from_id': 'VVVVVVVV', 'target_id': 'XXXXXXXX'})
    msg = vwr.recv(timeout=5)

    assert msg.get('type') == 'user_offline', \
        f'Expected user_offline, got: {msg}'
    assert msg.get('target_id') == 'XXXXXXXX', \
        f'Wrong target_id in user_offline: {msg}'

    vwr.close()


def test_heartbeat(host, port):
    """Heartbeat → server must respond with heartbeat_ack."""
    c = SignalingClient(host, port)
    c.register('HBHBHBHB')
    c.send({'type': 'heartbeat', 'user_id': 'HBHBHBHB'})
    msg = c.recv(timeout=5)
    assert msg.get('type') == 'heartbeat_ack', \
        f'Expected heartbeat_ack, got: {msg}'
    c.close()


def test_duplicate_registration(host, port):
    """Registering the same ID twice (reconnect) should succeed."""
    c1 = SignalingClient(host, port)
    c1.register('DUPDUP01')

    c2 = SignalingClient(host, port)
    c2.register('DUPDUP01')   # re-register same ID on new connection

    # Make sure c2 can be found (c1 should be displaced)
    vwr = SignalingClient(host, port)
    vwr.register('DUPDUP02')
    vwr.send({'type': 'connect_request', 'from_id': 'DUPDUP02', 'target_id': 'DUPDUP01'})
    # Should relay to c2 (the newer connection), not user_offline
    msg = c2.recv(timeout=5)
    assert msg.get('type') == 'connect_request', \
        f'Expected relayed connect_request to c2, got: {msg}'

    c1.close(); c2.close(); vwr.close()


def test_address_info_in_relay(host, port):
    """
    The server should include public/local address information in the relayed
    connect_request so that hole-punching can use it.
    """
    hst, vwr = make_clients(host, port)
    hst.register('HHHHHHHH', local_host='10.0.0.1', local_port=50000)
    vwr.register('VVVVVVVV', local_host='10.0.0.2', local_port=50001)

    vwr.send({'type': 'connect_request', 'from_id': 'VVVVVVVV', 'target_id': 'HHHHHHHH'})
    msg = hst.recv(timeout=5)

    # The relay should contain viewer's public address (as seen by the server)
    assert 'from_public_host' in msg, f'Missing from_public_host in relay: {msg}'
    assert 'from_local_host' in msg, f'Missing from_local_host in relay: {msg}'
    assert msg.get('from_local_host') == '10.0.0.2', \
        f'Wrong local host: {msg.get("from_local_host")}'

    hst.close()
    vwr.close()


# -- Main ----------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description='OmniDesk24 signaling relay test')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9800)
    parser.add_argument('--launch-server', action='store_true',
                        help='Launch omnidesk24.exe --server before running tests')
    parser.add_argument('--server-exe', default=None,
                        help='Path to omnidesk24.exe (required with --launch-server)')
    args = parser.parse_args()

    server_proc = None
    if args.launch_server:
        exe = args.server_exe
        if not exe:
            # Try to find it relative to this script
            script_dir = os.path.dirname(os.path.abspath(__file__))
            candidates = [
                os.path.join(script_dir, '..', 'build-fresh', 'omnidesk24.exe'),
                os.path.join(script_dir, '..', 'build', 'omnidesk24.exe'),
            ]
            for c in candidates:
                if os.path.isfile(c):
                    exe = os.path.abspath(c)
                    break
        if not exe:
            print('ERROR: Could not find omnidesk24.exe. Use --server-exe.')
            sys.exit(1)

        print(f'  Launching server: {exe} --server --signal-port {args.port}')
        server_proc = subprocess.Popen(
            [exe, '--server', '--signal-port', str(args.port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.5)   # give it a moment to start

    print(f'\nOmniDesk24 Signaling Relay Test  ->  {args.host}:{args.port}\n')

    tests = [
        ('Register with valid 8-char ID',        lambda: test_register_ok(args.host, args.port)),
        ('Register with invalid (short) ID',     lambda: test_register_invalid_id(args.host, args.port)),
        ('connect_request relayed to host',      lambda: test_connect_request_relayed(args.host, args.port)),
        ('connect_accept relayed to viewer',     lambda: test_connect_accept_relayed(args.host, args.port)),
        ('connect_reject relayed to viewer',     lambda: test_connect_reject_relayed(args.host, args.port)),
        ('user_offline for unknown target',      lambda: test_user_offline(args.host, args.port)),
        ('heartbeat -> heartbeat_ack',           lambda: test_heartbeat(args.host, args.port)),
        ('duplicate registration (reconnect)',   lambda: test_duplicate_registration(args.host, args.port)),
        ('address info included in relay',       lambda: test_address_info_in_relay(args.host, args.port)),
    ]

    for name, fn in tests:
        run_test(name, fn)

    passed = sum(1 for _, ok in results if ok)
    total  = len(results)
    print(f'\n{"-"*52}')
    print(f'  Results: {passed}/{total} passed')

    if server_proc:
        server_proc.terminate()
        server_proc.wait()

    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
