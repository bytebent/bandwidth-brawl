// Bandwidth Brawl — real-time DSP core (no JUCE dependency, so it can be tested on its own).
//
// One loop buffer plays on repeat. Input A (the track) and input B (the sidechain)
// each keep a target (what the loop would contain if that input had its way) and a
// model (what that side last wrote at each position). A side's pending change is
// where its target differs from its model: the parts where that input has changed
// since it last wrote there. Every 10 ms control block the byte budget is split
// between A and B by how much each has pending (tilted by Bias, more or less
// winner-take-all by Sharpness, held by Hold, unused budget banked by Savings), and
// each side writes its most-changed chunks into the shared buffer. What you hear is
// the buffer.
//
// This is a port of reference/brawl_fx.py. Fixed 10 ms control blocks make the sound
// independent of the host's buffer size, at the cost of 10 ms latency that the plugin
// reports to the host.
//
// Real-time rules: everything is allocated in prepare(); process() never allocates,
// locks or does I/O.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bb
{

struct Params
{
    float budgetKbs  = 24.0f;   // total for both inputs, kB/s
    float bias       = 0.0f;    // -1 = all A ... +1 = all B
    float sharpness  = 0.5f;    // 0 = proportional split, 1 = winner takes all
    float holdMs     = 0.0f;    // a new leader keeps the upper hand at least this long
    float savingsMs  = 0.0f;    // how much budget an idle side may bank, in ms of budget
    float loopMs     = 250.0f;
    float chunk      = 64.0f;   // samples per write
    float smooth     = 0.5f;    // chunk edge crossfade, 0..1
    float memory     = 1.0f;    // fraction of unrefreshed audio left after 1 s
    float bits       = 16.0f;
    float randomness = 0.0f;    // 0 = most-changed chunks first, 1 = random
    float bTrimDb    = 0.0f;    // sidechain level
    float mix        = 1.0f;    // 0 = A + B dry, 1 = buffer
    float outGainDb  = 0.0f;
};

// Snapshot for the UI. Written by the audio thread, read by the UI thread.
struct LoopView
{
    static constexpr int N = 240;
    std::array<std::atomic<float>, N> amp {};     // peak level 0..1 per bin
    std::array<std::atomic<int>, N>   own {};     // 0 = never written, 1 = A, 2 = B
    std::array<std::atomic<float>, N> age {};     // seconds since that bin was last written
    std::atomic<float> shareA { 0.5f };            // recent budget split (fraction to A)
    std::atomic<float> holdA { 0.0f }, holdB { 0.0f };   // fraction of the loop each side holds
    std::atomic<float> playhead { 0.0f }, loopMs { 0.0f }, sideLevel { 0.0f };
    std::atomic<uint32_t> counter { 0 };
};

class Rng
{
public:
    void seed (uint64_t v)
    {
        s = v * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
        if (s == 0) s = 1;
        for (int i = 0; i < 8; ++i) next();
    }
    uint64_t next()
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    float uniform() { return (float) (next() >> 40) * (1.0f / 16777216.0f); }  // [0, 1)
private:
    uint64_t s = 1;
};

inline float dbToLin (float db) { return std::pow (10.0f, db / 20.0f); }

inline float softLimit (float x)
{
    constexpr float knee = 0.8f, head = 0.2f;
    const float a = std::fabs (x);
    if (a <= knee) return x;
    return std::copysign (knee + head * std::tanh ((a - knee) / head), x);
}

// Fraction of the budget that goes to A. Same as split_share() in the Python reference.
inline double splitShare (double claimA, double claimB, double wA, double wB, double sharp)
{
    const double ca = claimA * wA, cb = claimB * wB;
    if (ca <= 0.0 && cb <= 0.0) { const double tot = wA + wB; return tot <= 0.0 ? 0.5 : wA / tot; }
    if (cb <= 0.0) return 1.0;
    if (ca <= 0.0) return 0.0;
    if (sharp >= 0.999) return ca > cb ? 1.0 : (cb > ca ? 0.0 : 0.5);
    const double k = 1.0 / (1.0 - sharp);
    const double z = k * (std::log (cb) - std::log (ca));
    if (z > 50.0) return 0.0;
    if (z < -50.0) return 1.0;
    return 1.0 / (1.0 + std::exp (z));
}

class Brawl
{
public:
    static constexpr int maxChunk = 4096;
    static constexpr double eps = 1e-6;   // pending change below this per sample = nothing to do

    void prepare (double sampleRate, int numChannels, double maxLoopSeconds = 8.0)
    {
        sr = sampleRate;
        ch = std::clamp (numChannels, 1, 2);
        block = std::max (16, (int) std::lround (sr * 0.010));
        maxL = (int) std::ceil (maxLoopSeconds * sr) + 1;
        const auto L_ = (size_t) maxL;

        for (int s = 0; s < 2; ++s)
        {
            for (int c = 0; c < 2; ++c)
            {
                target[s][c].assign (L_, 0.0f);
                model[s][c].assign (L_, 0.0f);
                inFifo[s][c].assign ((size_t) block, 0.0f);
                dry[s][c].assign ((size_t) block, 0.0f);
            }
            d[s].assign (L_, 0.0f);
            sum[s].assign (L_, 0.0);
            taken[s].assign (L_, 0);
            picks[s].assign (L_, 0);
        }
        for (int c = 0; c < 2; ++c)
        {
            buf[c].assign (L_, 0.0f);
            outFifo[c].assign ((size_t) block, 0.0f);
        }
        owner.assign (L_, 0);
        stamp.assign (L_, 0u);
        mask.assign (L_, 0.0f);
        touched.assign (L_, 0);
        scratchF.assign (L_, 0.0f);
        scratchU.assign (L_, 0u);
        scratchB.assign (L_, 0);
        candJ.assign ((size_t) (maxCandidates + block + 8), 0);   // window + the chunks at the playhead
        candE.assign (candJ.size(), 0.0);
        scores.assign (candJ.size(), 0.0f);
        dirtyMax = maxL;
        reset (1);
    }

    int latencySamples() const { return block; }

    // Clears the loop and the random generator. Safe on the audio thread.
    void reset (uint32_t seed)
    {
        const auto n = (std::ptrdiff_t) dirtyMax;
        for (int s = 0; s < 2; ++s)
        {
            for (int c = 0; c < 2; ++c)
            {
                std::fill (target[s][c].begin(), target[s][c].begin() + n, 0.0f);
                std::fill (model[s][c].begin(), model[s][c].begin() + n, 0.0f);
            }
            std::fill (d[s].begin(), d[s].begin() + n, 0.0f);
            std::fill (sum[s].begin(), sum[s].begin() + n, 0.0);
            std::fill (taken[s].begin(), taken[s].begin() + n, (uint8_t) 0);
            total[s] = 0.0; pending[s] = 0; bank[s] = 0.0;
        }
        for (int c = 0; c < 2; ++c)
        {
            std::fill (buf[c].begin(), buf[c].begin() + n, 0.0f);
            std::fill (outFifo[c].begin(), outFifo[c].end(), 0.0f);
            lastOut[c] = 0.0f;
        }
        std::fill (owner.begin(), owner.begin() + n, (uint8_t) 0);
        std::fill (stamp.begin(), stamp.begin() + n, 0u);
        std::fill (mask.begin(), mask.begin() + n, 0.0f);
        dirtyMax = 0;
        L = 0; pos = 0; fifoPos = 0; curChunk = -1; rollIdx = 0;
        leader = 0; holdLeft = 0.0; lastShareA = 0.5; shareEma = 0.5f; sideEma = 0.0f;
        declick = false; firstBlock = true; clock = 0;
        rng.seed (seed);
    }

    // main: the track (in place, becomes the output). side: the sidechain, or nullptr / 0 channels.
    void process (float* const* main, int numMain, const float* const* side, int numSide, int numSamples, const Params& p)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < ch; ++c)
            {
                inFifo[0][c][(size_t) fifoPos] = main[std::min (c, numMain - 1)][i];
                inFifo[1][c][(size_t) fifoPos] = numSide > 0 ? side[std::min (c, numSide - 1)][i] : 0.0f;
            }
            for (int c = 0; c < numMain; ++c)
                main[c][i] = outFifo[std::min (c, ch - 1)][(size_t) fifoPos];

            if (++fifoPos == block)
            {
                runControlBlock (p);
                fifoPos = 0;
            }
        }
    }

    // Fill a UI snapshot. O(loop length / stride); call ~30x per second while the editor is open.
    void writeView (LoopView& v) const
    {
        if (L <= 0) return;
        const uint32_t now = (uint32_t) clock;
        long totalA = 0, totalB = 0, counted = 0;
        for (int b = 0; b < LoopView::N; ++b)
        {
            const int s0 = (int) ((int64_t) b * L / LoopView::N);
            const int s1 = std::max (s0 + 1, (int) ((int64_t) (b + 1) * L / LoopView::N));
            const int stride = std::max (1, (s1 - s0) / 64);
            float peak = 0.0f; int na = 0, nb = 0; uint32_t newest = 0; bool any = false;
            for (int i = s0; i < s1 && i < L; i += stride)
            {
                for (int c = 0; c < ch; ++c) peak = std::max (peak, std::fabs (buf[c][(size_t) i]));
                const uint8_t o = owner[(size_t) i];
                if (o == 1) ++na; else if (o == 2) ++nb;
                const uint32_t st = stamp[(size_t) i];
                if (st != 0 && (! any || (uint32_t) (now - st) < (uint32_t) (now - newest))) { newest = st; any = true; }
                ++counted;
            }
            totalA += na; totalB += nb;
            v.amp[(size_t) b].store (std::min (1.0f, peak), std::memory_order_relaxed);
            v.own[(size_t) b].store (na == 0 && nb == 0 ? 0 : (na >= nb ? 1 : 2), std::memory_order_relaxed);
            v.age[(size_t) b].store (any ? (float) ((uint32_t) (now - newest) / sr) : 1000.0f, std::memory_order_relaxed);
        }
        v.holdA.store (counted ? (float) totalA / (float) counted : 0.0f, std::memory_order_relaxed);
        v.holdB.store (counted ? (float) totalB / (float) counted : 0.0f, std::memory_order_relaxed);
        v.shareA.store (shareEma, std::memory_order_relaxed);
        v.sideLevel.store (sideEma, std::memory_order_relaxed);
        v.loopMs.store ((float) (1000.0 * L / sr), std::memory_order_relaxed);
        v.playhead.store ((float) pos / (float) L, std::memory_order_relaxed);
        v.counter.fetch_add (1, std::memory_order_release);
    }

