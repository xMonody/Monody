#!/usr/bin/env python3
"""
Mock compositor for testing xmonodybar, following the xmonodywm IPC protocol:

  compositor -> bar (newline-delimited JSON):
    {"event":"window_list","windows":[{"id":1,"app_id":"firefox"}],
     "focused_id":1}                            on connect (focused_id optional)
    {"event":"window_added","id":1,"app_id":"firefox"}
    {"event":"window_removed","id":1}
    {"event":"window_focus","id":1}          id 0 clears
    {"event":"window_full","id":1}           enter AND exit

  bar -> compositor:
    {"action":"focus_window","id":1}
    {"action":"list_windows"}                 answered with a fresh window_list
    {"action":"close_window","id":1}
    {"action":"maximize_window","id":1}      (compositor toggles)
    {"action":"minimize_window","id":1}

Commands (typed at the "mock>" prompt):
  add <id> <app_id>   window_added
  rm <id>             window_removed
  focus <id>          window_focus  (-1 -> id 0, clears)
  full <id>           window_full   (send twice to enter+exit fullscreen)
  quit
"""
import json
import os
import socket
import threading

SOCKET_PATH = os.environ.get("XMONODYWM_SOCKET", "/tmp/xmonodywm.sock")


def main():
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(8)
    print(f"[mock] listening on {SOCKET_PATH}")

    clients = set()
    lock = threading.Lock()
    windows = {}  # id -> app_id
    focused_id = 0

    def reader(conn):
        buf = b""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        print(f"[mock] <- bar sent: {line.decode()}")
                        try:
                            msg = json.loads(line)
                            if msg.get("action") == "list_windows":
                                send_window_list(target=conn)
                        except json.JSONDecodeError:
                            pass
        except OSError:
            pass
        finally:
            with lock:
                clients.discard(conn)
                n = len(clients)
            conn.close()
            print(f"[mock] bar disconnected ({n} client(s) left)")

    def send_window_list(target=None):
        payload = {
            "event": "window_list",
            "windows": [{"id": i, "app_id": a} for i, a in sorted(windows.items())],
            "focused_id": focused_id,
        }
        msg = (json.dumps(payload) + "\n").encode()
        with lock:
            targets = [target] if target else list(clients)
            for c in targets:
                try:
                    c.sendall(msg)
                except OSError:
                    pass
        print(f"[mock] -> {payload}", flush=True)

    def accept_loop():
        while True:
            try:
                conn, _ = server.accept()
            except OSError:
                return
            with lock:
                clients.add(conn)
                n = len(clients)
            print(f"[mock] bar connected ({n} client(s))")
            threading.Thread(target=reader, args=(conn,), daemon=True).start()
            send_window_list(target=conn)  # snapshot, like xmonodywm

    threading.Thread(target=accept_loop, daemon=True).start()

    def broadcast(payload):
        msg = (json.dumps(payload) + "\n").encode()
        with lock:
            for c in list(clients):
                try:
                    c.sendall(msg)
                except OSError:
                    clients.discard(c)
        print(f"[mock] -> {payload}", flush=True)

    while True:
        try:
            cmd = input("mock> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not cmd:
            continue
        parts = cmd.split()
        op = parts[0].lower()

        if op == "quit":
            break
        elif op == "add" and len(parts) >= 3:
            wid, app = int(parts[1]), parts[2]
            windows[wid] = app
            broadcast({"event": "window_added", "id": wid, "app_id": app})
        elif op == "rm" and len(parts) >= 2:
            wid = int(parts[1])
            windows.pop(wid, None)
            if focused_id == wid:
                focused_id = 0
            broadcast({"event": "window_removed", "id": wid})
        elif op == "focus" and len(parts) >= 2:
            wid = max(int(parts[1]), 0)   # -1 -> 0 (clear)
            focused_id = wid
            broadcast({"event": "window_focus", "id": wid})
        elif op == "full" and len(parts) >= 2:
            broadcast({"event": "window_full", "id": int(parts[1])})
        else:
            print("usage: add <id> <app_id> | rm <id> | focus <id> | full <id> | quit")

    server.close()
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass


if __name__ == "__main__":
    main()
