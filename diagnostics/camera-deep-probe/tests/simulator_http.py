#!/usr/bin/env python3
from io import BytesIO
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from threading import Thread
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from control import mock
from simulator import Simulator, make_server


def check(base):
    def get(path, event=None):
        data = json.dumps(event).encode() if event is not None else None
        request = Request(base + path, data)
        with urlopen(request, timeout=4) as response:
            return response.read()

    assert b"Film3DS" in get("/")
    with Image.open(BytesIO(get("/preview.jpg"))) as image:
        image.load()
        assert image.size == (640, 480)
    assert json.loads(get("/state"))["count"] == 3
    assert json.loads(get("/state", [3]))["aspect"] == 1
    assert json.loads(get("/state", [4]))["scale"] == 2
    captured = json.loads(get("/state", [160, 171]))
    assert (captured["count"], captured["selected"]) == (4, 3)
    assert json.loads(get("/state", [160, 220]))["page"] == 1
    assert json.loads(get("/state", [6]))["page"] == 2
    assert json.loads(get("/state", [3]))["look"] == 1
    assert json.loads(get("/state", [6]))["revision"] == 1
    with Image.open(BytesIO(get("/export.jpg"))) as image:
        image.load()
        assert image.size == (1278, 852)
        pixel = image.getpixel((400, 100))
        assert max(pixel) - min(pixel) <= 1
    try:
        get("/state", [320, 0])
    except HTTPError as error:
        assert error.code == 400
    else:
        raise AssertionError("out-of-range coordinates accepted")


def main():
    with TemporaryDirectory() as temp:
        jpeg, source = Path(temp) / "mock.jpg", Path(temp) / "mock.f3s"
        with mock(jpeg, source) as client:
            with make_server(0, Simulator(client, jpeg, source)) as server:
                thread = Thread(target=server.serve_forever)
                thread.start()
                try:
                    check(f"http://127.0.0.1:{server.server_port}")
                finally:
                    server.shutdown()
                    thread.join()
    print("PASS desktop dual-screen HTTP prototype, keyboard/touch state, roll and mono export")


if __name__ == "__main__":
    main()
