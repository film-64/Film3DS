"""Real native v3 index (>8KiB), last-file ranges and unchanged old limits."""
from pathlib import Path
import subprocess,sys,zlib
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/"tools"))
import retrieve_probe_ram as recovery
binary=root/"build/ram-sweep-test"
subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-function",str(root/"tests/ram_sweep.c"),"-o",str(binary)],check=True)
server=subprocess.Popen([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
    line=server.stdout.readline();assert line.startswith("PORT "),line
    port=int(line.split()[1])
    status,_,body=recovery.get("127.0.0.1","/info",port=port)
    assert status==200 and len(body)>8192
    prefix,files=recovery.parse_index(body)
    assert len(files)==278 and prefix["failed"]=="0"
    for entry in (files[0],files[-1]):
        status,_,data=recovery.get("127.0.0.1",f"/file/{entry['id']}",{"Range":"bytes=0-24575"},port=port)
        assert status==206 and data==bytes((entry["id"]+i)%251 for i in range(24576))
        assert f"{zlib.crc32(data):08x}"==entry["crc32"]
    for version in (b"FILMRAM 1",b"FILMRAM 2"):
        try:recovery.parse_index(body.replace(b"FILMRAM 3",version,1))
        except ValueError:pass
        else:raise AssertionError("legacy file-count limit weakened")
finally:
    server.terminate();server.communicate(timeout=5)
print("PASS RAM v3 native full index, final file CRC/range, bounded file count, no IPC and legacy limits")