private:
    // ---------------------------------------------------------------- pending-change bookkeeping
    inline void addToChunk (int s, int j, double delta)
    {
        double& x = sum[s][(size_t) j];
        const double thr = eps * curChunk;
        const bool was = x > thr;
        x += delta;
        total[s] += delta;
        const bool is = x > thr;
        pending[s] += (int) is - (int) was;
    }
    inline void updateD (int s, int i)
    {
        float nd = 0.0f;
        for (int c = 0; c < ch; ++c) nd += std::fabs (target[s][c][(size_t) i] - model[s][c][(size_t) i]);
        const float delta = nd - d[s][(size_t) i];
        if (std::fabs (delta) > 0.0f) { d[s][(size_t) i] = nd; addToChunk (s, i / curChunk, (double) delta); }
    }
    void resumChunk (int s, int j)
    {
        const int s0 = j * curChunk, s1 = std::min (L, s0 + curChunk);
        double e = 0.0;
        for (int i = s0; i < s1; ++i) e += d[s][(size_t) i];
        addToChunk (s, j, e - sum[s][(size_t) j]);
    }
    void rebuildSums()
    {
        const int n = (L + curChunk - 1) / curChunk;
        const double thr = eps * curChunk;
        for (int s = 0; s < 2; ++s)
        {
            total[s] = 0.0; pending[s] = 0;
            for (int j = 0; j < n; ++j)
            {
                const int s0 = j * curChunk, s1 = std::min (L, s0 + curChunk);
                double e = 0.0;
                for (int i = s0; i < s1; ++i) e += d[s][(size_t) i];
                sum[s][(size_t) j] = e; total[s] += e;
                if (e > thr) ++pending[s];
            }
        }
    }

    void stretchTo (int newL)
    {
        const int oldL = L;
        auto stretchF = [&] (std::vector<float>& a)
        {
            for (int j = 0; j < newL; ++j)
            {
                const double src = (double) j * oldL / newL;
                const int i0 = (int) src;
                const float f = (float) (src - i0);
                const float x0 = a[(size_t) i0], x1 = a[(size_t) ((i0 + 1) % oldL)];
                scratchF[(size_t) j] = x0 + f * (x1 - x0);
            }
            std::copy_n (scratchF.begin(), newL, a.begin());
        };
        for (int c = 0; c < ch; ++c)
        {
            stretchF (buf[c]);
            for (int s = 0; s < 2; ++s) { stretchF (target[s][c]); stretchF (model[s][c]); }
        }
        for (int j = 0; j < newL; ++j)
        {
            const auto src = (size_t) std::min (oldL - 1, (int) ((int64_t) j * oldL / newL));
            scratchU[(size_t) j] = stamp[src];
            scratchB[(size_t) j] = owner[src];
        }
        std::copy_n (scratchU.begin(), newL, stamp.begin());
        std::copy_n (scratchB.begin(), newL, owner.begin());
        pos = (int) ((int64_t) pos * newL / oldL) % newL;
        L = newL;
        dirtyMax = std::max (dirtyMax, L);
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < L; ++i)
            {
                float nd = 0.0f;
                for (int c = 0; c < ch; ++c) nd += std::fabs (target[s][c][(size_t) i] - model[s][c][(size_t) i]);
                d[s][(size_t) i] = nd;
            }
        declick = true;
        curChunk = -1;   // forces the chunk sums to be rebuilt for the new length
    }

    // ---------------------------------------------------------------- choosing chunks
    // Adds up to floor(avail / cost) of side s's most-changed chunks to its pick list.
    // Returns the budget spent.
    double pick (int s, double avail, double cost, float r, int nChunks, int m)
    {
        long k = (long) std::floor (avail / cost);
        if (k <= 0 || pending[s] <= 0) return 0.0;
        const double thr = eps * curChunk;
        auto& sm = sum[s];
        auto& tk = taken[s];

        // Candidates. Small loops: all chunks. Long loops with tiny chunks: a random
        // contiguous stretch plus the chunks just written by the input (where new change
        // appears), so the cost per block stays bounded.
        int nList = 0; double emax = 0.0;
        auto add = [&] (int j)
        {
            const double e = sm[(size_t) j];
            if (e <= thr || tk[(size_t) j]) return;
            candJ[(size_t) nList] = j; candE[(size_t) nList] = e; ++nList;
            emax = std::max (emax, e);
        };
        if (nChunks <= maxCandidates)
        {
            for (int j = 0; j < nChunks; ++j) add (j);
        }
        else
        {
            const int ws = (int) (rng.next() % (uint64_t) nChunks);
            for (int t = 0, j = ws; t < maxCandidates; ++t) { add (j); if (++j == nChunks) j = 0; }
            const int j0 = pos / curChunk, nHot = (m + curChunk - 1) / curChunk + 1;
            for (int t = 0; t < nHot; ++t)
            {
                const int j = (j0 + t) % nChunks;
                if ((j - ws + nChunks) % nChunks >= maxCandidates) add (j);
            }
        }
        if (nList == 0) return 0.0;
        k = std::min<long> (k, nList);
        int& np = nPicks[s];
        if (k == nList)
        {
            for (int t = 0; t < nList; ++t) { const int j = candJ[(size_t) t]; picks[s][(size_t) np++] = j; tk[(size_t) j] = 1; }
            return (double) k * cost;
        }

        // Top-k by score via a histogram: everything above the cut bin, plus enough from it.
        std::array<int, 1024> hist {};
        const double invMax = 1.0 / emax;
        for (int t = 0; t < nList; ++t)
        {
            float sc = (float) ((1.0 - r) * candE[(size_t) t] * invMax);
            if (r > 0.0f) sc += r * rng.uniform();
            sc = std::min (std::max (sc, 0.0f), 0.99999f);
            scores[(size_t) t] = sc;
            ++hist[(size_t) (sc * 1024.0f)];
        }
        int cut = 1023; long above = 0;
        while (cut > 0 && above + hist[(size_t) cut] < k) { above += hist[(size_t) cut]; --cut; }
        long fromCut = k - above;
        const int start = (int) (rng.next() % (uint64_t) nList);
        for (int u = 0; u < nList; ++u)
        {
            int t = start + u; if (t >= nList) t -= nList;
            const int bin = (int) (scores[(size_t) t] * 1024.0f);
            if (bin > cut || (bin == cut && fromCut-- > 0))
            {
                const int j = candJ[(size_t) t];
                picks[s][(size_t) np++] = j; tk[(size_t) j] = 1;
            }
        }
        return (double) k * cost;
    }

    void writePicks (int s, int chunk, int h, uint32_t now)
    {
        const int np = nPicks[s];
        auto& tg = target[s];
        auto& md = model[s];
        if (h == 0)
        {
            for (int n = 0; n < np; ++n)
            {
                const int s0 = picks[s][(size_t) n] * chunk, s1 = std::min (L, s0 + chunk);
                for (int i = s0; i < s1; ++i)
                {
                    for (int c = 0; c < ch; ++c)
                    {
                        const float t = tg[c][(size_t) i];
                        buf[c][(size_t) i] = t; md[c][(size_t) i] = t;
                    }
                    updateD (s, i);
                    owner[(size_t) i] = (uint8_t) (s + 1);
                    stamp[(size_t) i] = now;
                }
            }
            return;
        }
        // Each written chunk is fully on inside and fades over h samples into its
        // neighbours; overlapping windows take the larger weight.
        int nt = 0;
        for (int n = 0; n < np; ++n)
        {
            const int s0 = picks[s][(size_t) n] * chunk;
            const int len = std::min (chunk, L - s0);
            const int wl = len + 2 * h;
            for (int t = 0; t < wl; ++t)
            {
                const float w = t < h ? ((float) t + 0.5f) / (float) h
                              : (t < h + chunk ? 1.0f : ((float) (h + chunk + h - 1 - t) + 0.5f) / (float) h);
                const int i = ((s0 - h + t) % L + L) % L;
                float& mk = mask[(size_t) i];
                if (mk <= 0.0f) touched[(size_t) nt++] = i;
                mk = std::max (mk, w);
            }
        }
        for (int u = 0; u < nt; ++u)
        {
            const int i = touched[(size_t) u];
            const float w = mask[(size_t) i];
            for (int c = 0; c < ch; ++c)
            {
                const float t = tg[c][(size_t) i];
                float& b = buf[c][(size_t) i];
                b += w * (t - b);
                md[c][(size_t) i] = t;   // the side counts its whole window as written
            }
            updateD (s, i);
            if (w >= 0.5f) { owner[(size_t) i] = (uint8_t) (s + 1); stamp[(size_t) i] = now; }
            mask[(size_t) i] = 0.0f;
        }
    }

    // ---------------------------------------------------------------- processing
    void runControlBlock (const Params& p)
    {
        const float gB1 = dbToLin (p.bTrimDb), gOut1 = dbToLin (p.outGainDb);
        const float mix1 = std::clamp (p.mix, 0.0f, 1.0f);
        if (firstBlock) { gB0 = gB1; gOut0 = gOut1; mix0 = mix1; firstBlock = false; }

        double sideSq = 0.0;
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i)
            {
                dry[0][c][(size_t) i] = inFifo[0][c][(size_t) i];
                const float x = inFifo[1][c][(size_t) i];
                sideSq += (double) x * x;
                dry[1][c][(size_t) i] = x * (gB0 + (gB1 - gB0) * (float) i / (float) block);
            }
        sideEma = 0.8f * sideEma + 0.2f * (float) std::sqrt (sideSq / (block * ch));

        int off = 0;
        while (off < block)
        {
            const int newL = std::clamp ((int) std::lround (p.loopMs * sr / 1000.0), 8, maxL);
            if (L == 0) { L = newL; dirtyMax = std::max (dirtyMax, L); curChunk = -1; pos = 0; }
            else if (std::abs (newL - L) > std::max (1.0, 0.01 * L)) stretchTo (newL);

            const int m = std::min (block - off, L);
            subBlock (off, m, p, gOut1, mix1);
            off += m;
        }
        gB0 = gB1; gOut0 = gOut1; mix0 = mix1;
    }

    void subBlock (int off, int m, const Params& p, float gOut1, float mix1)
    {
        const double blockS = (double) m / sr;
        const int bits = std::clamp ((int) std::lround (p.bits), 2, 16);
        const float levels = (float) ((1 << (bits - 1)) - 1);
        const int chunk = std::clamp ((int) std::lround (p.chunk), 1, std::min (L, maxChunk));
        if (chunk != curChunk) { curChunk = chunk; rollIdx = 0; rebuildSums(); }
        const int nChunks = (L + chunk - 1) / chunk;

        // 1) both inputs write their own target at the positions playing now
        for (int i = 0; i < m; ++i)
        {
            const int at = (pos + i) % L;
            for (int s = 0; s < 2; ++s)
            {
                for (int c = 0; c < ch; ++c)
                {
                    const float x = std::clamp (dry[s][c][(size_t) (off + i)], -1.0f, 1.0f);
                    target[s][c][(size_t) at] = std::round (x * levels) / levels;
                }
                updateD (s, at);
            }
        }
        // keep the incremental sums honest: re-add a few chunks exactly every block
        {
            const int n = std::max (1, nChunks / 256);
            for (int t = 0; t < n; ++t)
            {
                rollIdx = (rollIdx + 1) % nChunks;
                resumChunk (0, rollIdx); resumChunk (1, rollIdx);
            }
            if (rollIdx + n >= nChunks)   // once per lap, also re-add the totals
                for (int s = 0; s < 2; ++s)
                {
                    double t = 0.0;
                    for (int j = 0; j < nChunks; ++j) t += sum[s][(size_t) j];
                    total[s] = t;
                }
        }

        // 2) split the budget
        const double bias = std::clamp ((double) p.bias, -1.0, 1.0);
        const double wA = (1.0 - bias) / 2.0, wB = (1.0 + bias) / 2.0;
        const double sharp = std::clamp ((double) p.sharpness, 0.0, 1.0);
        double share = splitShare (std::max (0.0, total[0]) / chunk, std::max (0.0, total[1]) / chunk, wA, wB, sharp);

        const int rawLeader = share > 0.5 ? 1 : (share < 0.5 ? 2 : leader);
        const double holdMs = std::max (0.0f, p.holdMs);
        if (leader == 0 || (rawLeader != leader && holdLeft <= 0.0))
        {
            if (rawLeader != leader) { leader = rawLeader; holdLeft = holdMs; }
        }
        else if (rawLeader != leader && holdLeft > 0.0)
        {
            share = 1.0 - share;   // the current leader keeps the bigger part while its hold lasts
        }
        holdLeft -= blockS * 1000.0;

        const double budget = std::max (0.0f, p.budgetKbs);
        const double bytesPs = bits / 8.0;
        const double allowance = budget * 1000.0 * blockS / bytesPs;   // channel-samples
        const double cost = (double) chunk * ch;

        // Savings: an idle side keeps earning its fair share (set by Bias) into its bank
        const double savS = std::max (0.0f, p.savingsMs) / 1000.0;
        const double cap = std::max (cost, savS * budget * 1000.0 / bytesPs);
        double keep[2] = { 0.0, 0.0 };
        if (savS > 0.0)
        {
            if (pending[0] <= 0) keep[0] = wA * allowance;
            if (pending[1] <= 0) keep[1] = wB * allowance;
        }
        const double rest = allowance - keep[0] - keep[1];
        double avail[2] = { share * rest + keep[0] + bank[0], (1.0 - share) * rest + keep[1] + bank[1] };

        const float r = std::clamp (p.randomness, 0.0f, 1.0f);
        nPicks[0] = nPicks[1] = 0;
        double sp[2];
        sp[0] = pick (0, avail[0], cost, r, nChunks, m);
        sp[1] = pick (1, avail[1], cost, r, nChunks, m);
        double left[2] = { avail[0] - sp[0], avail[1] - sp[1] };

        // each side may keep up to `cap` for later; anything above goes to the other side
        double over[2] = { std::max (0.0, left[0] - cap), std::max (0.0, left[1] - cap) };
        left[0] -= over[0]; left[1] -= over[1];
        if (over[1] > 0.0)
        {
            const double x = pick (0, over[1] + left[0], cost, r, nChunks, m);
            sp[0] += x; left[0] = left[0] + over[1] - x;
        }
        if (over[0] > 0.0)
        {
            const double x = pick (1, over[0] + left[1], cost, r, nChunks, m);
            sp[1] += x; left[1] = left[1] + over[0] - x;
        }
        bank[0] = std::min (left[0], cap);
        bank[1] = std::min (left[1], cap);

        // 3) write: the leader writes last, so it wins chunks both sides picked
        const uint32_t now = (uint32_t) (clock + (uint64_t) m) | 1u;   // never 0 (0 = never written)
        const int h = chunk >= 4 ? (int) std::lround (std::clamp (p.smooth, 0.0f, 1.0f) * (float) chunk / 2.0f) : 0;
        const int first = leader == 1 ? 1 : 0;
        writePicks (first, chunk, h, now);
        writePicks (1 - first, chunk, h, now);
        for (int s = 0; s < 2; ++s)
            for (int n = 0; n < nPicks[s]; ++n) taken[s][(size_t) picks[s][(size_t) n]] = 0;

        const double tot = sp[0] + sp[1];
        if (tot > 0.0) lastShareA = sp[0] / tot;
        shareEma += (float) (blockS / 0.1) * ((float) lastShareA - shareEma);   // ~100 ms smoothing for the UI
        shareEma = std::clamp (shareEma, 0.0f, 1.0f);

        // 4) play, then let what was just played fade (Memory = what's left after 1 s)
        const float mem = std::clamp (p.memory, 0.0f, 1.0f);
        const float decay = mem >= 1.0f ? 1.0f : (mem > 0.0f ? std::pow (mem, (float) (L / sr)) : 0.0f);
        float offset[2] = { 0.0f, 0.0f };
        int rampLen = 0;
        if (declick)
        {
            for (int c = 0; c < ch; ++c) offset[c] = lastOut[c] - buf[c][(size_t) pos];
            rampLen = std::max (1, std::min (m, (int) (0.004 * sr)));
            declick = false;
        }
        for (int i = 0; i < m; ++i)
        {
            const int at = (pos + i) % L;
            const float frac = (float) (off + i) / (float) block;
            const float gOut = gOut0 + (gOut1 - gOut0) * frac;
            const float mx = mix0 + (mix1 - mix0) * frac;
            for (int c = 0; c < ch; ++c)
            {
                float wet = buf[c][(size_t) at];
                if (i < rampLen) wet += offset[c] * (1.0f - (float) i / (float) rampLen);
                if (decay < 1.0f) buf[c][(size_t) at] *= decay;
                const float dr = dry[0][c][(size_t) (off + i)] + dry[1][c][(size_t) (off + i)];
                outFifo[c][(size_t) (off + i)] = softLimit (((1.0f - mx) * dr + mx * wet) * gOut);
                lastOut[c] = wet;
            }
        }
        pos = (pos + m) % L;
        clock += (uint64_t) m;
    }

    // ---------------------------------------------------------------- state
    double sr = 48000.0;
    int ch = 2, block = 480, maxL = 0, L = 0, pos = 0, fifoPos = 0, curChunk = -1, rollIdx = 0, dirtyMax = 0;
    uint64_t clock = 0;
    int leader = 0;
    double holdLeft = 0.0, lastShareA = 0.5;
    double total[2] {}, bank[2] {};
    int pending[2] {}, nPicks[2] {};
    float shareEma = 0.5f, sideEma = 0.0f;
    bool declick = false, firstBlock = true;
    float lastOut[2] {};
    float gB0 = 1, gOut0 = 1, mix0 = 1;
    Rng rng;

    std::vector<float> target[2][2], model[2][2], inFifo[2][2], dry[2][2];   // [side][channel]
    std::vector<float> buf[2], outFifo[2];
    std::vector<float> d[2];             // pending change per sample, summed over channels
    std::vector<double> sum[2];          // pending change per chunk
    std::vector<uint8_t> taken[2];
    std::vector<int> picks[2];
    std::vector<uint8_t> owner, scratchB;
    std::vector<uint32_t> stamp, scratchU;
    std::vector<float> mask, scratchF, scores;
    std::vector<int> touched, candJ;
    std::vector<double> candE;
    static constexpr int maxCandidates = 8192;
};

} // namespace bb
