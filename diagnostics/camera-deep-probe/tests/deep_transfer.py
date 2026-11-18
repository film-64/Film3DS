#!/usr/bin/env python3
"""Exercise the actual evidence receiver with complete and interrupted payloads."""
import importlib.util,json,socket,tempfile,threading
from pathlib import Path
spec=importlib.util.spec_from_file_location('deep_run',Path(__file__).resolve().parents[1]/'tools/run_deep_probe.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
def trial(root,payload,expect_ack=False):
    left,right=socket.socketpair();left.settimeout(3);right.settimeout(3)
    def sender():
        try:
            right.sendall(payload);right.shutdown(socket.SHUT_WR)
            if expect_ack:assert right.recv(1)==b'K'
        finally:right.close()
    thread=threading.Thread(target=sender);thread.start()
    try:return module.receive(left,root)
    finally:left.close();thread.join(timeout=4);assert not thread.is_alive()
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    assert trial(root,b'FILMPROBE_FILES 2 drivers\nFILE camera.bin 4\nABCDDONE\n',True)=='drivers'
    assert (root/'drivers/camera.bin').read_bytes()==b'ABCD'
    assert json.loads((root/'drivers/received-sha256.json').read_text())[0]['bytes']==4
    try:trial(root,b'FILMPROBE_FILES 2 sensor-1\nFILE frames.csv 8\nabc')
    except EOFError:pass
    else:raise AssertionError('partial transfer accepted')
    assert not (root/'sensor-1/received-sha256.json').exists()
    try:trial(root,b'FILMPROBE_FILES 2 sensor-2\nFILE ../escape 0\n')
    except ValueError:pass
    else:raise AssertionError('path traversal accepted')
    assert not (root/'escape').exists()
print('PASS actual evidence receiver: acknowledged bytes, truncated payload and invalid path')
