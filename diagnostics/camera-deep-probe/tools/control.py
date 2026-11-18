#!/usr/bin/env python3
"""Film3DS protocol client, mock probe and SD batch generator."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = ROOT / "build/filmctl"


class DeviceError(RuntimeError):
    def __init__(self, status, message):
        self.status = status
        super().__init__(message)


class Client:
    def __init__(self, send, receive):
        self.send, self.receive = send, receive
        self.serial = 0

    def command(self, request):
        if not request or len(request) > 220 or "\n" in request or "\r" in request:
            raise ValueError("invalid request")
        self.serial += 1
        self.send.write(f"F3D1 {self.serial} {request}\n")
        self.send.flush()
        fields = self.receive.readline(768).strip().split(maxsplit=3)
        if len(fields) < 3 or fields[:2] != ["F3D1", str(self.serial)]:
            raise RuntimeError(f"invalid or missing reply for {request}: {fields}")
        status = int(fields[2])
        data = fields[3] if len(fields) == 4 else ""
        if status:
            raise DeviceError(status, data)
        return data

    def dump(self):
        _, _, _, size = map(int, self.command("INFO").split())
        if not 1 <= size <= 4096 * 4096 * 3:
            raise RuntimeError("invalid payload size")
        result = bytearray()
        for offset in range(0, size, 256):
            count = min(256, size - offset)
            data = bytes.fromhex(self.command(f"READ {offset} {count}"))
            if len(data) != count:
                raise RuntimeError("incorrect chunk size")
            result.extend(data)
        return result


@contextmanager
def mock(jpeg, source, *look, executable=DEFAULT_BINARY):
    with subprocess.Popen([executable, "mock", jpeg, source, *look], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, encoding="ascii") as process:
        try:
            yield Client(process.stdin, process.stdout)
        finally:
            process.stdin.close()
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
        if code:
            raise RuntimeError(f"mock process exited with {code}")


def batch(dump, rgb565):
    fmt = "RGB565" if rgb565 else "YUYV"
    commands = ["STATUS", f"INIT {fmt}", "CAPTURE", "TIME", "ERROR", "INFO", "SOURCE",
                "DEVELOP PLAIN", "TIME", "MEM", "SAVE", "ERROR"]
    if dump:
        commands.extend(f"READ {offset} 256" for offset in range(0, 640 * 480 * 2, 256))
    return "".join(f"F3D1 {i} {cmd}\n" for i, cmd in enumerate(commands, 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    probe = sub.add_parser("mock")
    probe.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    probe.add_argument("--dump", type=Path)
    sd = sub.add_parser("batch")
    sd.add_argument("--output", type=Path, required=True)
    sd.add_argument("--dump", action="store_true")
    sd.add_argument("--rgb565", action="store_true")
    args = parser.parse_args()
    if args.mode == "batch":
        args.output.write_text(batch(args.dump, args.rgb565), encoding="ascii")
    else:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            with mock(base / "debug.jpg", base / "debug.f3s", executable=args.binary) as client:
                for command in ("STATUS", "INIT", "CAPTURE", "INFO", "SOURCE", "DEVELOP", "TIME", "MEM", "SAVE"):
                    print(command, client.command(command))
                if args.dump:
                    args.dump.write_bytes(client.dump())


if __name__ == "__main__":
    main()
