import argparse
import json
import re
import socket
import struct
import time

class Remote:
    def __init__(self, address, port):
        self.sock = socket.create_connection((address, port), timeout=4)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.offset = 0
        self.acks = True

    def read_byte(self):
        if self.offset == len(self.buffer):
            self.buffer = self.sock.recv(4096)
            self.offset = 0
            if not self.buffer:
                raise EOFError("Luma disconnected")
        value = self.buffer[self.offset:self.offset + 1]
        self.offset += 1
        return value

    def receive(self):
        while self.read_byte() != b"$":
            pass
        wire = bytearray()
        while (value := self.read_byte()) != b"#":
            wire.extend(value)
        checksum = self.read_byte() + self.read_byte()
        if sum(wire) % 256 != int(checksum, 16):
            if self.acks:
                self.sock.sendall(b"-")
            raise ValueError("RSP checksum mismatch")
        if self.acks:
            self.sock.sendall(b"+")
        result = bytearray()
        i = 0
        while i < len(wire):
            value = wire[i]
            i += 1
            if value == 0x7d:
                result.append(wire[i] ^ 0x20)
                i += 1
            elif value == 0x2a:
                result.extend(bytes((result[-1],)) * (wire[i] - 29))
                i += 1
            else:
                result.append(value)
        return result.decode("ascii", "replace")

    def command(self, command):
        body = command.encode("ascii")
        self.sock.sendall(b"$" + body + b"#" + ("%02x" % (sum(body) % 256)).encode())
        if self.acks and self.read_byte() != b"+":
            raise ConnectionError(f"No ACK for {command}")
        return self.receive()

    def close(self):
        self.sock.close()

def expect_ok(command, response):
    if response != "OK":
        raise RuntimeError(f"{command} returned {response!r}")

def trace(args):
    remote = Remote(args.ip, args.port)
    attached = False
    stopped = True
    tls_by_thread = {}
    start = time.monotonic()
    try:
        features = remote.command("qSupported:multiprocess+;swbreak+")
        print("features:", features, flush=True)
        expect_ok("extended mode", remote.command("!"))
        if "QStartNoAckMode+" in features:
            expect_ok("no ACK", remote.command("QStartNoAckMode"))
            remote.acks = False
        stop = remote.command(f"vAttach;{args.pid:x}")
        print("attach:", stop[:140], flush=True)
        if not stop.startswith(("T", "S")):
            raise RuntimeError("The requested process did not attach")
        attached = True
        expect_ok("syscall catch", remote.command("QCatchSyscalls:1;32"))
        deadline = time.monotonic() + args.seconds
        count = 0
        with open(args.output, "w", encoding="utf-8") as output:
            while time.monotonic() < deadline and count < args.limit:
                stopped = False
                stop = remote.command("c")
                stopped = True
                if not stop.startswith(("T", "S")):
                    raise RuntimeError(f"Unexpected process state {stop[:160]}")
                if "syscall_entry:32;" not in stop:
                    continue
                fields = dict(re.findall(r"(?:T[0-9a-f]{2}|;)([a-z0-9_]+):([^;]+)", stop))
                thread = fields["thread"]
                tid = thread.rsplit(".", 1)[-1]
                if tid not in tls_by_thread:
                    answer = remote.command(f"qGetTLSAddr:{tid},80,0")
                    if not re.fullmatch(r"[a-f0-9]{8}", answer):
                        extra = remote.command(f"qThreadExtraInfo,{thread}")
                        match = re.search(r"TLS: 0x([a-f0-9]+)", bytes.fromhex(extra).decode())
                        if not match:
                            raise RuntimeError(f"No TLS address for thread {thread}")
                        tls_by_thread[tid] = int(match.group(1), 16) + 0x80
                    else:
                        tls_by_thread[tid] = int(answer, 16)
                buffer = remote.command(f"m{tls_by_thread[tid]:x},30")
                if not re.fullmatch(r"[a-f0-9]{96}", buffer):
                    raise RuntimeError(f"Could not read IPC header for thread {thread}: {buffer[:30]}")
                words = struct.unpack("<12I", bytes.fromhex(buffer))
                header = words[0]
                limit = min(len(words), 1 + ((header >> 6) & 63) + (header & 63))
                entry = {
                    "ms": round(1000 * (time.monotonic() - start), 1),
                    "thread": thread,
                    "handle": int.from_bytes(bytes.fromhex(fields["0"]), "little"),
                    "pc": int.from_bytes(bytes.fromhex(fields["f"]), "little"),
                    "lr": int.from_bytes(bytes.fromhex(fields["e"]), "little"),
                    "words": [f"{word:08x}" for word in words[:limit]],
                }
                print(json.dumps(entry), file=output, flush=True)
                count += 1
        print("IPC entries:", count, flush=True)
    finally:
        try:
            if attached:
                if not stopped:
                    remote.sock.sendall(b"\x03")
                    stop = remote.receive()
                    stopped = stop.startswith(("T", "S"))
                if stopped:
                    try:
                        print("clear catch:", remote.command("QCatchSyscalls:0"), flush=True)
                    finally:
                        print("detach:", remote.command(f"D;{args.pid:x}" if "multiprocess+" in features else "D"), flush=True)
        finally:
            remote.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("ip")
    parser.add_argument("pid", type=int)
    parser.add_argument("output")
    parser.add_argument("--port", type=int, default=4000)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--limit", type=int, default=400)
    trace(parser.parse_args())
