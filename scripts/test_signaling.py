#!/usr/bin/env python3
"""
OmniDesk24 Signaling Server End-to-End Test
============================================
Tests the full signaling protocol by simulating two clients.
Run directly on the VPS: python3 /root/omnidesk24/scripts/test_signaling.py

Protocol wire format:
  [4B magic=0x4F4D4E44][2B version=1][2B type][4B payload_len][JSON payload]
All multi-byte values are network byte order (big-endian).
"""

import socket
import struct
import json
import time
import sys
import subprocess
import random
import string

MAGIC = 0x4F4D4E44
VERSION = 1
MSG_HELLO = 0x0001

HOST = "127.0.0.1"
PORT = 9800

PASS = 0
FAIL = 0

def make_header(payload_len):
    """Build a 12-byte ControlHeader."""
    return struct.pack("!IHHI", MAGIC, VERSION, MSG_HELLO, payload_len)

def parse_header(data):
    """Parse 12-byte ControlHeader. Returns (magic, version, type, length)."""
    magic = struct.unpack("!I", data[0:4])[0]
    version = struct.unpack("!H", data[4:6])[0]
    msg_type = struct.unpack("!H", data[6:8])[0]
    length = struct.unpack("!I", data[8:12])[0]
    return magic, version, msg_type, length

def send_msg(sock, payload_dict):
    """Send a JSON message with ControlHeader prefix."""
    payload = json.dumps(payload_dict).encode()
    header = make_header(len(payload))
    sock.sendall(header + payload)

def recv_msg(sock, timeout=5):
    """Receive a complete message. Returns parsed JSON dict."""
    sock.settimeout(timeout)
    # Read 12-byte header
    header_data = b""
    while len(header_data) < 12:
        chunk = sock.recv(12 - len(header_data))
        if not chunk:
            raise ConnectionError("Server closed connection")
        header_data += chunk

    magic, version, msg_type, length = parse_header(header_data)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08X}")
    if version != VERSION:
        raise ValueError(f"Bad version: {version}")

    # Read payload
    payload_data = b""
    while len(payload_data) < length:
        chunk = sock.recv(length - len(payload_data))
        if not chunk:
            raise ConnectionError("Server closed connection during payload")
        payload_data += chunk

    return json.loads(payload_data.decode())

