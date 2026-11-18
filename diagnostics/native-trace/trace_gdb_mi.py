import argparse
import json
import os
import re
import selectors
import signal
import struct
import subprocess
import time

class Debugger:
    def __init__(self, executable):
        self.proc = subprocess.Popen(
            [executable, "-nx", "-q", "--interpreter=mi2"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.poll = selectors.DefaultSelector()
        self.poll.register(self.proc.stdout, selectors.EVENT_READ)
        self.buffer = bytearray()
        self.token = 0

    def line(self, end):
        while b"\n" not in self.buffer:
            if not self.poll.select(max(0, end - time.monotonic())):
                raise TimeoutError("GDB did not respond")
            incoming = os.read(self.proc.stdout.fileno(), 16384)
            if not incoming:
                raise EOFError("GDB exited")
            self.buffer.extend(incoming)
        result, _, self.buffer = self.buffer.partition(b"\n")
        return result.rstrip(b"\r").decode(errors="replace")

    def command(self, command, timeout=6):
        self.token += 1
        self.proc.stdin.write(f"{self.token}{command}\n".encode())
        self.proc.stdin.flush()
        end = time.monotonic() + timeout
        output = []
        while True:
            line = self.line(end)
            output.append(line)
            if line.startswith(f"{self.token}^"):
                if line.startswith(f"{self.token}^error"):
                    raise RuntimeError(line)
                return output

    def console(self, command):
        return self.command("-interpreter-exec console " + json.dumps(command))

    def stop(self, output, timeout=4):
        for line in output:
            if line.startswith("*stopped"):
                return line
        end = time.monotonic() + timeout
        while True:
            line = self.line(end)
            if line.startswith("*stopped"):
                return line

    def close(self):
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        self.poll.close()

def trace(args):
    gdb = Debugger(args.gdb)
    attached, running = False, False
    tls_cache = {}
    try:
        for setting in ("tcp connect-timeout 4", "remotetimeout 4", "pagination off",
                        "confirm off", "sysroot /nonexistent", "mi-async on"):
            gdb.console("set " + setting)
        gdb.command(f"-target-select extended-remote {args.ip}:{args.port}", 9)
        attach = gdb.command(f"-target-attach {args.pid}", 9)
        attached = True
        print("attach:", gdb.stop(attach), flush=True)
        catch = gdb.console("catch syscall SendSyncRequest")
        if not any("Catchpoint" in line or "bkpt=" in line for line in catch):
            raise RuntimeError(f"Catchpoint was not created: {catch}")
        deadline = time.monotonic() + args.seconds
        count = 0
        with open(args.output, "w", encoding="utf-8") as records:
            while time.monotonic() < deadline and count < args.limit:
                running = True
                continued = gdb.command("-exec-continue")
                stop = gdb.stop(continued)
                running = False
                if 'reason="syscall-return"' in stop:
                    continue
                if 'reason="syscall-entry"' not in stop:
                    raise RuntimeError(f"Unexpected GDB stop: {stop}")
                thread = re.search(r'thread-id="(\d+)"', stop).group(1)
                if thread not in tls_cache:
                    info = " ".join(gdb.command(f"-thread-info {thread}"))
                    tls = re.search(r"TLS: 0x([0-9a-fA-F]+)", info)
                    if not tls:
                        raise RuntimeError(f"Missing TLS information for GDB thread {thread}")
                    tls_cache[thread] = int(tls.group(1), 16) + 0x80
                info = " ".join(gdb.command(f"-data-list-register-values --thread {thread} x 0 14 15"))
                regs = dict(re.findall(r'number="(\d+)",value="(0x[0-9a-f]+)"', info))
                mem = " ".join(gdb.command(f"-data-read-memory-bytes 0x{tls_cache[thread]:x} 48"))
                words = struct.unpack("<12I", bytes.fromhex(re.search(r'contents="([0-9a-f]+)"', mem).group(1)))
                header = words[0]
                nwords = min(12, 1 + ((header >> 6) & 63) + (header & 63))
                record = {
                    "thread": thread, "handle": int(regs["0"], 16),
                    "lr": int(regs["14"], 16), "pc": int(regs["15"], 16),
                    "words": [f"{word:08x}" for word in words[:nwords]],
                }
                print(json.dumps(record), file=records, flush=True)
                count += 1
        print("IPC entries:", count, flush=True)
    finally:
        try:
            if attached:
                if running:
                    interrupted = gdb.command("-exec-interrupt", 4)
                    gdb.stop(interrupted, 4)
                try:
                    gdb.console("delete breakpoints")
                finally:
                    print("detach:", gdb.command("-target-detach")[-1], flush=True)
        finally:
            try:
                gdb.command("-gdb-exit", 3)
            finally:
                gdb.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("gdb")
    parser.add_argument("ip")
    parser.add_argument("pid", type=int)
    parser.add_argument("output")
    parser.add_argument("--port", type=int, default=4000)
    parser.add_argument("--seconds", type=float, default=2)
    parser.add_argument("--limit", type=int, default=8)
    trace(parser.parse_args())
