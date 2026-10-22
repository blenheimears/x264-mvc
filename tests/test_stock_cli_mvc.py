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
    common = ['--mvc','--threads','2','--preset','veryfast','--bitrate','400','--vbv-maxrate','600',
              '--vbv-bufsize','800','--frames','12','--keyint','6','--bframes','2']
    invoke(*common,'-o',baseline_base,'--mvc-dependent',baseline_dep,sbs)
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
    # TEST11: the bounded cross-view worker is asynchronous, but its stream
    # must remain exactly the same as the diagnostic serial path. Exercise
    # frame-threaded independent views, B-picture inter-view prediction, and
    # multiple outstanding reconstructed-reference snapshots.
    assert invoke('--mvc', '--mvc-pipeline-depth', '0', '-o', d/'bad.264', sbs, ok=False).returncode
    assert invoke('--mvc', '--mvc-pipeline-depth', '9', '-o', d/'bad.264', sbs, ok=False).returncode
    for mask_option in ([], ['--mvc-no-inter'], ['--mvc-no-inter-i']):
        for bframes in (0, 3):
            for threads in (1, 3):
                params = ['--mvc', *mask_option, '--preset', 'veryfast',
                          '--threads', str(threads), '--bframes', str(bframes),
                          '--keyint', '8', '--frames', '12']
                serial = d/'serial-threading.264'
                invoke(*params, '--mvc-serial', '-o', serial, sbs)
                expected = serial.read_bytes()
                for depth in (1, 3, 8):
                    concurrent = d/'concurrent-threading.264'
                    invoke(*params, '--mvc-pipeline-depth', str(depth), '-o', concurrent, sbs)
                    if threads <= 1:
                        assert concurrent.read_bytes() == expected, (
                            f'MVC pipeline changed output: mask={mask_option} '
                            f'bframes={bframes} threads={threads} depth={depth}')
                    else:
                        check_mvc(concurrent)
    # TEST20: an AVC encoder outputs B-picture access units in decode order.
    # The dependent input must follow the base *picture PTS*, never FIFO input
    # order. Under GOP-boundary reordering the old code associated different
    # source frames with the two views and visibly glitched near the start.
    reordered = d/'reordered-pts.y4m'
    make_y4m(reordered, 256, 128, 72)
    for bcount in (2, 3, 5):
        for threads in (1, 8):
            result = invoke('--mvc', '--preset','veryfast','--keyint','12',
                            '--bframes',str(bcount),'--threads',str(threads),
                            '--frames','72','-o',d/'pts-b.264',
                            '--mvc-dependent',d/'pts-d.mvc',reordered)
            assert b'72 stereo pairs encoded' in result.stderr, result.stderr[-1000:]
            assert b'picture order diverged' not in result.stderr

    # TEST23: verify actual decoded per-eye *content*, not just frame counts.
    # The source colors change each picture and differ between the eyes. A
    # display-order FIFO matched to AVC decode-order output pairs mismatched
    # eyes around the first B-picture GOP, even while encoder exit was 0.
    decoder = os.environ.get('MVC_DECODER')
    if decoder:
        import statistics
        sync_src = d/'eye-sync.y4m'
        width, height, count = 128, 64, 48  # each eye is 64x64
        with sync_src.open('wb') as f:
            f.write(f'YUV4MPEG2 W{width} H{height} F24:1 Ip A1:1 C420jpeg\n'.encode())
            for idx in range(count):
                luma_left, luma_right = 30+idx*3, 220-idx*3
                f.write(b'FRAME\n')
                for _ in range(height):
                    f.write(bytes((luma_left,))*64+bytes((luma_right,))*64)
                f.write(bytes((128,))* (width*height//2))
        for threads in (1, 8):
            encoded = d/'eye-sync.264'
            run = invoke('--mvc', '--preset', 'veryfast', '--crf', '9',
                         '--threads', str(threads), '--keyint', '12',
                         '--bframes', '3', '--frames', str(count),
                         '-o', encoded, sync_src)
            assert b'48 stereo pairs encoded' in run.stderr
            assert b'frame B:' in run.stderr, 'MVC B-frame scheduling was not exercised'
            decoded = subprocess.run([decoder, str(encoded), '-O'],
                                     capture_output=True, timeout=90)
            assert decoded.returncode == 0, decoded.stderr[-600:]
            assert decoded.stdout.startswith(b'YUV4MPEG2 W128 H64 ')
            frames = decoded.stdout.split(b'FRAME\n')[1:]
            assert len(frames) == count, f'expected {count} frames, got {len(frames)}'
            for idx, frame in enumerate(frames):
                luma = frame[:width*height]
                assert len(luma) == width*height
                left = sum(luma[row*width+24] for row in range(height)) / height
                right = sum(luma[row*width+width-24] for row in range(height)) / height
                assert abs(left - (30+idx*3)) < 16, (threads,idx,'left',left)
                assert abs(right - (220-idx*3)) < 16, (threads,idx,'right',right)

        # Shared VBV must NEVER replace native CPB fullness with speculative
        # per-picture grants. That old bug drove B pictures to QP=51 under
        # two-pass even though the 10 Mbit combined CPB was mostly empty.
        stats = d/'eye-sync-two-pass-stats'
        opt = ['--mvc','--preset','veryfast','--threads','4',
               '--keyint','12','--bframes','3','--frames',str(count),
               '--bitrate','1200','--mvc-dependent-bitrate','800',
               '--stats',str(stats)]
        invoke(*opt,'--pass','1','-o',d/'eye-sync-unwritten.264',sync_src)
        two_pass = d/'eye-sync-two-pass.264'
        p2 = invoke(*opt,'--pass','2','--mvc-combined-vbv-maxrate','4500',
                    '--mvc-combined-vbv-bufsize','10000',
                    '-o',two_pass,sync_src)
        assert b'frame B:' in p2.stderr
        decoded = subprocess.run([decoder,str(two_pass),'-O'],
                                 capture_output=True,timeout=90)
        assert decoded.returncode == 0, decoded.stderr[-600:]
        frames = decoded.stdout.split(b'FRAME\n')[1:]
        assert len(frames) == count, ('two-pass',len(frames))
        for idx,frame in enumerate(frames):
            luma = frame[:width*height]
            left = sum(luma[row*width+24] for row in range(height)) / height
            right = sum(luma[row*width+width-24] for row in range(height)) / height
            assert abs(left-(30+idx*3)) < 16, ('two-pass',idx,'left',left)
            assert abs(right-(220-idx*3)) < 16, ('two-pass',idx,'right',right)

    # TEST20: MVC intra inter-view P slices must retain logical IDR/I frame
    # types in first-pass stats.  Without the distinct 'D' marker, dependent
    # stats mislabel each GOP boundary as a temporal P, causing pass 2 to log
    # "specified frame type (3) ... not compatible with keyframe interval".
    gop = d/'gop-120.y4m'
    make_y4m(gop, 128, 64, 120)
    for mask_options, expect_hybrid in (([], True), (['--mvc-no-inter-i'], False)):
        tag = 'inter-i' if expect_hybrid else 'no-inter-i'
        stats = d/f'gop-{tag}'
        opts = ['--mvc','--mvc-layout','sbs',*mask_options,'--preset','veryfast',
                '--keyint','24','--min-keyint','1','--bframes','3',
                '--threads','4','--frames','120',
                '--bitrate','450','--mvc-dependent-bitrate','120',
                '--stats',stats]
        invoke(*opts,'--pass','1','-o',d/f'{tag}-discard.264',gop)
        dep_stats = pathlib.Path(str(stats)+'.dependent').read_bytes()
        base_stats = pathlib.Path(str(stats)+'.base').read_bytes()
        types = dict((int(n), t.decode()) for n,t in re.findall(
            rb'in:(\d+)\s+out:\d+\s+type:([A-Za-z])',dep_stats))
        base_types = dict((int(n), t.decode()) for n,t in re.findall(
            rb'in:(\d+)\s+out:\d+\s+type:([A-Za-z])',base_stats))
        for boundary in range(0,120,24):
            assert base_types[boundary] == 'I', (tag,boundary,base_types[boundary])
            assert types[boundary] == ('D' if expect_hybrid else 'I'), (
                tag,boundary,types[boundary])
        encoded = d/f'gop-{tag}.264'
        second = invoke(*opts,'--pass','2','-o',encoded,gop)
        assert b'120 stereo pairs encoded' in second.stderr
        assert b'specified frame type' not in second.stderr, second.stderr[-1200:]
        assert b'frame B:' in second.stderr
        decoder = os.environ.get('MVC_DECODER')
        if decoder:
            decoded = subprocess.run([decoder,str(encoded),'-O'],
                                     capture_output=True,timeout=120)
            assert decoded.returncode == 0, decoded.stderr[-800:]
            assert decoded.stdout.count(b'FRAME\n') == 120

    # TEST12: the per-eye frame-thread engine must be functional with
    # inter-view prediction.  Small 12-frame tests alone miss delayed
    # lookahead / B-reordering interactions; exercise a longer clip.
    extended = d/'extended-sbs.y4m'
    make_y4m(extended, 256, 128, 44)
    for count in (1, 2, 4, 7, 8):
        args = ['--mvc','--threads',str(count),'--preset','veryfast',
                '--keyint','12','--bframes','3', '--frames','44']
        baseline = d/'extended-serial.264'
        invoke(*args,'--mvc-serial','-o',baseline,extended)
        expected = baseline.read_bytes()
        assert b'\x00\x00\x00\x01' in expected
        for depth in (1, 3, 8):
            concurrent = d/'extended-parallel.264'
            result = invoke(*args,'--mvc-pipeline-depth',str(depth),'-o',concurrent,extended)
            if count <= 2:
                assert concurrent.read_bytes() == expected, (count,depth)
            else:
                # Independently threaded slice workers may produce different
                # entropy bytes; the decoded per-eye content test below checks
                # correctness instead of relying on binary identity.
                assert len(nal_units(concurrent.read_bytes())) == len(nal_units(expected))
            if count == 7 and depth == 3:
                assert b'base 4, dependent 3' in result.stderr
                decoder = os.environ.get('MVC_DECODER')
                if decoder:
                    got = subprocess.run([decoder, str(concurrent), '-O'], capture_output=True, timeout=90)
                    assert got.returncode == 0, got.stderr[-500:]
                    assert got.stdout.count(b'FRAME\n') == 44
    for mode in ('--mvc-no-inter','--mvc-no-inter-i','--mvc-no-inter-p','--mvc-no-inter-b'):
        dst = d/'extended-opt.264'
        invoke('--mvc',mode,'--threads','8','--frames','44',
               '--preset','veryfast','-o',dst,extended)
        assert len(dst.read_bytes()) > 500
    invoke('--mvc','--threads','8','--sliced-threads',
           '--frames','44','-o',d/'extended-sliced.264',extended)
    # TEST13: native two-pass ABR with separate stats per eye. First pass
    # intentionally ignores -o and --mvc-dependent and emits no video bytes.
    stats = d/'mvc-two-pass'
    first_output = d/'unwritten-base.264'
    first_dep = d/'unwritten-dependent.mvc'
    common = ['--mvc','--preset','veryfast','--threads','4',
              '--bitrate','1500','--mvc-dependent-bitrate','750',
              '--frames','44','--bframes','2','--keyint','12']
    invoke(*common,'--pass','1','--stats',stats,
           '-o',first_output,'--mvc-dependent',first_dep,extended)
    assert not first_output.exists() and not first_dep.exists(), 'first pass wrote video'
    assert all((d/('mvc-two-pass.'+view)).stat().st_size > 0 for view in ('base','dependent'))
    combined = d/'two-pass-combined.264'
    run = invoke(*common,'--pass','2','--stats',stats,'-o',combined,extended)
    assert b'per-frame joint RD plan' in run.stderr
    # Shared CPB constraints must be accepted in pass 2, with the same
    # predictive allocation operating at per-picture rather than clip level.
    hrd = d/'two-pass-hrd.264'
    hrun = invoke(*common,'--pass','2','--stats',stats,
                  '--mvc-combined-vbv-maxrate','4000',
                  '--mvc-combined-vbv-bufsize','10000',
                  '-o',hrd,extended)
    assert b'shared VBV 4000 kb/s / 10000 kbit' in hrun.stderr
    assert b'predictive shared VBV' in hrun.stderr
    assert hrd.stat().st_size > 0
    invoke(*common,'--pass','2','--stats',stats,
           '--mvc-combined-vbv-maxrate','2',
           '--mvc-combined-vbv-bufsize','1',
           '-o',d/'overfull.264',extended,ok=False)
    invoke(*common,'--pass','2','--stats',stats,
           '--mvc-combined-vbv-maxrate','4000',
           '-o',d/'incomplete-hrd.264',extended,ok=False)
    decoded_nals = nal_units(combined.read_bytes())
    assert sum((nal[0]&31)==20 for nal in decoded_nals) >= 44  # multiple slices per MVC frame
    split_base, split_dep = d/'two-pass-base.264', d/'two-pass-dep.mvc'
    invoke(*common,'--pass','2','--stats',stats,'-o',split_base,
           '--mvc-dependent',split_dep,extended)
    assert split_base.stat().st_size and split_dep.stat().st_size
    decoder = os.environ.get('MVC_DECODER')
    if decoder:
        p = subprocess.run([decoder, str(combined), '-O'], capture_output=True, timeout=90)
        assert p.returncode == 0, p.stderr[-500:]
        assert p.stdout.count(b'FRAME\n') == 44
    # Reject missing/mismatched statistics, and support fixed independent targets.
    invoke(*common,'--pass','2','--stats',d/'missing',
           '-o',d/'missing-out.264',extended,ok=False)
    fixed = invoke(*common,'--pass','2','--mvc-no-joint-ratecontrol',
                   '--stats',stats,'-o',d/'no-joint.264',extended)
    assert b'per-frame joint RD plan' not in fixed.stderr
    # TEST15: a non-seekable source must be read exactly once during each
    # pass; the second pass cannot depend on replaying a pipe after a late
    # CPB failure.  A generated Python test fixture is intentionally tiny;
    # the encoder itself does not cache the input or output in memory.
    pipe_stats = d/'mvc-pipe-stats'
    pipe_base = d/'pipe-base.264'
    pipe_dep = d/'pipe-dependent.mvc'
    payload = extended.read_bytes()
    pipe_common = ['--mvc', '--demuxer', 'y4m', '--threads', '4',
                   '--preset', 'veryfast', '--bitrate', '1500',
                   '--mvc-dependent-bitrate', '750', '--frames', '44',
                   '--bframes', '2', '--keyint', '12', '--stats', str(pipe_stats)]
    proc = subprocess.run([EXE,*pipe_common, '--pass', '1',
                           '-o', str(pipe_base), '--mvc-dependent', str(pipe_dep), '-'],
                          input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    assert proc.returncode == 0, proc.stderr[-1000:]
    assert not pipe_base.exists() and not pipe_dep.exists(), 'pass 1 wrote video to disk'
    proc = subprocess.run([EXE,*pipe_common, '--pass', '2',
                           '--mvc-combined-vbv-maxrate', '4000',
                           '--mvc-combined-vbv-bufsize', '10000',
                           '-o', str(pipe_base), '--mvc-dependent', str(pipe_dep), '-'],
                          input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    assert proc.returncode == 0, proc.stderr[-2000:]
    assert b'native x264 per-view VBV coordinated by dynamic shared' in proc.stderr
    assert pipe_base.stat().st_size and pipe_dep.stat().st_size
    assert len(nal_units(pipe_dep.read_bytes())) >= 44
    decoder = os.environ.get('MVC_DECODER')
    if decoder:
        # Stream as combined Annex B using the same pass-2 stats.
        combined_pipe = d/'pipe-combined.264'
        proc = subprocess.run([EXE,*pipe_common, '--pass', '2',
                               '--mvc-combined-vbv-maxrate', '4000',
                               '--mvc-combined-vbv-bufsize', '10000',
                               '-o', str(combined_pipe), '-'],
                              input=payload, capture_output=True, timeout=90)
        assert proc.returncode == 0, proc.stderr[-1600:]
        decoded = subprocess.run([decoder, str(combined_pipe), '-O'], capture_output=True, timeout=90)
        assert decoded.returncode == 0 and decoded.stdout.count(b'FRAME\n') == 44
    print('x264 MVC non-seekable two-pass + native combined VBV regressions passed')
    print('x264 MVC two-pass/no-video-output regressions passed')
    print('x264 MVC threading output-parity regressions passed')
print('x264 stock CLI/MVC smoke suite passed')
