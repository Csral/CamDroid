"""Loopback CLI/core smoke test"""

import os
import signal
import socket
import struct
import subprocess
import sys
import time

def packet(flags, payload):
    # Normal framing, protocol v1, Communication type, no extensions.
    return b"Nya" + bytes([flags, 1, 0x10, 0, 0]) + struct.pack(">QQI", 0, len(payload), 0) + payload


def read_exact(peer, size):
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            raise RuntimeError("Server disconnected during handshake")
        data.extend(chunk)
    return bytes(data)


def discover(port, busy):
    nonce = os.urandom(8)
    request = b"CDDISC01" + nonce
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.settimeout(0.15)
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            probe.sendto(request, ("255.255.255.255", 39696))
            try:
                reply, sender = probe.recvfrom(128)
            except (socket.timeout, ConnectionResetError):
                continue
            if len(reply) < 22 or reply[:8] != b"CDPEER01" or reply[8:16] != nonce:
                continue
            assert sender[1] == 39696
            if struct.unpack(">H", reply[16:18])[0] != port:
                continue
            assert reply[18] == 1 and reply[19] == int(busy)
            assert len(reply) == 21 + reply[20] and 1 <= reply[20] <= 63
            assert reply[21:].decode("utf-8", errors="replace").strip()
            return
    raise RuntimeError("Desktop discovery did not report the correct receiver")


def main():
    executable = sys.argv[1]
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    options = {}
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        options = {"creationflags": subprocess.CREATE_NEW_CONSOLE, "startupinfo": startup}
    process = subprocess.Popen(
        [executable, "--headless", "--port", str(port)], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, **options
    )
    try:
        discover(port, False)
        deadline = time.monotonic() + 10
        while True:
            try:
                peer = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Server did not start")
                time.sleep(0.05)

        with peer:
            peer.settimeout(5)
            peer.sendall(packet(4, bytes([1, 1, 1, 0])))
            header = read_exact(peer, 28)
            assert header[:3] == b"Nya" and header[3] == 33
            size, offset = struct.unpack(">QI", header[16:28])
            payload = read_exact(peer, size + offset)
            assert payload == bytes([1])
            peer.sendall(packet(33, bytes([1, 0, 174, 239, 0, 17, 255])))
            # Empty EOS is legal even before configuration; the decoder drain is a no-op.
            eos = b"Nya" + bytes([16, 1, 0x40, 0x40, 0]) + struct.pack(">QQI", 0, 0, 0)
            peer.sendall(eos)
            discover(port, True)

        # The receiver now remains available after EOF. Ask its normal console
        # cancellation path to stop rather than expecting a one-connection exit.
        time.sleep(0.3)
        assert process.poll() is None, "Receiver exited instead of listening again"
        discover(port, False)
        if os.name == "nt":
            helper = (
                "import ctypes,sys,time; k=ctypes.windll.kernel32; k.FreeConsole(); "
                "assert k.AttachConsole(int(sys.argv[1])); "
                "handler=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_uint)(lambda event:1); "
                "assert k.SetConsoleCtrlHandler(handler,1); "
                "assert k.GenerateConsoleCtrlEvent(1,0); time.sleep(.2)"
            )
            subprocess.run([sys.executable, "-c", helper, str(process.pid)], check=True)
        else:
            process.send_signal(signal.SIGINT)
        output, _ = process.communicate(timeout=10)
        assert process.returncode == 0, output
        assert f"Waiting on port {port}" in output, output
        assert "Device:" in output and "Listening:" in output, output
        assert "Connected." in output, output
        assert "Video stream ended. Total decoded frames: 0" in output, output
        assert "Disconnected. Total decoded frames: 0" in output, output
        print("Application/CLI, IP/port discovery, busy state and shutdown checks passed.")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


if __name__ == "__main__":
    main()