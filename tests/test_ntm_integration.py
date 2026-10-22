#!/usr/bin/env python3
"""NTM-3D compatibility regressions: MVC input normalization, seek headers, trace."""
import pathlib
import re
import subprocess
import sys
import tempfile

EXE = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './x264').resolve())

def run(*args):
    p = subprocess.run([EXE, *map(str, args)], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=120)
    assert p.returncode == 0, p.stderr.decode(errors='replace')[-2200:]

def nals(path):
    chunks = re.split(b'\x00\x00\x00\x01|\x00\x00\x01', path.read_bytes())
    return [(x[0] & 31, x) for x in chunks if x]

with tempfile.TemporaryDirectory(prefix='mvc-ntm-') as tmp:
    d = pathlib.Path(tmp)
    width, height, frames = 128, 64, 12
    y = bytes((x * 3 + yy + 48) % 256 for yy in range(height) for x in range(width))
    u = bytes((x * 2 + 90) % 256 for yy in range(height//2) for x in range(width//2))
    v = bytes((yy * 2 + 150) % 256 for yy in range(height//2) for x in range(width//2))
    interleaved = bytes(t for a, b in zip(u, v) for t in (a,b))
    (d/'packed.nv12').write_bytes((y + interleaved) * frames)
    (d/'planar.i420').write_bytes((y + u + v) * frames)
    flags = ['--mvc', '--mvc-layout','sbs', '--demuxer','raw',
             '--input-res',f'{width}x{height}', '--fps','24', '--frames',str(frames),
             '--keyint','4','--min-keyint','4','--bframes','0',
             '--preset','ultrafast','--threads','1','--aud','--crf','28']
    nv12, planar = d/'nv12.mvc', d/'i420.mvc'
    probe = subprocess.run([EXE, *map(str, flags), '--input-csp', 'nv12',
                            '-o', str(nv12), str(d/'packed.nv12')],
                           capture_output=True, timeout=120)
    run(*flags, '--input-csp','i420', '-o',planar, d/'planar.i420')
    if probe.returncode == 0:
        assert nv12.read_bytes() == planar.read_bytes(), 'NV12/I420 conversion changes encoded samples'
    else:
        assert b'not compiled with swscale support' in probe.stderr, probe.stderr[-1200:]
        print('SKIP: NV12 integration encode needs libswscale at build time')
    ns = nals(planar)
    assert sum(t == 7 for t, _ in ns) >= 3, 'SPS not repeated at keyframes'
    assert sum(t == 15 for t, _ in ns) >= 3, 'MVC subset SPS not repeated at keyframes'
    assert sum(t == 8 for t, _ in ns) >= 6, 'PPS not repeated for both views'
    base, dep, trace = d/'base.264', d/'dependent.mvc', d/'trace.csv'
    run(*flags, '--input-csp','i420', '--mvc-rc-trace',trace,
        '--mvc-combined-vbv-maxrate','1200', '--mvc-combined-vbv-bufsize','1600',
        '-o',base,'--mvc-dependent',dep,d/'planar.i420')
    dn = nals(dep)
    assert not any(t in (6,9) for t,_ in dn), 'Dependent view emitted redundant SEI/AUD'
    lines = trace.read_text().splitlines()
    assert lines[0] == 'pair,base_bytes,dependent_bytes,total_bytes,cpb_fill_kbit'
    assert len(lines) == frames + 1, 'Trace lost pair rows'
    assert all(int(row.split(',')[1])+int(row.split(',')[2])==int(row.split(',')[3])
               for row in lines[1:]), 'Trace size mismatch'
    # Ensure potentially destructive CSV/output file alias is rejected.
    wrong = subprocess.run([EXE, *map(str, flags), '--input-csp','i420',
        '--mvc-rc-trace',str(base), '-o',str(base),str(d/'planar.i420')],
        capture_output=True,timeout=120)
    assert wrong.returncode != 0, 'Trace can overwrite video output'
print('NTM compatibility tests passed')
