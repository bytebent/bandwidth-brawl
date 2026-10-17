# Bandwidth Brawl

The sister plugin of [Bandwidth Crusher](https://github.com/bytebent/bandwidth-crusher).
Two inputs (the track and a sidechain) share **one loop buffer** and **one byte budget per
second**. Whichever input is changing more wins the right to rewrite the loop; the other
freezes on stale audio until it gets bandwidth back. The two overwrite each other, so
what you hear is a patchwork of both tracks, eating each other.

Formats: **AU and VST3 for macOS** (Apple Silicon + Intel), plus a standalone app.

## Download and install

Grab `BandwidthBrawl-macOS.zip` from the
[latest build](https://github.com/bytebent/bandwidth-brawl/releases/tag/latest),
unzip it and follow `INSTALL.txt` (right-click `Install.command` → Open). It also explains
how to route the sidechain in Ableton.

## How the fight works

Each input keeps its own idea of what the loop should contain, and remembers what it last
wrote there. Its **claim** is how much its input has changed since then. Every 10 ms the
budget is split by those claims, and each side rewrites its most-changed chunks. When B
overwrites part of A's audio, A only takes that ground back once A changes there. A silent
or unchanging input claims nothing, so without a sidechain the plugin leaves A alone at
high budgets.

Loud, sustained material builds up the biggest claims and tends to win; **B Trim** and
**Bias** are the counterweights.

## Controls

| Group | Control | What it does |
|---|---|---|
| Fight | Budget (0.5–1000 kB/s) | Total bytes per second for both inputs together. |
| | Bias | Tilts the fight towards A (left) or B (right). |
| | Sharpness | Soft: the budget is split in proportion, both move. Hard: winner takes all. |
| | Hold (0–1000 ms, or synced) | A side that just won keeps the upper hand at least this long. Most audible with high Sharpness. |
| | Savings (0–2000 ms) | An idle side banks its fair share and spends it in a burst when it changes again. |
| Buffer | Loop (1 ms–4 s, or synced 1/64–4 bars) | Length of the shared loop. |
| | Chunk (1–4096 samples) | How much is written at a time. Small = fizz, large = fragments. |
| | Smoothing | Crossfade where a written chunk meets the audio around it. |
| | Memory | How much unrefreshed audio is left after 1 s. 100% = lingers forever. |
| | Bits (2–16) | Quantisation of written samples. Fewer bits = more samples per byte. |
| | Random | 0 = most-changed chunks first, 1 = random picks. |
| In / Out | B Trim | Sidechain level. A louder B changes more, so it fights harder. |
| | Mix | Dry is A and B summed; wet is the loop buffer. |
| | Output | Into a soft limiter. |
| Footer | Reset when playback starts / Seed | Makes renders repeatable from the start of playback. |

The **territory strip** shows the loop: blue parts were written by A, red by B, fading to
grey as they age. The meter under it shows who is getting the bandwidth right now. The
**B chip** in the header lights up when sidechain audio arrives.

The plugin adds 10 ms of latency, which it reports to the host, so Ableton compensates automatically.

## Repository layout

- `source/BrawlDSP.h` — the effect itself, real-time safe, no JUCE dependency
- `source/PluginProcessor.*`, `source/PluginEditor.*` — JUCE plugin wrapper (main input + sidechain bus)
- `ui/index.html` — the interface (shown in a web view inside the plugin; also opens in a browser as a preview)
- `reference/brawl_fx.py` — the Python version, used as the reference
- `tests/` — C++ vs Python comparison tests
- `tools/bandwidth_brawl.html` — the offline browser test bench (two files in, automation lanes, WAV out)

## Building

Every push to `main` builds the Mac plugin on GitHub Actions and publishes it as the
`latest` release. To build locally on a Mac (Xcode command line tools + CMake):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target BandwidthBrawl_AU BandwidthBrawl_VST3
```

## License note

Built with [JUCE](https://juce.com) 8, which is available under the AGPLv3 or a
commercial licence. Keeping this repository public and open source satisfies the
AGPL; if you ever want to sell the plugin as closed source, check JUCE's licence terms first.
