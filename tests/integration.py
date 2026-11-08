#!/usr/bin/env python3
from pathlib import Path
import io
import struct
import subprocess
import sys
import tempfile
import zlib

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from control import DEFAULT_BINARY, DeviceError, batch, mock
from fixtures import generate
from profile import encode


def verify_jpeg(path, size):
    with Image.open(path) as image:
        image.load()
        assert image.format == "JPEG" and image.size == size and image.mode == "RGB"
        assert not image.info.get("progressive")
        exif = image.getexif()
        assert exif[274] == 1 and exif[305] == "Film3DS"
        tags = exif.get_ifd(34665)
        assert tags[40961] == 1 and (tags[40962], tags[40963]) == size
        return image.copy()


def reject(client, command, expected):
    try:
        client.command(command)
    except DeviceError as error:
        assert error.status == expected
    else:
        raise AssertionError(command)


def run(*args, expected=0):
    result = subprocess.run([DEFAULT_BINARY, *args], text=True, capture_output=True)
    assert result.returncode == expected, result.stderr
    return result


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        luts = root / "luts"
        generate(luts)
        jpeg, source = root / "capture.jpg", root / "capture.f3s"
        with mock(jpeg, source) as client:
            assert client.command("STATUS") == "ready=0 captured=0 developed=0"
            reject(client, "CAPTURE", 6)
            assert client.command("ERROR") == "bad_state device=0x00000000"
            client.command("INIT YUYV")
            assert client.command("UI 3").split()[1] == "1"  # right = 3:2
            assert client.command("UI 4").split()[2] == "1"  # up = 1.5x
            client.command("CAPTURE")
            assert client.command("INFO") == "640 480 1 614400"
            assert client.command("READ 0 4") == "2a542a50"
            reject(client, "READ 614399 2", 1)
            reject(client, "READ 4294967296 2", 1)
            reject(client, "READ 0 257", 1)
            reject(client, "READ 0 2 extra", 1)
            reject(client, "DEVELOP LUT", 6)
            assert client.command("DEVELOP") == "960 640"
            assert int(client.command("TIME").split("=")[1]) > 0
            assert client.command("MEM") == "logical_buffer_bytes=2688000"
            client.command("SOURCE")
            client.command("SAVE")
            packet = source.read_bytes()
            assert packet[:4] == b"F3SR" and len(packet) == 304 + 614400
            assert packet[304:] == client.dump()
            assert struct.unpack_from("<I", packet, 300)[0] == zlib.crc32(packet[:300] + packet[304:])
            assert client.command("UI 130 215").split()[0] == "1"
            assert client.command("UI 130 170").split()[0] == "2"
        plain = verify_jpeg(jpeg, (960, 640))
        reproduced = root / "reproduced.jpg"
        run("develop", source, reproduced)
        assert jpeg.read_bytes() == reproduced.read_bytes()
        print("PASS mock protocol, full 614400-byte buffer, source CRC and byte-exact redevelopment")
        print("PASS baseline JPEG, Pillow decode, dimensions, orientation and Exif metadata")

        profile = root / "look.f3p"
        profile.write_bytes(encode({"input": "srgb", "output": "srgb"}))
        native_profile = root / "native.f3p"
        run("profile", native_profile)
        assert profile.read_bytes() == native_profile.read_bytes()
        lookup = {}
        for name in ("identity", "swap", "gray", "tone", "nonlinear"):
            target = root / f"{name}.jpg"
            run("develop", source, target, luts / f"{name}17.cube", profile)
            lookup[name] = verify_jpeg(target, (960, 640))
        assert lookup["identity"].tobytes() == plain.tobytes()
        assert lookup["swap"].getpixel((959, 0))[2] > lookup["swap"].getpixel((959, 0))[0]
        r, g, b = lookup["gray"].getpixel((0, 0))
        assert abs(r-g) <= 1 and abs(g-b) <= 1
        assert lookup["tone"].tobytes() != plain.tobytes()
        assert lookup["nonlinear"].tobytes() != plain.tobytes()
        with mock(root / "lut.jpg", root / "lut.f3s", luts / "swap17.cube", profile) as client:
            client.command("INIT")
            client.command("CAPTURE")
            assert client.command("DEVELOP PLAIN") == "640 480"
            assert client.command("DEVELOP LUT") == "640 480"
            client.command("SOURCE")
        assert b"swap17.cube\0" in (root / "lut.f3s").read_bytes()[:304]
        assert run("develop", root / "lut.f3s", root / "missing.jpg", expected=1).stderr.strip() == "bad_state"
        print("PASS 5 synthetic LUT looks, explicit LUT protocol, profile interop and source reuse")

        profile.write_bytes(encode({"input": "rec709", "output": "rec709", "strength": .6,
                                    "global": [0, -.5, .125], "zones": [[0, 0, 0]] * 12}))
        run("develop", source, reproduced, luts / "identity17.cube", profile)
        assert verify_jpeg(reproduced, (960, 640)).tobytes() != plain.tobytes()
        bad = root / "bad"
        for invalid in (packet[:-1], packet + b"X", packet[:-1] + bytes([packet[-1] ^ 1])):
            bad.write_bytes(invalid)
            assert run("develop", bad, reproduced, expected=1).stderr.strip() == "corrupt"
        bad.write_bytes(packet[:4] + b"\x02" + packet[5:])
        assert run("develop", bad, reproduced, expected=1).stderr.strip() == "unsupported"
        bad.write_bytes(profile.read_bytes()[:-1])
        assert run("develop", source, reproduced, luts / "identity17.cube", bad,
                   expected=1).stderr.strip() == "corrupt"
        assert len(batch(True, False).splitlines()) == 2412
        assert "INIT RGB565" in batch(False, True)
        print("PASS profile transfer and HSL, corrupt/truncated files, version and SD batch generation")
        assert Image.open(io.BytesIO(jpeg.read_bytes())).size == (960, 640)


if __name__ == "__main__":
    main()
