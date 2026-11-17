import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from raw_capture_qa import collect, prepare_receipt
from camera_runtime_contract import CONTRACT_MATRIX

TOKEN = "0123456789abcdef0123456789abcdef"


class RawQaTests(unittest.TestCase):
    def parse(self, payload):
        with tempfile.TemporaryDirectory() as out:
            return collect(io.BytesIO(payload), TOKEN, Path(out), io.StringIO())

    def test_failure_is_evidence_not_success(self):
        result = self.parse(f"F3RAWQA1 {TOKEN}\nRESULT status=bad_state raw_bytes=0\nDONE\n".encode())
        self.assertEqual(result["status"], "bad_state")
        self.assertIsNone(result["raw_sha256"])

    def test_success_requires_real_payload(self):
        head = f"F3RAWQA1 {TOKEN}\nRESULT status=ok raw_bytes=614400\n".encode()
        with self.assertRaises(ValueError):
            self.parse(head + b"DONE\n")
        raw = bytes((12, 1, 13, 2)) * 153600
        result = self.parse(head + b"RAW_BYTES 614400\n" + raw + b"\nDONE\n")
        self.assertEqual(len(result["raw_sha256"]), 64)
        for bad in (bytes((12, 4)) * 307200, bytes(614400), raw[:-1]):
            with self.assertRaises(ValueError):
                self.parse(head + b"RAW_BYTES 614400\n" + bad + b"\nDONE\n")

    def test_fake_proof_and_token_rejected(self):
        for payload in (b"F3RAWQA1 fake\nDONE\n", f"F3RAWQA1 {TOKEN}\nDONE\n".encode(),
                        f"F3RAWQA1 {TOKEN}\nRAW_BYTES 614400\n".encode()):
            with self.assertRaises(ValueError):
                self.parse(payload)

    def test_registered_single_production_attempt(self):
        rule = CONTRACT_MATRIX["film-raw-capture-automation/v1"]
        self.assertTrue(rule["compute"])
        self.assertFalse(rule["lane_admission"])
        self.assertIsNone(rule["override"])
        self.assertEqual(rule["capture_attempts"], 1)
        self.assertEqual(rule["retries"], 0)
        self.assertEqual(rule["downstream"], "film-camera-raw-capture/v1")
        transport=CONTRACT_MATRIX["film-online-evidence-send/v1"]
        self.assertFalse(transport["compute"])
        self.assertEqual(transport["max_send_bytes"],4096)

    def test_interrupted_raw_preserves_received_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)
            header=f"F3RAWQA1 {TOKEN}\nRESULT status=ok raw_bytes=614400\nRAW_BYTES 614400\n".encode()
            raw=bytes((12,1,13,2))*1536
            with self.assertRaises(ValueError):
                collect(io.BytesIO(header+raw),TOKEN,out,io.StringIO())
            self.assertEqual((out/"capture.bin.part").read_bytes(),raw)
            self.assertFalse((out/"capture.bin").exists())

    def test_no_hidden_relaunch_or_changed_build(self):
        with tempfile.TemporaryDirectory() as out:
            receipt = Path(out) / "rawqa-deployment.json"
            previous = dict(app_sha256="a", manifest_sha256="b", status="failed")
            receipt.write_text(json.dumps(previous))
            record = dict(app_sha256="a", manifest_sha256="b", status="prepared")
            for note, candidate in ((None, record), ("NetLoader ready", dict(record, app_sha256="different"))):
                with self.assertRaises(FileExistsError):
                    prepare_receipt(receipt, candidate, note)
            self.assertEqual(json.loads(receipt.read_text()), previous)
            prepare_receipt(receipt, record, "User: NetLoader restarted and ready")
            self.assertEqual(json.loads((Path(out) / "rawqa-failed-01.json").read_text()), previous)
            self.assertEqual(len(record["prior_failure_sha256"]), 64)
            with self.assertRaises(FileExistsError):
                prepare_receipt(receipt, record, "still ready")


if __name__ == "__main__":
    unittest.main()
