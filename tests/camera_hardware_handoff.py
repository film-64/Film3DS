import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import bootstrap_camera_handoff as b
import export_camera_handoff as e
import verify_camera_handoff as v


class HandoffTests(unittest.TestCase):
    def fixture(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        name = 'handoff/camera-20261114/README.md'
        path = root / name
        path.parent.mkdir(parents=True)
        path.write_text('Observed evidence, not absolute calibration.\n')
        raw = path.read_bytes()
        data = dict(schema=v.CAMERA_HANDOFF_SOURCE, images_included=False, calibration_validated=False,
                    files=[dict(path=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())])
        (root / v.MANIFEST).write_text(json.dumps(data))
        return root, data

    def save(self, root, data):
        (root / v.MANIFEST).write_text(json.dumps(data))

    def test_positive_package_is_not_capture_or_calibration_proof(self):
        root, _ = self.fixture()
        result = v.verify(root)
        self.assertTrue(result['package_verified'])
        self.assertFalse(result['capture_reverified'])
        self.assertFalse(result['calibration_validated'])

    def test_unknown_source_and_fake_proof_rejected(self):
        for key, value in (('schema', 'unknown'), ('images_included', 'false'), ('calibration_validated', 'false')):
            root, data = self.fixture()
            data[key] = value
            self.save(root, data)
            with self.assertRaises(ValueError):
                v.verify(root)

    def test_tamper_missing_and_duplicate_rejected(self):
        for mode in ('tamper', 'missing', 'duplicate'):
            root, data = self.fixture()
            path = root / data['files'][0]['path']
            if mode == 'tamper': path.write_text('changed')
            elif mode == 'missing': path.unlink()
            else: data['files'].append(data['files'][0]); self.save(root, data)
            with self.assertRaises(ValueError):
                v.verify(root)

    def test_path_traversal_and_symlink_rejected(self):
        root, data = self.fixture()
        for name in ('../outside.txt', '/outside.txt', 'handoff/../outside.txt', 'bad\\path.txt'):
            with self.assertRaises(ValueError):
                v.safe_file(root, name)
        path = root / data['files'][0]['path']
        actual = root / 'actual.md'
        path.rename(actual)
        path.symlink_to(actual)
        with self.assertRaises(ValueError):
            v.verify(root)

    def test_image_binary_and_unlisted_file_rejected(self):
        root, _ = self.fixture()
        for name, content in (('photo.yuyv', b'pixels'), ('data.json', b'\0bad')):
            path = root / name
            path.write_bytes(content)
            with self.assertRaises(ValueError):
                v.safe_file(root, name)
        (root / 'handoff/camera-20261114/photo.png').write_bytes(b'pixels')
        with self.assertRaises(ValueError):
            v.verify(root)

    def test_export_omits_pixels_preserves_text_and_original(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / 'private'
            recovered = source / 'batch28-test/recovered'
            recovered.mkdir(parents=True)
            (recovered / 'reads.csv').write_text('addr,value\n3012,128\n')
            (recovered / 'photo.yuyv').write_bytes(b'private pixels')
            before = {x.name: x.read_bytes() for x in recovered.iterdir()}
            public = root / 'public'
            result = e.export(source, public)
            self.assertEqual(result['omitted_images'], 1)
            self.assertFalse(list(public.rglob('*.yuyv')))
            self.assertEqual((public / 'evidence/batch28-test/recovered/reads.csv').read_bytes(), before['reads.csv'])
            self.assertEqual({x.name: x.read_bytes() for x in recovered.iterdir()}, before)
            with self.assertRaises(FileExistsError):
                e.export(source, public)
            with self.assertRaises(ValueError):
                e.export(source, source / 'nested')
            with self.assertRaises(ValueError):
                e.export(source, root / 'unknown', 'unknown')

    def test_bootstrap_links_idempotently_without_device_work(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for target in b.CONTRACT_MATRIX[b.CAMERA_HANDOFF_BOOTSTRAP_SOURCE]['links'].values():
                (root / target).mkdir(parents=True)
            b.bootstrap(root)
            b.bootstrap(root)
            self.assertTrue((root / 'build/worktrees/camera-deep-probe').is_symlink())

    def test_bootstrap_conflict_or_missing_target_preserves_paths(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'diagnostics/camera-deep-probe').mkdir(parents=True)
            with self.assertRaises(ValueError):
                b.bootstrap(root)
            self.assertFalse((root / 'build').exists())
            (root / 'diagnostics/native-trace').mkdir()
            conflict = root / 'build/worktrees/camera-deep-probe'
            conflict.mkdir(parents=True)
            (conflict / 'keep').write_text('existing work')
            with self.assertRaises(ValueError):
                b.bootstrap(root)
            self.assertEqual((conflict / 'keep').read_text(), 'existing work')
            with self.assertRaises(ValueError):
                b.bootstrap(root, 'unknown')


if __name__ == '__main__':
    unittest.main()
