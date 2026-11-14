"""Quiet session integration: real HTTP range recovery; only device deployment is stubbed."""
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import exposure_sweep_session as session


class SessionTests(unittest.TestCase):
    def exercise(self, invalid=False, awb_refresh=False, post_refresh_manual=False,isp_update=False,no_receive=False,writer_window=False,hg_isolation=False,ccm_normalization=False,gain_matrix=False,exposure_rebuild=False,exposure_dense=False):
        payload=b'original evidence\n'*2000
        crc=f'{zlib.crc32(payload):08x}'
        requests=[]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_GET(self):
                requests.append(self.path)
                if self.path=='/info':
                    body=(f'FILMRAM {4 if invalid else 3}\nrun ram:/test\ncomplete 0\nfailed 0\ncount 1\n0 reads.csv {len(payload)} {crc}\n').encode()
                    self.send_response(200)
                else:
                    start,end=map(int,self.headers['Range'][6:].split('-'))
                    body=payload[start:end+1]
                    self.send_response(206)
                    self.send_header('Content-Range',f'bytes {start}-{end}/{len(payload)}')
                    self.send_header('ETag',f'"{crc}"')
                self.send_header('Content-Length',str(len(body)))
                self.end_headers();self.wfile.write(body)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        connection=session.http.client.HTTPConnection
        def connect(host,port,timeout):
            return connection('127.0.0.1',server.server_port,timeout=timeout)
        deployments=[]
        def deploy(ip,binary,args):
            deployments.append(args)
            print('NETWORK_DETAIL\n'*2000)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);probe=root/'probe'
                (probe/'probes/exposure').mkdir(parents=True)
                (probe/'FilmExposureSweep.3dsx').write_bytes(b'explicit test binary')
                (probe/'probes/exposure/source.c').write_text('test source')
                out=root/'result';console=io.StringIO()
                with patch.object(session,'PROBE',probe),patch.object(session,'deploy',deploy),patch.object(session.http.client,'HTTPConnection',connect),contextlib.redirect_stdout(console):
                    if invalid:
                        with self.assertRaises(ValueError):session.run('test-device',out,None,check=True)
                    else:session.run('test-device',out,None,check=not (awb_refresh or post_refresh_manual or isp_update or no_receive or writer_window or hg_isolation or ccm_normalization or gain_matrix or exposure_rebuild or exposure_dense),awb_refresh=awb_refresh,post_refresh_manual=post_refresh_manual,isp_update=isp_update,no_receive=no_receive,writer_window=writer_window,hg_isolation=hg_isolation,ccm_normalization=ccm_normalization,gain_matrix=gain_matrix,exposure_rebuild=exposure_rebuild,exposure_dense=exposure_dense)
                lines=console.getvalue().splitlines()
                self.assertLess(len(console.getvalue()),1600)
                self.assertNotIn('NETWORK_DETAIL',console.getvalue())
                self.assertEqual(len(deployments),1)
                self.assertIn('--calibration-part='+('camera-exposure-dense-v1' if exposure_dense else 'camera-exposure-rebuild-v2' if exposure_rebuild else 'camera-gain-matrix-v4' if gain_matrix else 'camera-ccm-normalization-v3' if ccm_normalization else 'camera-hg-isolation' if hg_isolation else 'camera-writer-window-v2' if writer_window else 'no-receive-frame-update' if no_receive else 'exposure-isp-update' if isp_update else 'exposure-sweep-post-refresh-manual' if post_refresh_manual else 'exposure-sweep-awb-refresh' if awb_refresh else 'exposure-sweep-check'),deployments[0])
                self.assertIn('NETWORK_DETAIL',(out/'session.log').read_text())
                status=json.loads((out/'session-status.json').read_text())
                self.assertFalse((out/'session-status.json.tmp').exists())
                if invalid:
                    self.assertEqual(status['phase'],'failed')
                    self.assertFalse((out/'recovered').exists())
                    self.assertIn('unexpected recovery identity',(out/'session.log').read_text())
                else:
                    self.assertEqual([json.loads(x)['phase'] for x in lines],['deploying','capturing','recovering','recovered'])
                    self.assertFalse(status['capture_complete'])
                    self.assertFalse(status['calibration_validated'])
                    self.assertEqual((out/'recovered/reads.csv').read_bytes(),payload)
                    receipt=json.loads((out/'recovered/received-ram-sha256.json').read_text())[0]
                    self.assertEqual(receipt['sha256'],hashlib.sha256(payload).hexdigest())
                    self.assertGreater(requests.count('/file/0'),1)
        finally:
            server.shutdown();server.server_close();thread.join()

    def test_dense_is_one_registered_deployment(self):self.exercise(exposure_dense=True)
    def test_rebuild_is_one_registered_deployment(self):self.exercise(exposure_rebuild=True)
    def test_chunked_recovery_stays_on_disk_and_incomplete_stays_incomplete(self):self.exercise()
    def test_unknown_protocol_fails_once_with_details_only_on_disk(self):self.exercise(invalid=True)
    def test_refresh_is_one_explicit_deployment_and_cannot_masquerade_as_calibration(self):self.exercise(awb_refresh=True)
    def test_mixed_diagnostic_scopes_are_rejected_before_deployment(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,check=True,awb_refresh=True)
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,post_refresh_manual=True,awb_refresh=True)
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,isp_update=True,post_refresh_manual=True)
    def test_post_refresh_manual_is_one_explicit_deployment(self):self.exercise(post_refresh_manual=True)
    def test_ISP_update_is_one_explicit_deployment(self):self.exercise(isp_update=True)
    def test_no_receive_is_one_explicit_deployment(self):self.exercise(no_receive=True)
    def test_writer_window_is_single_explicit_deployment(self):self.exercise(writer_window=True)
    def test_writer_window_cannot_mix_scopes(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,writer_window=True,no_receive=True)
    def test_matrix_single_deployment(self):self.exercise(gain_matrix=True)
    def test_matrix_mixed_source_rejected(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,gain_matrix=True,ccm_normalization=True)
    def test_CCM_single_deployment(self):self.exercise(ccm_normalization=True)
    def test_CCM_mixed_scope_rejected(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,ccm_normalization=True,hg_isolation=True)
    def test_HG_single_deployment(self):self.exercise(hg_isolation=True)
    def test_HG_mixed_scope_rejected(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,hg_isolation=True,writer_window=True)
    def test_no_receive_cannot_mix_scopes(self):
        with self.assertRaises(ValueError):session.run('test-device',Path('/unused'),None,no_receive=True,isp_update=True)


if __name__=='__main__':unittest.main()
