"""Verify host boundary against actual native C-driver fault fixtures.

No generated fixture is presented as a physical camera measurement.
"""
import csv
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import exposure_field_report as field


class FieldTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / "source"
        fixture = ROOT / "build/worktrees/camera-deep-probe/build/exposure-field-0"
        if not (fixture / "manifest.txt").exists():
            self.fail("Run tools/test_exposure_field_native.sh in the probe toolchain first")
        shutil.copytree(fixture, self.source)
        self.output = Path(self.temp.name) / "analysis"

    def mutate(self, filename, column, value, match=lambda row: True):
        path = self.source / filename
        with path.open() as file:
            reader = csv.DictReader(file)
            names, rows = reader.fieldnames, list(reader)
        for row in rows:
            if match(row):
                row[column] = value
                break
        with path.open("w") as file:
            writer = csv.DictWriter(file, fieldnames=names)
            writer.writeheader()
            writer.writerows(rows)

    def test_fixture_contract_passes_without_becoming_hardware_evidence(self):
        result = field.analyse(self.source, self.output, native_fixture=True)
        self.assertTrue(result["contract_proof_verified"])
        self.assertFalse(result["hardware_proof_verified"])
        self.assertTrue(result["no_model_refit_on_validation_sample"])

    def test_fixture_requires_explicit_mode(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            field.analyse(self.source, self.output)

    def test_prediction_cannot_be_replaced_by_reference(self):
        self.mutate("exposure.csv", "predicted_A_s", ".25", lambda r: r["slot"] == "1")
        with self.assertRaisesRegex(ValueError, "immutable"):
            field.analyse(self.source, self.output, native_fixture=True)

    def test_fake_dose_rejected_against_hardware_timing(self):
        self.mutate("exposure.csv", "dose", "1", lambda r: r["slot"] == "1")
        with self.assertRaisesRegex(ValueError, "timing/gain"):
            field.analyse(self.source, self.output, native_fixture=True)

    def test_unknown_source_fails_closed(self):
        path = self.source / "manifest.txt"
        path.write_text(path.read_text().replace("source=exposure-field", "source=unregistered"))
        with self.assertRaisesRegex(ValueError, "contract"):
            field.analyse(self.source, self.output, native_fixture=True)

    def test_isp_drift_rejects_proof(self):
        self.mutate("reads.csv", "value", "129", lambda r: r["phase"] == "4" and r["addr"] == "32d6" and r["when"] == "before" and r["frame"] == "8")
        result = field.analyse(self.source, self.output, native_fixture=True)
        self.assertFalse(result["contract_proof_verified"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
