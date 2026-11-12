"""Exercise the real bounded storage diagnostic on files, including failed writes and bad readback."""
import csv,hashlib,os,subprocess,tempfile,zlib
from pathlib import Path
root=Path(__file__).parents[1]
with tempfile.TemporaryDirectory() as temporary:
    base=Path(temporary);card=base/'card';card.mkdir();marker=card/'original.txt';marker.write_bytes(b'original data')
    for label,flag,expected in [('pass',None,0),('write-failure','STORAGE_INJECT_WRITE_FAILURE=13',1),('corruption','STORAGE_INJECT_CORRUPTION=1',1)]:
        binary=base/label
        command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror']
        if flag:command+=['-D'+flag]
        subprocess.run(command+[str(root/'probes/storage/main.c'),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary),str(card)],capture_output=True,text=True)
        assert result.returncode==expected,(label,result.stdout,result.stderr)
        assert marker.read_bytes()==b'original data'
        if expected==0:
            directory=next((card/'storage-checks').iterdir())
            logs=list(csv.DictReader((directory/'result.csv').open()))
            crc_rows=[r for r in logs if r['operation']=='readback_crc'];assert len(crc_rows)==10
            for row in crc_rows:
                data=(directory/f'test-{int(row["file"]):02}.bin').read_bytes()
                assert len(data)==64*4096 and f'{zlib.crc32(data):08x}'==row['crc32']
        else:
            assert 'STORAGE CHECK FAIL' in result.stdout and 'DATA_CHECK_PASS' not in result.stdout
            assert ('write file=' in result.stdout if label=='write-failure' else 'crc_mismatch' in result.stdout)
    assert len(list((card/'storage-checks').iterdir()))==3
print('Storage diagnostic passed: 10 real-file CRCs, unique directories, originals preserved, write failure and readback corruption rejected')
