// Command-line runner for the DSP core, used by tests/compare.py.
// usage: dsp_cli a.f32 b.f32 out.f32 channels sampleRate hostBlock [key=value ...]
// Files are raw float32, planar (all of channel 0, then channel 1). b.f32 may be "-"
// for no sidechain. hostBlock = 0 means random host buffer sizes between 1 and 2048.
#include "../source/BrawlDSP.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

static std::vector<std::vector<float>> readPlanar (const char* path, int ch, long& n)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) { std::fprintf (stderr, "can't open %s\n", path); std::exit (1); }
    std::fseek (f, 0, SEEK_END); const long bytes = std::ftell (f); std::fseek (f, 0, SEEK_SET);
    n = bytes / 4 / ch;
    std::vector<std::vector<float>> x ((size_t) ch, std::vector<float> ((size_t) n));
    for (int c = 0; c < ch; ++c)
        if (std::fread (x[(size_t) c].data(), 4, (size_t) n, f) != (size_t) n) std::exit (1);
    std::fclose (f);
    return x;
}

int main (int argc, char** argv)
{
    if (argc < 7) { std::fprintf (stderr, "usage\n"); return 1; }
    const int ch = std::atoi (argv[4]);
    const double sr = std::atof (argv[5]);
    const int hostBlock = std::atoi (argv[6]);
    bb::Params p; uint32_t seed = 1; bool sweep = false;
    for (int a = 7; a < argc; ++a)
    {
        std::string kv = argv[a]; auto eq = kv.find ('='); auto k = kv.substr (0, eq); float v = std::stof (kv.substr (eq + 1));
        if (k == "budget_kbs") p.budgetKbs = v; else if (k == "bias") p.bias = v; else if (k == "sharpness") p.sharpness = v;
        else if (k == "hold_ms") p.holdMs = v; else if (k == "savings_ms") p.savingsMs = v; else if (k == "loop_ms") p.loopMs = v;
        else if (k == "chunk") p.chunk = v; else if (k == "smooth") p.smooth = v; else if (k == "memory") p.memory = v;
        else if (k == "bits") p.bits = v; else if (k == "randomness") p.randomness = v; else if (k == "b_trim_db") p.bTrimDb = v;
        else if (k == "mix") p.mix = v; else if (k == "out_gain_db") p.outGainDb = v;
        else if (k == "seed") seed = (uint32_t) v; else if (k == "sweep") sweep = v > 0.5f;
        else { std::fprintf (stderr, "unknown %s\n", k.c_str()); return 1; }
    }

    long n = 0, nb = 0;
    auto a = readPlanar (argv[1], ch, n);
    const bool hasB = std::strcmp (argv[2], "-") != 0;
    std::vector<std::vector<float>> b;
    if (hasB) { b = readPlanar (argv[2], ch, nb); if (nb != n) { std::fprintf (stderr, "length mismatch\n"); return 1; } }

    bb::Brawl dsp; dsp.prepare (sr, ch, 8.0); dsp.reset (seed);
    std::mt19937 gen (7); std::uniform_int_distribution<int> dist (1, 2048);
    bb::LoopView view;
    auto t0 = std::chrono::steady_clock::now();
    long at = 0;
    while (at < n)
    {
        int len = hostBlock > 0 ? hostBlock : dist (gen);
        len = (int) std::min<long> (len, n - at);
        bb::Params q = p;
        if (sweep)   // automate everything at once to shake out edge cases
        {
            const float t = (float) at / (float) n;
            q.loopMs = 1.0f + 3999.0f * std::pow (t, 3.0f); q.chunk = 1.0f + 4095.0f * t; q.bits = 2.0f + 14.0f * t;
            q.budgetKbs = 0.5f + 999.5f * (1.0f - t); q.memory = t; q.randomness = 1.0f - t; q.smooth = t;
            q.bias = std::sin (t * 20.0f); q.sharpness = t; q.holdMs = 1000.0f * t * t; q.savingsMs = 2000.0f * (1.0f - t);
            q.bTrimDb = -24.0f + 48.0f * t; q.mix = 1.0f - 0.5f * t;
        }
        float* ptrs[2] = { a[0].data() + at, a[(size_t) std::min (1, ch - 1)].data() + at };
        const float* sptrs[2] = { hasB ? b[0].data() + at : nullptr, hasB ? b[(size_t) std::min (1, ch - 1)].data() + at : nullptr };
        dsp.process (ptrs, ch, sptrs, hasB ? ch : 0, len, q);
        at += len;
        if ((at / 1600) != ((at - len) / 1600)) dsp.writeView (view);   // ~30x per second, like the editor
    }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::fprintf (stderr, "latency=%d realtime_factor=%.1f holdA=%.3f holdB=%.3f\n",
                  dsp.latencySamples(), ((double) n / sr) / secs, view.holdA.load(), view.holdB.load());

    FILE* f = std::fopen (argv[3], "wb");
    for (int c = 0; c < ch; ++c) std::fwrite (a[(size_t) c].data(), 4, (size_t) n, f);
    std::fclose (f);
    return 0;
}
