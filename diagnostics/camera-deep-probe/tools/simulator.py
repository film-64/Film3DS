#!/usr/bin/env python3
"""Local desktop dual-screen simulator using the Film3DS C state machine."""
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import tempfile

from control import DEFAULT_BINARY, DeviceError, mock

UI_FILE = Path(__file__).with_name("simulator.html")


class Simulator:
    def __init__(self, client, jpeg, source):
        self.client = client
        self.jpeg = jpeg
        self.source = source
        self.roll = [None] * 3
        self.message = "虚拟胶卷已装载"
        self.revision = 0
        client.command("INIT")
        client.command("CAPTURE")
        client.command("DEVELOP PLAIN")
        client.command("SAVE")
        self.preview = jpeg.read_bytes()

    def update(self, event=""):
        result = list(map(int, self.client.command("UI" + event).split()))
        page, aspect, scale, selected, count, look, strength, action = result
        try:
            if action == 1:
                self.client.command("CAPTURE")
                self.client.command("SOURCE")
                self.roll.append(self.source.read_bytes())
                self.client.command("UI ADD")
                self.message = "源图已记录到模拟胶卷"
                return self.update()
            if action == 2:
                if look == 3:
                    raise ValueError("SD LUT 通过 3DS 或 filmctl 导入")
                saved = self.roll[selected]
                if saved is None:
                    self.client.command("CAPTURE")
                else:
                    self.source.write_bytes(saved)
                    self.client.command("LOAD")
                self.client.command("DEVELOP " + ("PLAIN", "MONO", "WARM")[look])
                self.client.command("SAVE")
                self.revision += 1
                self.message = "JPEG 已生成"
        except (DeviceError, ValueError) as exc:
            self.message = str(exc)
        return dict(page=page, aspect=aspect, scale=scale, selected=selected, count=count,
                    look=look, strength=strength, status=self.message, revision=self.revision)


def make_server(port, sim):
    class Handler(BaseHTTPRequestHandler):
        def reply(self, status, kind, data):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = self.path.partition("?")[0]
            if path == "/":
                self.reply(200, "text/html; charset=utf-8", UI_FILE.read_bytes())
            elif path == "/preview.jpg":
                self.reply(200, "image/jpeg", sim.preview)
            elif path == "/export.jpg" and sim.revision:
                self.reply(200, "image/jpeg", sim.jpeg.read_bytes())
            elif path == "/state":
                self.state()
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path != "/state":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 1 <= length <= 128:
                    raise ValueError
                values = json.loads(self.rfile.read(length))
                if not isinstance(values, list) or not all(type(v) is int for v in values):
                    raise ValueError
                if len(values) == 1 and 0 <= values[0] <= 6:
                    event = f" {values[0]}"
                elif len(values) == 2 and 0 <= values[0] < 320 and 0 <= values[1] < 240:
                    event = f" {values[0]} {values[1]}"
                else:
                    raise ValueError
            except (ValueError, TypeError):
                self.send_error(400)
                return
            self.state(event)

        def state(self, event=""):
            data = json.dumps(sim.update(event), ensure_ascii=False).encode()
            self.reply(200, "application/json", data)

    return HTTPServer(("127.0.0.1", port), Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        jpeg, source = (Path(directory) / name for name in ("mock.jpg", "mock.f3s"))
        with mock(jpeg, source, executable=args.binary) as client:
            with make_server(args.port, Simulator(client, jpeg, source)) as server:
                print(f"http://127.0.0.1:{server.server_port}/", flush=True)
                try:
                    server.serve_forever()
                except KeyboardInterrupt:
                    pass


if __name__ == "__main__":
    main()
