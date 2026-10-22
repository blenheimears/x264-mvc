#!/usr/bin/env python3
"""x264 stock-CLI MVC regressions. Python stdlib only.

An optional STOCK_X264 environment variable enables byte-identical 2D
regressions against a separately built upstream binary.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

EXE = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './x264').resolve())

def invoke(*args, ok=True):
    p = subprocess.run([EXE, *map(str, args)], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=90)
    if (p.returncode == 0) != ok:
        raise AssertionError(f'{args}: unexpected exit {p.returncode}\n{p.stderr.decode(errors="replace")[-1500:]}')
    return p

def make_y4m(path, w, h, count, fps=24, shift=0):
    with open(path, 'wb') as f:
        f.write(f'YUV4MPEG2 W{w} H{h} F{fps}:1 Ip A1:1 C420jpeg\n'.encode())
        for n in range(count):
            f.write(b'FRAME\n')
            f.write(bytes((x*3+y*2+n*9+shift) % 256 for y in range(h) for x in range(w)))
            f.write(bytes((85+n+x//4+shift) % 256 for y in range(h//2) for x in range(w//2)))
            f.write(bytes((190+n+y//4+shift) % 256 for y in range(h//2) for x in range(w//2)))

def nal_units(data):
    units = []
    for chunk in re.split(b'\x00\x00\x00\x01|\x00\x00\x01', data):
        if chunk: units.append(chunk.rstrip(b'\0'))
    return units

def check_mvc(path):
    nals = nal_units(path.read_bytes())
    types = {nal[0] & 31 for nal in nals}
    assert {7, 8, 15, 20}.issubset(types), f'bad MVC NALs: {types}'
    assert sum((nal[0] & 31)==20 for nal in nals) == 12
    assert sum((nal[0] & 31) in (1,5) for nal in nals) == 12

with tempfile.TemporaryDirectory(prefix='x264-mvc-cli-') as work:
    d = pathlib.Path(work)
    sbs, tb, alternating, left, right = [d/x for x in ('sbs.y4m','tb.y4m','alt.y4m','left.y4m','right.y4m')]
    make_y4m(sbs, 128, 64, 12)
    make_y4m(tb, 64, 128, 12)
    make_y4m(alternating, 64, 64, 24, fps=48)
    make_y4m(left, 64, 64, 12)
    make_y4m(right, 64, 64, 12, shift=11)
    assert b'Syntax: x264' in invoke('--help').stdout
    assert b'x264' in invoke('--version').stdout

    # No --mvc: stock CLI output, filters, presets and option parser.
    output2d = d/'2d.264'
    arguments = ['--preset','veryfast','--crf','23','--bframes','2','--keyint','8','-o',output2d,left]
    invoke(*arguments)
    if os.environ.get('STOCK_X264'):
        stock = d/'stock.264'
        p = subprocess.run([os.environ['STOCK_X264'],'--preset','veryfast','--crf','23','--bframes','2','--keyint','8','-o',str(stock),str(left)],
                           capture_output=True)
        assert p.returncode == 0, p.stderr[-500:]
        assert output2d.read_bytes() == stock.read_bytes(), '2D output differs from upstream x264'

    for layout, inp, extra in [('sbs',sbs,[]), ('tb',tb,[]),
                               ('frames',alternating,[]), ('separate',left,['--mvc-right',right])]:
        name = d/f'{layout}.264'
        invoke('--mvc', '--mvc-layout',layout, *extra,
               '--preset','veryfast', '--crf','25', '--bframes','2',
               '--keyint','6', '--frames','12', '-o',name,inp)
        check_mvc(name)
        decoder = os.environ.get('MVC_DECODER')
        if decoder:
            decoded = subprocess.run([decoder, str(name), '-O'], capture_output=True, timeout=90)
            assert decoded.returncode == 0, decoded.stderr[-500:]
            assert decoded.stdout.startswith(b'YUV4MPEG2 '), 'decoder did not emit Y4M'
            assert decoded.stdout.count(b'FRAME\n') == 12, 'stereo decoder lost frames'
        # Split output: base plus dependent must reconstruct the combined NAL order.
        bfile, dfile = d/f'{layout}-base.264',d/f'{layout}-dep.mvc'
        invoke('--mvc','--mvc-layout',layout,*extra,
               '--preset','veryfast','--crf','25','--bframes','2',
               '--keyint','6','--frames','12','-o',bfile,'--mvc-dependent',dfile,inp)
        combined = nal_units(name.read_bytes())
        base_nals = nal_units(bfile.read_bytes())
        dep_nals = nal_units(dfile.read_bytes())
        base_from_combined = [n for n in combined if n[0]&31 in (1,5,7,8,6,9) and n in base_nals]
        assert base_nals == base_from_combined, 'base view not preserved'
        assert any((n[0]&31)==20 for n in dep_nals)

    # Each switch must work separately, and combinations must be accepted.
    for mask in range(8):
        name = d/f'mask{mask}.264'
        controls = ['--mvc-no-inter'] + [s for bit,s in [(1,'--mvc-inter-i'), (2,'--mvc-inter-p'), (4,'--mvc-inter-b')] if mask&bit]
        invoke('--mvc',*controls,'--preset','veryfast','--bframes','2',
               '--frames','12','-o',name,sbs)
        check_mvc(name)
        decoder = os.environ.get('MVC_DECODER')
        if decoder:
            decoded = subprocess.run([decoder, str(name), '-O'], capture_output=True, timeout=90)
            assert decoded.returncode == 0, decoded.stderr[-500:]
            assert decoded.stdout.count(b'FRAME\n') == 12, 'prediction switches changed decoded frame count'

    # Different per-eye rate controls must preserve MVC NAL structure and 2D base bytes.
    baseline_base = d/'baseline-base.264'
    baseline_dep = d/'baseline-dependent.mvc'
    tuned_base = d/'tuned-base.264'
    tuned_dep = d/'tuned-dependent.mvc'
    common = ['--mvc','--preset','veryfast','--bitrate','400','--vbv-maxrate','600',
              '--vbv-bufsize','800','--frames','12','--keyint','6','--bframes','2']
    invoke(*common,'--mvc-dependent-bitrate','400','--mvc-dependent-vbv-maxrate','600',
           '--mvc-dependent-vbv-bufsize','800','-o',baseline_base,'--mvc-dependent',baseline_dep,sbs)
    invoke(*common,'--mvc-dependent-bitrate','120','--mvc-dependent-vbv-maxrate','240',
           '--mvc-dependent-vbv-bufsize','320','-o',tuned_base,'--mvc-dependent',tuned_dep,sbs)
    assert tuned_base.read_bytes() == baseline_base.read_bytes(), 'dependent RC altered AVC base'
    assert tuned_dep.read_bytes() != baseline_dep.read_bytes(), 'dependent rate overrides ignored'
    for flag in ('--mvc-dependent-bitrate','--mvc-dependent-vbv-maxrate','--mvc-dependent-vbv-bufsize'):
        invoke('--mvc',flag,'0','-o',d/'bad-rate.264',sbs,ok=False)
        invoke('--mvc',flag,'bad','-o',d/'bad-rate.264',sbs,ok=False)
        invoke(flag,'100','-o',d/'bad-2d.264',sbs,ok=False)
        invoke('--mvc-no-inter-i','-o',d/'bad3.264',sbs,ok=False)
    invoke('--mvc','--mvc-layout','bogus','-o',d/'bad.264',sbs,ok=False)
    invoke('--mvc','--mvc-layout','separate','-o',d/'bad2.264',left,ok=False)
    invoke('--mvc','--mvc-layout','sbs','--mvc-dependent',d/'bad.mvc',
           '-o',d/'bad.mkv',sbs,ok=False)
    long_sbs = d/'long.y4m'
    make_y4m(long_sbs, 128, 64, 48)
    # Combined rate control: bitrate tolerance, VBV never exceeded, per-view split not fixed.
    trace = d/'rc.csv'
    invoke('--mvc','--preset','veryfast','--bitrate','300','--vbv-maxrate','600','--vbv-bufsize','600',
           '--frames','48','--keyint','12','--bframes','2','--mvc-rc-trace',trace,'-o',d/'rc.264',long_sbs)
    rows = [l.split(',') for l in trace.read_text().splitlines()[1:]]
    assert len(rows) == 48, 'trace row per access unit'
    assert min(float(r[5]) for r in rows) >= 0, 'combined VBV underflow'
    invoke('--mvc','--bitrate','300','--mvc-dependent-bitrate','100','--mvc-combined-bitrate','300',
           '-o',d/'mix.264',sbs,ok=False)
    invoke('--mvc','--mvc-dependent-crf-offset','3','--bitrate','300','--frames','12','-o',d/'off.264',sbs)

    # Two-pass: per-view statistics, first pass must precede second.
    stats = d/'mv.log'
    two = ['--mvc','--preset','veryfast','--bitrate','300','--frames','24','--keyint','12','--bframes','2']
    invoke(*two,'--pass','2','--stats',stats,'-o',d/'x.264',long_sbs,ok=False)
    invoke(*two,'--pass','1','--stats',stats,'-o',d/'p1.264',long_sbs)
    assert (d/'mv.log.mvc').exists() and (d/'mv.log.base').exists() and (d/'mv.log.dep').exists()
    invoke(*two,'--pass','2','--stats',stats,'-o',d/'p2.264',long_sbs)
    check_two = nal_units((d/'p2.264').read_bytes())
    assert sum((n[0]&31)==20 for n in check_two) == 24
print('x264 stock CLI/MVC smoke suite passed')
