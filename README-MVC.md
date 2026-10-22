# x264 MVC — TEST8: stock CLI integration (EXPERIMENTAL)

This fork adds opt-in two-view H.264 MVC encoding to the **normal x264 command
line program**, not a custom executable. The default x264 encoder CLI and its
library-backed 2D encoding path remain in place. Stock x264 flags, option
parsing, presets, tune, rate-control options, input demuxers and video filters
are not reimplemented. `x264` without `--mvc` runs the existing AVC path.

The MVC work lives in small C sources (`mvc/cli.c`, `mvc/nal.c`), specific
library extensions for inter-view references and Annex H slice syntax, and a
small extension to the existing CLI. The two-eye encoder still uses the same
x264 motion estimation, SIMD, reconstruction, entropy coding, and rate control.
The stock `filters/video/source.c` now remembers each input's demuxer callbacks
per source; multiple simultaneous inputs cannot corrupt each other's context.

There is no command-line compatibility with the fork's previous separate
`x264-mvc encode ...` frontend. This is deliberate. Use standard x264 syntax.

## Build

```sh
./configure --enable-static --disable-opencl
make -j"$(nproc)" x264
python3 tests/test_stock_cli_mvc.py ./x264
# Optional: compare 2D output against a separately compiled stock x264
STOCK_X264=/path/to/stock/x264 python3 tests/test_stock_cli_mvc.py ./x264
```

As in ordinary x264, configure autodetects FFmpeg/libavformat, libswscale,
FFMS2, and other optional input libraries. Availability of container formats
therefore depends on the build dependencies. On x86, install NASM to retain
x264's assembly optimizations. Only raw Annex B `.264`/`.mvc` **output** is
supported in MVC mode. 2D muxer support is unchanged.

NixOS:

```sh
nix build .#x264-mvc -L
./result/bin/x264 --help
nix flake check -L
```

The Nix build selects NASM explicitly on x86-64 and patches shell-script
shebangs inside the sandbox. The Nix build itself has not been run here.

## Usage

```sh
# Ordinary AVC: identical command-line semantics to stock x264
x264 --preset slow --crf 20 -o film.264 film.y4m

# Side-by-side full-resolution stereo video (e.g. 3840x1080 -> 2x1920x1080)
x264 --mvc --mvc-layout sbs --preset medium --crf 20 \
    -o combined.264 stereo.y4m

# Split AVC base view / MVC dependent view
x264 --mvc --mvc-layout sbs --preset slow --crf 18 \
    -o base.264 --mvc-dependent dependent.mvc stereo.y4m

# Top/bottom layout
x264 --mvc --mvc-layout tb --bframes 3 -o stereo.264 top_bottom.y4m

# Alternating-eye Y4M (input F=48 fps -> MVC pairs=24 fps by default)
x264 --mvc --mvc-layout frames -o stereo.264 alternating.y4m

# Two separate input files. Both use the stock x264 input modules.
x264 --mvc --mvc-layout separate --mvc-right right.y4m \
    --preset medium --crf 21 -o stereo.264 left.y4m

# Disable MVC I/IDR prediction without disabling P and B
x264 --mvc --mvc-no-inter-i -o stereo.264 stereo_sbs.y4m

# Turn all inter-view prediction off, then enable only P-pictures
x264 --mvc --mvc-no-inter --mvc-inter-p \
    -o stereo.264 stereo_sbs.y4m
```

### New options

| Flag | Meaning |
| --- | --- |
| `--mvc` | Enable two-view MVC (otherwise stock x264 2D) |
| `--mvc-layout sbs\|tb\|frames\|separate` | Select input arrangement (default `sbs`) |
| `--mvc-right FILE` | Right-eye input for `separate` |
| `--mvc-dependent FILE` | Split dependent MVC NAL units into their own file |
| `--mvc-inter`, `--mvc-no-inter` | Enable/disable all inter-view prediction |
| `--mvc-inter-i`, `--mvc-no-inter-i` | Enable/disable inter-view prediction for dependent I/IDR |
| `--mvc-inter-p`, `--mvc-no-inter-p` | Enable/disable prediction for dependent P |
| `--mvc-inter-b`, `--mvc-no-inter-b` | Enable/disable prediction for dependent B |
| `--mvc-combined-bitrate`, `--mvc-combined-vbv-maxrate`, `--mvc-combined-vbv-bufsize` | Combined targets for both views (stock `--bitrate`, `--vbv-maxrate`, `--vbv-bufsize` mean the same) |
| `--mvc-dependent-bitrate`, `--mvc-dependent-vbv-maxrate`, `--mvc-dependent-vbv-bufsize` | Independent dependent-view rate control; stock rate options then apply to the base view only. Cannot be mixed with the combined options |
| `--mvc-dependent-crf-offset` | CRF offset of the dependent view under combined rate control |
| `--mvc-view-vbv-maxrate`, `--mvc-view-vbv-bufsize` | Upper limits for each view's VBV when combined rate control divides the budget (e.g. Blu-ray per-view limits) |
| `--mvc-rc-trace FILE` | Per-access-unit rate-control CSV |

All inter-view prediction is **enabled by default**, and flags are processed
in their command-line order. The plain x264 `--frames` flag counts stereo
**pairs** in MVC mode; `--fps` specifies the output rate of stereo pairs.
Alternating-eye input halves the detected per-frame rate unless `--fps`
is explicitly supplied. Standard `--seek` counts pairs when MVC is active.

## Current MVC constraints

* Progressive, 8-bit, YUV 4:2:0 picture encoding only. The stock input
  demuxer and filters may convert other formats where libswscale is available.
* Only one AVC-compatible base eye and one dependent MVC eye.
* Raw Annex-B `.264` or `.mvc` output; no M2TS, SSIF, or Blu-ray playlists.
* Both eyes share an enforced, deterministic GOP structure. Adaptive B-frame
  placement, scenecut insertion, and open GOP are disabled **only in MVC mode**
  to prevent drift between the two separately optimized x264 encoders.
* The MVC joint DPB limit is eight pictures per view. With inter-view
  prediction, slice threading is used to keep reconstructed base references
  ready for the dependent encoder, potentially lowering overall throughput.
* `--qpfile`, pulldown, timecode output, VFR, and interlaced encoding are not
  supported in MVC mode. Ordinary 2D modes keep stock support. MVC two-pass
  supports `--pass 1` and `--pass 2` with per-view statistics files.
* A dependent view I/IDR that uses inter-view prediction is coded as an MVC
  P-slice, referencing the already reconstructed base picture. Consumer
  hardware acceptance of that mode remains **unverified**.
* Do not interpret byte-for-byte matching on the included 2D regression
  vectors as exhaustive testing of all upstream command-line combinations.

## Engineering and licensing

The stock `x264.c` parser is extended rather than replaced. New switches use
an `mvc-` prefix. Without `--mvc`, parsing and the native `encode()` function
work as upstream. Changes to the existing encoder library are used only when
the MVC parameter flags are set. MVC bitstream transformation lives in C code;
there is no runtime invocation of the JMVC/reference encoder and no JMVC code.
The original x264 license notices and `COPYING` are preserved, and all new code
is GPL-2.0-or-later. Source licenses do not grant AVC/MVC patent rights.

Release validation: native scalar GCC build, independent edge264-mvc software
decode of test clips, stock x264 2D byte-comparison on representative test
vectors, and a targeted AddressSanitizer check of independent right-eye input.
These do not establish full MVC conformance, hardware Blu-ray 3D playback, or
NixOS/NASM build success. **TEST / EXPERIMENTAL build; not production-certified.**
