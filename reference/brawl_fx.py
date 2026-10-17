"""
Bandwidth Brawl: two inputs fight over one bandwidth budget for one loop buffer.

One loop buffer plays on repeat. Input A and input B each keep a *target*
(what the loop would contain if that input had its way) and a *model* (what
that side last wrote at each position). A side's pending change is where its
target differs from its model: the parts where that input has changed since
it last wrote there. Every control block (10 ms) the budget is split between
A and B by how much each has pending (tilted by Bias, more or less
winner-take-all by Sharpness, held by Hold, unused budget banked by Savings),
and each side writes its most-changed chunks into the shared buffer.
A write by B covers A's audio, and A only takes that ground back once A
changes there. A silent or unchanging input claims nothing.
What you hear is the buffer.

All parameters are per-block arrays so they can be automated.
"""
import numpy as np

BLOCK_MS = 10.0
SNAP_EVERY = 5          # territory snapshot every 5 blocks (50 ms)
SNAP_BINS = 240         # resolution of the territory strip
EPS = 1e-6              # errors below this count as "nothing to fix"


def db_to_lin(db):
    return 10.0 ** (np.asarray(db, dtype=np.float64) / 20.0)


def soft_limit(x, knee=0.8):
    a = np.abs(x)
    over = a > knee
    if not np.any(over):
        return x
    y = x.copy()
    head = 1.0 - knee
    y[over] = np.sign(x[over]) * (knee + head * np.tanh((a[over] - knee) / head))
    return y


def quantize(x, bits):
    levels = float(2 ** (int(bits) - 1) - 1)
    levels = max(levels, 1.0)
    return np.round(np.clip(x, -1.0, 1.0) * levels) / levels


def split_share(claim_a, claim_b, w_a, w_b, sharp):
    """Fraction of the budget that goes to A."""
    ca, cb = claim_a * w_a, claim_b * w_b
    if ca <= 0 and cb <= 0:
        tot = w_a + w_b
        return 0.5 if tot <= 0 else w_a / tot
    if cb <= 0:
        return 1.0
    if ca <= 0:
        return 0.0
    if sharp >= 0.999:
        return 1.0 if ca > cb else (0.0 if cb > ca else 0.5)
    k = 1.0 / (1.0 - sharp)
    # 1 / (1 + (cb/ca)^k), computed in log space so it can't overflow
    z = k * (np.log(cb) - np.log(ca))
    if z > 50:
        return 0.0
    if z < -50:
        return 1.0
    return float(1.0 / (1.0 + np.exp(z)))