def connect_client():
    """Create a TCP connection to the signaling server."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect((HOST, PORT))
    return s

def random_id():
    return ''.join(random.choices(string.ascii_lowercase + string.digits, k=8))

def test(name, condition):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}")

def run_tests():
    global PASS, FAIL

    print("=" * 60)
    print("OmniDesk24 Signaling Server — End-to-End Tests")
    print("=" * 60)

    # ----- Test 0: Server reachable -----
    print("\n--- Test 0: Server Connectivity ---")
    try:
        s = connect_client()
        test("TCP connect to port 9800", True)
        s.close()
    except Exception as e:
        test(f"TCP connect to port 9800 ({e})", False)
        print("\nFATAL: Cannot connect to server. Is omnidesk24-server running?")
        sys.exit(1)

    # ----- Test 1: Register Client A -----
    print("\n--- Test 1: Register Client A ---")
    id_a = random_id()
    client_a = connect_client()
    send_msg(client_a, {
        "type": "register",
        "user_id": id_a,
        "local_host": "192.168.1.100",
        "local_port": "12345"
    })
    resp = recv_msg(client_a)
    test(f"Register response type = register_ok", resp.get("type") == "register_ok")
    test(f"Register response user_id = {id_a}", resp.get("user_id") == id_a)

    # ----- Test 2: Register Client B -----
    print("\n--- Test 2: Register Client B ---")
    id_b = random_id()
    client_b = connect_client()
    send_msg(client_b, {
        "type": "register",
        "user_id": id_b,
        "local_host": "192.168.1.200",
        "local_port": "54321"
    })
    resp = recv_msg(client_b)
    test(f"Register response type = register_ok", resp.get("type") == "register_ok")
    test(f"Register response user_id = {id_b}", resp.get("user_id") == id_b)

    # ----- Test 3: Register with invalid ID -----
    print("\n--- Test 3: Invalid Registration ---")
    client_bad = connect_client()
    send_msg(client_bad, {
        "type": "register",
        "user_id": "abc",  # Too short (needs 8 chars)
        "local_host": "192.168.1.1",
        "local_port": "1000"
    })
    resp = recv_msg(client_bad)
    test("Reject invalid user_id (too short)", resp.get("type") == "register_fail")
    test("Reason = invalid_user_id", resp.get("reason") == "invalid_user_id")
    client_bad.close()

    # ----- Test 4: Heartbeat -----
    print("\n--- Test 4: Heartbeat ---")
    send_msg(client_a, {
        "type": "heartbeat",
        "user_id": id_a
    })
    resp = recv_msg(client_a)
    test("Heartbeat ACK received", resp.get("type") == "heartbeat_ack")

    # ----- Test 5: Connection Request (A -> B) -----
    print("\n--- Test 5: Connection Request (A wants to view B) ---")
    send_msg(client_a, {
        "type": "connect_request",
        "from_id": id_a,
        "target_id": id_b
    })
    # B should receive the relayed connect request
    resp_b = recv_msg(client_b)
    test("B receives connect_request", resp_b.get("type") == "connect_request")
    test("from_id matches A", resp_b.get("from_id") == id_a)
    test("from_public_host present", len(resp_b.get("from_public_host", "")) > 0)
    test("from_local_host = 192.168.1.100", resp_b.get("from_local_host") == "192.168.1.100")

    # ----- Test 6: Connection Accept (B accepts) -----
    print("\n--- Test 6: Connection Accept ---")
    send_msg(client_b, {
        "type": "connect_accept",
        "from_id": id_b,
        "target_id": id_a
    })
    # A should receive the acceptance with B's address info
    resp_a = recv_msg(client_a)
    test("A receives connect_accept", resp_a.get("type") == "connect_accept")
    test("from_id matches B", resp_a.get("from_id") == id_b)
    test("peer_public_host present", len(resp_a.get("peer_public_host", "")) > 0)
    test("peer_local_host = 192.168.1.200", resp_a.get("peer_local_host") == "192.168.1.200")

    # ----- Test 7: Connection Reject flow -----
    print("\n--- Test 7: Connection Reject ---")
    # A requests to connect to B again
    send_msg(client_a, {
        "type": "connect_request",
        "from_id": id_a,
        "target_id": id_b
    })
    recv_msg(client_b)  # B receives the request

    # B rejects
    send_msg(client_b, {
        "type": "connect_reject",
        "from_id": id_b,
        "target_id": id_a,
        "reason": "user_declined"
    })
    resp_a = recv_msg(client_a)
    test("A receives connect_reject", resp_a.get("type") == "connect_reject")
    test("Reject reason = user_declined", resp_a.get("reason") == "user_declined")

    # ----- Test 8: Connect to offline user -----
    print("\n--- Test 8: Connect to Offline User ---")
    send_msg(client_a, {
        "type": "connect_request",
        "from_id": id_a,
        "target_id": "zzzzxxxx"  # Nobody registered with this ID
    })
    resp = recv_msg(client_a)
    test("user_offline response", resp.get("type") == "user_offline")
    test("target_id = zzzzxxxx", resp.get("target_id") == "zzzzxxxx")

    # ----- Test 9: Duplicate registration (reconnect) -----
    print("\n--- Test 9: Reconnect (Duplicate Registration) ---")
    client_a2 = connect_client()
    send_msg(client_a2, {
        "type": "register",
        "user_id": id_a,
        "local_host": "10.0.0.50",
        "local_port": "9999"
    })
    resp = recv_msg(client_a2)
    test("Re-register succeeds", resp.get("type") == "register_ok")
    # Old client_a should be disconnected by server
    time.sleep(0.5)
    # Use new connection for remaining tests
    client_a.close()
    client_a = client_a2

    # ----- Test 10: Multiple heartbeats -----
    print("\n--- Test 10: Multiple Heartbeats ---")
    ok_count = 0
    for i in range(5):
        send_msg(client_a, {"type": "heartbeat", "user_id": id_a})
        r = recv_msg(client_a)
        if r.get("type") == "heartbeat_ack":
            ok_count += 1
    test(f"5/5 heartbeat ACKs received", ok_count == 5)

    # ----- Test 11: Concurrent connections -----
    print("\n--- Test 11: Concurrent Registrations (10 clients) ---")
    extra_clients = []
    extra_ids = []
    all_ok = True
    for i in range(10):
        cid = random_id()
        try:
            c = connect_client()
            send_msg(c, {
                "type": "register",
                "user_id": cid,
                "local_host": f"10.0.{i}.1",
                "local_port": str(30000 + i)
            })
            r = recv_msg(c)
            if r.get("type") != "register_ok":
                all_ok = False
            extra_clients.append(c)
            extra_ids.append(cid)
        except Exception as e:
            all_ok = False
            print(f"    Client {i} failed: {e}")
    test("10 concurrent registrations", all_ok)

    # Clean up extra clients
    for c in extra_clients:
        c.close()

    # ----- Test 12: PostgreSQL persistence -----
    print("\n--- Test 12: PostgreSQL Persistence Check ---")
    try:
        result = subprocess.run(
            ["sudo", "-u", "postgres", "psql", "-d", "omnidesk24", "-t", "-c",
             "SELECT count(*) FROM users;"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            count = result.stdout.strip()
            test(f"Users table accessible (count={count})", True)
        else:
            test(f"Users table query failed: {result.stderr.strip()}", False)
    except FileNotFoundError:
        print("  [SKIP] psql not available (not running on VPS)")
    except Exception as e:
        test(f"DB check: {e}", False)

    try:
        result = subprocess.run(
            ["sudo", "-u", "postgres", "psql", "-d", "omnidesk24", "-t", "-c",
             "SELECT count(*) FROM sessions;"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            count = result.stdout.strip()
            test(f"Sessions table accessible (count={count})", True)
        else:
            test(f"Sessions table query failed", False)
    except FileNotFoundError:
        pass
    except Exception as e:
        test(f"DB check: {e}", False)

    try:
        result = subprocess.run(
            ["sudo", "-u", "postgres", "psql", "-d", "omnidesk24", "-t", "-c",
             "SELECT count(*) FROM events;"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            count = result.stdout.strip()
            test(f"Events table accessible (count={count})", True)
        else:
            test(f"Events table query failed", False)
    except FileNotFoundError:
        pass
    except Exception as e:
        test(f"DB check: {e}", False)

    # ----- Test 13: Service health -----
    print("\n--- Test 13: Service Status ---")
    try:
        result = subprocess.run(
            ["systemctl", "is-active", "omnidesk24-server"],
            capture_output=True, text=True, timeout=5
        )
        status = result.stdout.strip()
        test(f"systemd service active (status={status})", status == "active")
    except FileNotFoundError:
        print("  [SKIP] systemctl not available")
    except Exception as e:
        test(f"Service check: {e}", False)

    # Cleanup
    client_a.close()
    client_b.close()

    # ----- Summary -----
    print("\n" + "=" * 60)
    total = PASS + FAIL
    print(f"RESULTS: {PASS}/{total} passed, {FAIL} failed")
    if FAIL == 0:
        print("ALL TESTS PASSED!")
    else:
        print(f"WARNING: {FAIL} test(s) failed")
    print("=" * 60)

    return FAIL == 0

if __name__ == "__main__":
    try:
        success = run_tests()
        sys.exit(0 if success else 1)
    except ConnectionRefusedError:
        print("ERROR: Connection refused on port 9800. Is omnidesk24-server running?")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
