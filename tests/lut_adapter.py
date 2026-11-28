"""Exercise independent Python CRC binding and the real host develop path."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from control import mock
from lut_adapter import create
from profile import encode
from camera_ui15_package import stage


class AdapterTests(unittest.TestCase):
    def test_sidecar_real_render_and_failure_preserves_originals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cube = root / "look.cube"
            cube_bytes = ("LUT_3D_SIZE 2\r\n" + "\r\n".join(
                f"{r} {g} {b}" for b in range(2) for g in range(2) for r in range(2))).encode()
            cube.write_bytes(cube_bytes)
            reference = root / "reference.cube"
            reference.write_bytes(cube_bytes)
            sidecar = create(cube, "LINEAR_SRGB", "SRGB", "preserve")
            self.assertEqual(cube.read_bytes(), cube_bytes)
            metadata = sidecar.read_bytes()
            with self.assertRaises(FileExistsError):
                create(cube, "SRGB", "SRGB", "bypass")
            self.assertEqual(sidecar.read_bytes(), metadata)
            default = root / "default.f3p"
            default.write_bytes(encode(dict(input="srgb", output="srgb")))
            matching = root / "matching.f3p"
            matching.write_bytes(encode(dict(input="linear-srgb", output="srgb")))
            source, original_jpeg = root / "source.f3s", root / "original.jpg"
            with mock(original_jpeg, source) as client:
                client.command("INIT")
                client.command("CAPTURE")
                client.command("SOURCE")
                client.command("DEVELOP")
                client.command("SAVE")
            original = source.read_bytes()
            jpeg = original_jpeg.read_bytes()

            def render(name, lut, profile):
                out = root / name
                result = subprocess.run([str(ROOT / "build/filmctl"), "develop", str(source),
                                         str(out), str(lut), str(profile)], capture_output=True, text=True)
                return result, out

            result, adapted = render("adapted.jpg", cube, default)
            self.assertEqual(result.returncode, 0, result.stderr)
            result, expected = render("expected.jpg", reference, matching)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(adapted.read_bytes(), expected.read_bytes())
            result, unadapted = render("unadapted.jpg", reference, default)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotEqual(adapted.read_bytes(), unadapted.read_bytes())
            # Alter one valid sample without changing length. Stale metadata fails.
            cube.write_bytes(cube_bytes.replace(b"1 1 1", b"0 1 1"))
            result, failed = render("failed.jpg", cube, default)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(failed.exists())
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(original_jpeg.read_bytes(), jpeg)

    def test_packaging_never_silently_drops_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            luts = root / "luts"
            luts.mkdir()
            cube = luts / "test.cube"
            cube.write_text("LUT_3D_SIZE 2\n")
            create(cube, "FLOG2C", "SRGB", "bypass")
            out = root / "package"
            with self.assertRaisesRegex(ValueError, "SD-only"):
                stage(out, luts)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