class BrawlFX:
    """
    Stateful processor. Call step(n_blocks) until it returns True, then read
    self.out (channels x samples) and the stats arrays.

    a, b:   float arrays (channels x samples), same sample rate and shape
    params: dict of per-block arrays (scalars are broadcast), see PARAM_NAMES
    """

    PARAM_NAMES = ("budget_kbs", "bias", "sharpness", "hold_ms", "savings_ms",
                   "loop_ms", "chunk", "smooth", "memory", "bits", "randomness",
                   "b_trim_db", "mix", "out_gain_db")

    def __init__(self, a, b, sr, params, seed=1, block_ms=BLOCK_MS):
        self.a = np.atleast_2d(np.asarray(a, dtype=np.float32))
        self.bsig = np.atleast_2d(np.asarray(b, dtype=np.float32))
        assert self.a.shape == self.bsig.shape, "A and B must have the same shape"
        self.ch, self.n = self.a.shape
        self.sr = int(sr)
        self.block = max(16, int(round(self.sr * block_ms / 1000.0)))
        self.n_blocks = int(np.ceil(self.n / self.block))
        self.params = {}
        for k in self.PARAM_NAMES:
            arr = np.asarray(params[k], dtype=np.float64).ravel()
            if arr.size == 1:
                arr = np.full(self.n_blocks, arr[0])
            if arr.size < self.n_blocks:
                arr = np.concatenate([arr, np.full(self.n_blocks - arr.size, arr[-1])])
            self.params[k] = arr
        self.rng = np.random.default_rng(int(seed))
        self.out = np.zeros((self.ch, self.n), dtype=np.float32)

        # loop state
        self.L = None
        self.tA = self.tB = self.buf = None
        self.owner = None        # 0 = never written, 1 = A, 2 = B (int8, length L)
        self.last = None         # block index of last write (int32, length L)
        self.pos = 0
        self.last_out = np.zeros(self.ch)
        self.declick = False

        # fight state
        self.bank = [0.0, 0.0]   # banked budget, in channel-samples
        self.leader = 0          # 0 = none yet, 1 = A, 2 = B
        self.hold_left = 0.0     # ms

        # stats for the UI
        self.share_a = np.full(self.n_blocks, 0.5, dtype=np.float32)   # actual spend split
        self.snaps = []          # list of (pos_frac, own, fresh, amp) per SNAP_EVERY blocks
        self.bi = 0

    # ---- loop buffer -------------------------------------------------------
    def _resize(self, new_L):
        if self.L is None:
            self.L = new_L
            z = lambda: np.zeros((self.ch, new_L))
            self.tA, self.tB, self.buf = z(), z(), z()
            self.mA, self.mB = z(), z()
            self.dA, self.dB = np.zeros(new_L), np.zeros(new_L)
            self.chunk = None
            self.owner = np.zeros(new_L, dtype=np.int8)
            self.last = np.full(new_L, -10**6, dtype=np.int64)
            self.pos = 0
            return
        old_L = self.L
        src = np.linspace(0, old_L, new_L, endpoint=False)
        idx = np.arange(old_L + 1)

        def stretch(x):
            ext = np.concatenate([x, x[:, :1]], axis=1)
            return np.stack([np.interp(src, idx, ext[c]) for c in range(self.ch)])

        near = np.minimum((src).astype(np.int64), old_L - 1)
        self.tA, self.tB, self.buf = stretch(self.tA), stretch(self.tB), stretch(self.buf)
        self.mA, self.mB = stretch(self.mA), stretch(self.mB)
        self.dA = np.abs(self.tA - self.mA).sum(axis=0)
        self.dB = np.abs(self.tB - self.mB).sum(axis=0)
        self.chunk = None
        self.owner = self.owner[near]
        self.last = self.last[near]
        self.pos = int(self.pos * new_L / old_L) % new_L
        self.L = new_L
        self.declick = True

    # ---- choosing chunks ---------------------------------------------------
    def _pick(self, err, avail, cost, taken, r):
        """Pick up to avail//cost chunks with the largest error. Returns indices, spent."""
        k = int(avail // cost)
        if k <= 0:
            return np.empty(0, dtype=np.int64), 0.0
        cand = np.flatnonzero(err > EPS)
        if taken is not None and cand.size:
            cand = cand[~taken[cand]]
        nc = cand.size
        if nc == 0:
            return np.empty(0, dtype=np.int64), 0.0
        k = min(k, nc)
        if k == nc:
            return cand, k * cost
        e = err[cand]
        score = e / (e.max() + 1e-12)
        if r > 0:
            score = (1.0 - r) * score + r * self.rng.random(nc)
        pick = cand[np.argpartition(score, -k)[-k:]]
        return pick, k * cost

    def _set_d(self, side, cols):
        """Recompute a side's pending change at `cols` and keep chunk sums in step."""
        t, mdl, d, s = (self.tA, self.mA, self.dA, self.sA) if side == 1 else \
                       (self.tB, self.mB, self.dB, self.sB)
        new = np.abs(t[:, cols] - mdl[:, cols]).sum(axis=0)
        np.add.at(s, cols // self.chunk, new - d[cols])
        d[cols] = new

    def _write(self, chunks, chunk, h, side, b):
        if chunks.size == 0:
            return
        L = self.L
        target, model = (self.tA, self.mA) if side == 1 else (self.tB, self.mB)
        if h == 0:
            cols = (chunks[:, None] * chunk + np.arange(chunk)[None, :]).ravel()
            cols = cols[cols < L]
            self.buf[:, cols] = target[:, cols]
            model[:, cols] = target[:, cols]
            self._set_d(side, cols)
            self.owner[cols] = side
            self.last[cols] = b
            return
        ramp = (np.arange(h) + 0.5) / h
        win = np.concatenate([ramp, np.ones(chunk), ramp[::-1]])
        off = np.arange(win.size) - h
        starts = chunks * chunk
        cols = (starts[:, None] + off[None, :])
        # the last chunk may be shorter than `chunk`; keep its window inside it
        valid = (cols - starts[:, None]) < np.minimum(chunk, L - starts)[:, None] + h
        cols = cols % L
        w = np.broadcast_to(win, cols.shape)[valid]
        cols = cols[valid]
        W = np.zeros(L)
        np.maximum.at(W, cols, w)
        nz = np.flatnonzero(W)
        Wn = W[nz]
        self.buf[:, nz] += Wn[None, :] * (target[:, nz] - self.buf[:, nz])
        # the side counts its whole crossfaded window as written, so the fade
        # edges don't leave tiny leftovers that keep claiming budget forever
        model[:, nz] = target[:, nz]
        self._set_d(side, nz)
        strong = nz[Wn >= 0.5]
        self.owner[strong] = side
        self.last[strong] = b

    # ---- one block ---------------------------------------------------------
    def _process_block(self, b):
        p = {k: v[b] for k, v in self.params.items()}
        s0 = b * self.block
        s1 = min(self.n, s0 + self.block)
        m = s1 - s0
        block_s = m / self.sr

        new_L = max(8, int(round(p["loop_ms"] * self.sr / 1000.0)))
        if self.L is None or abs(new_L - self.L) > max(1, 0.01 * self.L):
            self._resize(new_L)
        L = self.L

        bits = int(np.clip(round(p["bits"]), 2, 16))
        dryA = self.a[:, s0:s1].astype(np.float64)
        dryB = self.bsig[:, s0:s1] * db_to_lin(p["b_trim_db"])

        # 1) per-chunk pending-change sums (rebuilt only when the chunk size changes)
        chunk = int(np.clip(round(p["chunk"]), 1, L))
        n_chunks = int(np.ceil(L / chunk))
        if chunk != self.chunk:
            self.chunk = chunk
            pad = n_chunks * chunk - L

            def sums(d):
                e = np.concatenate([d, np.zeros(pad)]) if pad else d
                return e.reshape(n_chunks, chunk).sum(axis=1)

            self.sA, self.sB = sums(self.dA), sums(self.dB)

        # 2) both inputs write their own target at the positions playing now
        positions = (self.pos + np.arange(m)) % L
        self.tA[:, positions] = quantize(dryA, bits)
        self.tB[:, positions] = quantize(dryB, bits)
        self._set_d(1, positions)
        self._set_d(2, positions)

        # chunk means (the last chunk may be short; that only slightly favours it)
        eA = np.maximum(self.sA, 0.0) / chunk
        eB = np.maximum(self.sB, 0.0) / chunk

        # 3) split the budget
        bias = float(np.clip(p["bias"], -1.0, 1.0))         # -1 = all A, +1 = all B
        wA, wB = (1.0 - bias) / 2.0, (1.0 + bias) / 2.0
        sharp = float(np.clip(p["sharpness"], 0.0, 1.0))
        share = split_share(eA.sum(), eB.sum(), wA, wB, sharp)

        raw_leader = 1 if share > 0.5 else (2 if share < 0.5 else self.leader)
        hold_ms = max(0.0, float(p["hold_ms"]))
        if self.leader == 0 or (raw_leader != self.leader and self.hold_left <= 0):
            if raw_leader != self.leader:
                self.leader = raw_leader
                self.hold_left = hold_ms
        elif raw_leader != self.leader and self.hold_left > 0:
            # the current leader keeps the bigger part while its hold lasts
            share = 1.0 - share
        self.hold_left -= block_s * 1000.0

        bytes_ps = bits / 8.0
        allowance = max(0.0, p["budget_kbs"]) * 1000.0 * block_s / bytes_ps  # channel-samples
        cost = chunk * self.ch
        # Savings: an idle side (nothing changed) keeps earning its fair share
        # (set by Bias) into its bank instead of handing it to the other side
        sav_s = max(0.0, float(p["savings_ms"])) / 1000.0
        cap = max(cost, sav_s * max(0.0, p["budget_kbs"]) * 1000.0 / bytes_ps)
        keep = [0.0, 0.0]
        if sav_s > 0:
            if not np.any(eA > EPS):
                keep[0] = wA * allowance
            if not np.any(eB > EPS):
                keep[1] = wB * allowance
        rest = allowance - keep[0] - keep[1]
        avail = [share * rest + keep[0] + self.bank[0],
                 (1.0 - share) * rest + keep[1] + self.bank[1]]

        r = float(np.clip(p["randomness"], 0.0, 1.0))
        pA, spA = self._pick(eA, avail[0], cost, None, r)
        pB, spB = self._pick(eB, avail[1], cost, None, r)
        left = [avail[0] - spA, avail[1] - spB]

        # 4) each side may keep up to `cap` for later; anything above goes to the other side
        over = [max(0.0, left[0] - cap), max(0.0, left[1] - cap)]
        left = [left[0] - over[0], left[1] - over[1]]
        if over[1] > 0:
            takenA = np.zeros(n_chunks, dtype=bool); takenA[pA] = True
            extra, sp = self._pick(eA, over[1] + left[0], cost, takenA, r)
            pA = np.concatenate([pA, extra]); spA += sp
            left[0] = left[0] + over[1] - sp
        if over[0] > 0:
            takenB = np.zeros(n_chunks, dtype=bool); takenB[pB] = True
            extra, sp = self._pick(eB, over[0] + left[1], cost, takenB, r)
            pB = np.concatenate([pB, extra]); spB += sp
            left[1] = left[1] + over[0] - sp
        self.bank = [min(left[0], cap), min(left[1], cap)]

        # 5) write: the leader writes last, so it wins chunks both sides picked
        h = int(round(np.clip(p["smooth"], 0.0, 1.0) * chunk / 2)) if chunk >= 4 else 0
        order = [(pB, 2), (pA, 1)] if self.leader == 1 else [(pA, 1), (pB, 2)]
        for picks, side in order:
            self._write(picks, chunk, h, side, b)

        tot = spA + spB
        self.share_a[b] = (spA / tot) if tot > 0 else (self.share_a[b - 1] if b else 0.5)

        # 6) play, then let what was just played fade (Memory = what's left after 1 s)
        wet = self.buf[:, positions].copy()
        if self.declick:
            offset = self.last_out - wet[:, 0]
            ramp = np.clip(1.0 - np.arange(m) / min(m, max(1, int(0.004 * self.sr))), 0, 1)
            wet += offset[:, None] * ramp[None, :]
            self.declick = False
        mem = float(np.clip(p["memory"], 0.0, 1.0))
        if mem < 1.0:
            g = mem ** (L / self.sr) if mem > 0 else 0.0
            self.buf[:, positions] *= g

        if b % SNAP_EVERY == 0:
            self._snapshot(b)

        self.pos = (self.pos + m) % L
        mix = float(np.clip(p["mix"], 0.0, 1.0))
        y = (1.0 - mix) * (dryA + dryB) + mix * wet
        y *= db_to_lin(p["out_gain_db"])
        self.out[:, s0:s1] = soft_limit(y)
        self.last_out = wet[:, -1]

    def _snapshot(self, b):
        L = self.L
        edges = (np.arange(SNAP_BINS) * L) // SNAP_BINS
        edges = np.unique(edges)
        amp = np.maximum.reduceat(np.abs(self.buf).max(axis=0), edges)
        a_cnt = np.add.reduceat((self.owner == 1).astype(np.int32), edges)
        b_cnt = np.add.reduceat((self.owner == 2).astype(np.int32), edges)
        age_s = (b - self.last) * self.block / self.sr
        fresh = np.add.reduceat(np.clip(1.0 - age_s, 0.0, 1.0), edges)
        sizes = np.diff(np.append(edges, L))
        fresh = fresh / sizes
        own = np.where((a_cnt == 0) & (b_cnt == 0), 0, np.where(a_cnt >= b_cnt, 1, 2))
        if edges.size < SNAP_BINS:  # tiny loops: stretch to the full width
            sel = np.minimum((np.arange(SNAP_BINS) * edges.size) // SNAP_BINS, edges.size - 1)
            amp, own, fresh = amp[sel], own[sel], fresh[sel]
        self.snaps.append((
            self.pos / L,
            own.astype(np.uint8),
            np.round(np.clip(fresh, 0, 1) * 255).astype(np.uint8),
            np.round(np.clip(amp, 0, 1) * 255).astype(np.uint8),
        ))

    # ---- driving -----------------------------------------------------------
    def step(self, n_blocks=200):
        end = min(self.n_blocks, self.bi + n_blocks)
        while self.bi < end:
            self._process_block(self.bi)
            self.bi += 1
        return self.bi >= self.n_blocks

    @property
    def progress(self):
        return self.bi / max(1, self.n_blocks)

    def render(self):
        while not self.step(1000):
            pass
        return self.out

    def snap_arrays(self):
        """Territory snapshots packed as flat uint8 arrays for the browser."""
        n = len(self.snaps)
        pos = np.array([s[0] for s in self.snaps], dtype=np.float32)
        own = np.concatenate([s[1] for s in self.snaps]) if n else np.zeros(0, np.uint8)
        fresh = np.concatenate([s[2] for s in self.snaps]) if n else np.zeros(0, np.uint8)
        amp = np.concatenate([s[3] for s in self.snaps]) if n else np.zeros(0, np.uint8)
        return pos, own, fresh, amp


def process(a, b, sr, params, seed=1):
    return BrawlFX(a, b, sr, params, seed).render()
