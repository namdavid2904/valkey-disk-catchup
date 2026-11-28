#!/usr/bin/env python3
# Usage: insync_probe.py <port> <ready-file>. Times SET plus WAIT 1 every 10 ms until SIGTERM, then prints p50,p99,max in ms.
import signal
import socket
import sys
import time

REQ = b"*3\r\n$3\r\nSET\r\n$13\r\ncatchup-probe\r\n$1\r\n1\r\n*3\r\n$4\r\nWAIT\r\n$1\r\n1\r\n$4\r\n1000\r\n"
stop = False


def on_term(*_):
    global stop
    stop = True


def main():
    signal.signal(signal.SIGTERM, on_term)
    sock = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
    reader = sock.makefile("rb")
    open(sys.argv[2], "w").close()
    lat = []
    while not stop:
        t = time.perf_counter()
        sock.sendall(REQ)
        reader.readline()
        reader.readline()
        lat.append((time.perf_counter() - t) * 1000)
        time.sleep(0.01)
    lat.sort()
    if not lat:
        print(",,")
        return
    pick = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    print(f"{pick(0.5):.3f},{pick(0.99):.3f},{lat[-1]:.3f}")


main()
