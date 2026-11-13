// fx_core.hpp: the shared DSP of the Resona sound modules.
//
// Header only. No heap, no C library (only <resona/math.h>), every loop bounded, every
// feedback path flushed and bounded. Long buffers come from one host-owned PSRAM pool that
// main.cpp hands to dsp_init(); the small state lives in the module's State.
//
// The master copy lives in "Sound Modules/_shared/". tools/sync_shared.sh copies it into each
// module's src/ so every project still builds on its own. Edit the master, then sync.
#ifndef FX_CORE_HPP
#define FX_CORE_HPP

#include <stdint.h>

#include <resona/math.h>

namespace fx {

constexpr float kRate = 48000.0f;
constexpr float kInvRate = 1.0f / 48000.0f;
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return x < 0.0f ? -x : x; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline float flush(float x) { return (x > -1.0e-18f && x < 1.0e-18f) ? 0.0f : x; }
inline float pct(float p) { return clampf(p * 0.01f, 0.0f, 1.0f); }
inline float ms_to_samples(float ms) { return ms * (kRate * 0.001f); }

// One-pole coefficient for a time constant in ms, or for a corner frequency in Hz.
inline float coef_ms(float ms) { return ms <= 0.02f ? 1.0f : 1.0f - resona_expf(-1000.0f / (ms * kRate)); }
inline float coef_hz(float hz) { return clampf(1.0f - resona_expf(-kTwoPi * hz * kInvRate), 0.0f, 1.0f); }

// sin(2 pi p) for p in [0, 1), cheap enough for every sample (error < 0.1%).
inline float sin01(float p)
{
    const float x = 2.0f * p - 1.0f;
    float y = 4.0f * x * (1.0f - absf(x));
    y = 0.225f * (y * absf(y) - y) + y;
    return -y;
}
inline float wrap01(float p) { return p >= 1.0f ? p - 1.0f : (p < 0.0f ? p + 1.0f : p); }

// Smooth saturation: unity slope at 0, reaches +-1 at +-3 and stays there.
inline float sat(float x)
{
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Asymmetric soft clip for overdrive: a little even-order colour, bounded, DC handled outside.
inline float sat_asym(float x) { return x >= 0.0f ? sat(x) : sat(x * 0.85f) * 1.12f; }

// tan(x) for 0 <= x < 0.6 (cutoffs below ~9 kHz): used by the per-sample SVF.
inline float tan_small(float x)
{
    const float x2 = x * x;
    return x * (1.0f + x2 * (0.333333f + x2 * (0.133333f + x2 * 0.053968f)));
}

// A parameter that glides to its target instead of jumping (no zipper noise).
struct Smooth {
    float y, t, c;
    void init(float v, float ms) { y = t = v; c = coef_ms(ms); }
    void set(float v) { t = v; }
    void snap() { y = t; }
    float next() { y += c * (t - y); return y; }
};

struct LowPass1 {
    float y, a;
    void set_hz(float hz) { a = coef_hz(hz); }
    void reset() { y = 0.0f; }
    float process(float x) { y = flush(y + a * (x - y)); return y; }
};

struct HighPass1 {
    LowPass1 lp;
    void set_hz(float hz) { lp.set_hz(hz); }
    void reset() { lp.reset(); }
    float process(float x) { return x - lp.process(x); }
};

// First-order all-pass (dispersion: spring reverbs, phasers).
struct Allpass1 {
    float a, x1, y1;
    void reset() { x1 = y1 = 0.0f; }
    float process(float x)
    {
        const float y = flush(a * (x - y1) + x1);
        x1 = x;
        y1 = y;
        return y;
    }
};

// RBJ biquad, transposed direct form II.
struct Biquad {
    enum Kind { LP, HP, BP, PEAK, LOWSHELF, HIGHSHELF };
    float b0, b1, b2, a1, a2, z1, z2;
    void reset() { z1 = z2 = 0.0f; }
    void set(Kind k, float hz, float q, float db = 0.0f)
    {
        const float w = kTwoPi * clampf(hz, 10.0f, 20000.0f) * kInvRate;
        const float cw = resona_cosf(w), sw = resona_sinf(w);
        const float alpha = sw / (2.0f * (q > 0.05f ? q : 0.05f));
        const float A = resona_db_to_lin(db * 0.5f);
        float n0 = 1, n1 = 0, n2 = 0, d0 = 1, d1 = 0, d2 = 0;
        switch (k) {
        case LP: n0 = (1 - cw) * 0.5f; n1 = 1 - cw; n2 = n0; d0 = 1 + alpha; d1 = -2 * cw; d2 = 1 - alpha; break;
        case HP: n0 = (1 + cw) * 0.5f; n1 = -(1 + cw); n2 = n0; d0 = 1 + alpha; d1 = -2 * cw; d2 = 1 - alpha; break;
        case BP: n0 = alpha; n1 = 0; n2 = -alpha; d0 = 1 + alpha; d1 = -2 * cw; d2 = 1 - alpha; break;
        case PEAK:
            n0 = 1 + alpha * A; n1 = -2 * cw; n2 = 1 - alpha * A;
            d0 = 1 + alpha / A; d1 = -2 * cw; d2 = 1 - alpha / A;
            break;
        case LOWSHELF: {
            const float s = 2.0f * resona_sqrtf(A) * sw * 0.70710678f;
            n0 = A * ((A + 1) - (A - 1) * cw + s); n1 = 2 * A * ((A - 1) - (A + 1) * cw);
            n2 = A * ((A + 1) - (A - 1) * cw - s); d0 = (A + 1) + (A - 1) * cw + s;
            d1 = -2 * ((A - 1) + (A + 1) * cw); d2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case HIGHSHELF: {
            const float s = 2.0f * resona_sqrtf(A) * sw * 0.70710678f;
            n0 = A * ((A + 1) + (A - 1) * cw + s); n1 = -2 * A * ((A - 1) + (A + 1) * cw);
            n2 = A * ((A + 1) + (A - 1) * cw - s); d0 = (A + 1) - (A - 1) * cw + s;
            d1 = 2 * ((A - 1) - (A + 1) * cw); d2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        }
        const float inv = 1.0f / d0;
        b0 = n0 * inv; b1 = n1 * inv; b2 = n2 * inv; a1 = d1 * inv; a2 = d2 * inv;
    }
    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = flush(b1 * x - a1 * y + z2);
        z2 = flush(b2 * x - a2 * y);
        return y;
    }
};

// Topology-preserving state-variable filter; the cutoff may move every sample.
struct Svf {
    float ic1, ic2;
    void reset() { ic1 = ic2 = 0.0f; }
    // g = tan(pi fc / fs), k = 1 / Q
    void process(float x, float g, float k, float &lp, float &bp)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1, a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flush(clampf(2.0f * v1 - ic1, -8.0f, 8.0f));
        ic2 = flush(clampf(2.0f * v2 - ic2, -8.0f, 8.0f));
        lp = v2;
        bp = v1;
    }
};

// Bump allocator over the module's PSRAM buffer.
struct Pool {
    float *p;
    uint32_t left;
    float *take(uint32_t n)
    {
        if (!p || n > left) return nullptr;
        float *r = p;
        p += n;
        left -= n;
        return r;
    }
};

// A circular delay line. Read first, then push: at(d) is the sample pushed d pushes ago,
// valid for 1 <= d <= n.
struct Line {
    float *b;
    uint32_t n, w;
    bool attach(Pool &pool, uint32_t len)
    {
        b = pool.take(len);
        n = len;
        w = 0;
        return b != nullptr;
    }
    void clear()
    {
        if (b) for (uint32_t i = 0; i < n; ++i) b[i] = 0.0f;
        w = 0;
    }
    void push(float x) { b[w] = x; w = (w + 1u == n) ? 0u : w + 1u; }
    float at(uint32_t d) const
    {
        int32_t i = (int32_t)w - (int32_t)d;
        if (i < 0) i += (int32_t)n;
        return b[i];
    }
    float lin(float d) const
    {
        d = clampf(d, 1.0f, (float)n - 1.0f);
        const uint32_t i = (uint32_t)d;
        const float f = d - (float)i;
        const float a = at(i);
        return a + f * (at(i + 1u) - a);
    }
    // Read at an absolute (fractional) buffer index, wrapped once: grains keep their own heads.
    float read_abs(float p) const
    {
        if (p < 0.0f) p += (float)n;
        if (p >= (float)n) p -= (float)n;
        p = clampf(p, 0.0f, (float)n - 1.001f);
        const uint32_t i = (uint32_t)p;
        const float f = p - (float)i;
        const float a = b[i];
        return a + f * (b[i + 1u == n ? 0u : i + 1u] - a);
    }
    // The index the next push() writes, as a float, for grain anchors.
    float head() const { return (float)w; }
    // 4-point Hermite: clean modulated reads (chorus, tape, pitch).
    float cub(float d) const
    {
        d = clampf(d, 2.0f, (float)n - 3.0f);
        const uint32_t i = (uint32_t)d;
        const float t = d - (float)i;
        const float xm1 = at(i - 1u), x0 = at(i), x1 = at(i + 1u), x2 = at(i + 2u);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }
};

// Schroeder all-pass on a Line, optionally with a modulated length.
struct Diffuser {
    Line l;
    uint32_t len;
    float g;
    bool attach(Pool &pool, uint32_t length, uint32_t extra, float gain)
    {
        len = length;
        g = gain;
        return l.attach(pool, length + extra + 4u);
    }
    void clear() { l.clear(); }
    float process(float x)
    {
        const float d = l.at(len);
        const float v = flush(x + g * d);
        l.push(v);
        return d - g * v;
    }
    float process_mod(float x, float delay)
    {
        const float d = l.lin(delay);
        const float v = flush(x + g * d);
        l.push(v);
        return d - g * v;
    }
};

struct Lfo {
    float ph, inc;
    void set_hz(float hz) { inc = hz * kInvRate; }
    float sine() { const float s = sin01(ph); ph = wrap01(ph + inc); return s; }
    float tri() { const float t = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; ph = wrap01(ph + inc); return t; }
};

// Slow random drift (tape flutter, organic modulation): smoothed sample-and-hold noise.
struct Drift {
    uint32_t seed, seed0;
    float target, y, count, period, c;
    void init(uint32_t s, float hz)
    {
        seed0 = s ? s : 1u;
        period = kRate / hz;
        c = coef_hz(hz * 1.5f);
        reset();
    }
    void reset() { seed = seed0; y = target = 0.0f; count = 0.0f; }
    float rnd()
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return (float)(seed >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    float next()
    {
        if ((count -= 1.0f) <= 0.0f) { count = period; target = rnd(); }
        y += c * (target - y);
        return y;
    }
};

struct Follower {
    float y, att, rel;
    void set(float att_ms, float rel_ms) { att = coef_ms(att_ms); rel = coef_ms(rel_ms); }
    void reset() { y = 0.0f; }
    float process(float x)
    {
        const float a = absf(x);
        y = flush(y + (a > y ? att : rel) * (a - y));
        return y;
    }
};

// Feed-forward compressor with a soft knee and automatic make-up, set by one AMOUNT macro
// (0 = off). The gain is computed every 8 samples and glided per sample.
struct Compressor {
    Follower det;
    float thr_db, slope, makeup_db, g, gt;
    uint32_t tick;
    void init() { det.set(4.0f, 180.0f); g = gt = 1.0f; tick = 0; set_amount(0.0f); }
    void reset() { det.reset(); g = gt = 1.0f; tick = 0; }
    void set_amount(float a)   // 0..1
    {
        a = clampf(a, 0.0f, 1.0f);
        thr_db = -10.0f - 26.0f * a;
        const float ratio = 1.0f + 7.0f * a;
        slope = 1.0f / ratio - 1.0f;
        makeup_db = -thr_db * (-slope) * 0.5f;   // half of the full-scale reduction
    }
    float gain_now() const { return g; }
    float process(float x)
    {
        const float e = det.process(x);
        if ((tick++ & 7u) == 0u) {
            const float lvl = resona_lin_to_db(e + 1.0e-9f);
            const float over = lvl - thr_db;
            const float knee = 8.0f;
            float gr = 0.0f;
            if (over >= knee * 0.5f) gr = slope * over;
            else if (over > -knee * 0.5f) { const float o = over + knee * 0.5f; gr = slope * o * o / (2.0f * knee); }
            gt = resona_db_to_lin(gr + makeup_db);
        }
        g += 0.02f * (gt - g);
        return x * g;
    }
};

// Noise gate with hysteresis and hold. Threshold in dBFS; -80 means off.
struct Gate {
    Follower env;
    float open_lin, close_lin, g, hold, hold_len, a_open, a_close;
    bool off;
    void init() { env.set(0.3f, 25.0f); g = 1.0f; hold = 0.0f; hold_len = 0.05f * kRate; a_open = coef_ms(0.8f); a_close = coef_ms(70.0f); set_threshold(-80.0f); }
    void reset() { env.reset(); g = off ? 1.0f : 0.0f; hold = 0.0f; }
    void set_threshold(float db)
    {
        off = db <= -79.5f;
        open_lin = resona_db_to_lin(db);
        close_lin = open_lin * 0.5f;
    }
    float process(float x)
    {
        if (off) { g += a_open * (1.0f - g); return x * g; }
        const float e = env.process(x);
        float target;
        if (e > open_lin) { hold = hold_len; target = 1.0f; }
        else if (hold > 0.0f || e > close_lin) { hold -= 1.0f; target = 1.0f; }
        else target = 0.0f;
        g += (target > g ? a_open : a_close) * (target - g);
        return x * g;
    }
};

// Output stage: a peak limiter (1 ms attack, 150 ms release, -1 dBFS) and a soft ceiling.
struct Limiter {
    float env, att, rel;
    void init() { env = 0.0f; att = coef_ms(1.0f); rel = coef_ms(150.0f); }
    void reset() { env = 0.0f; }
    float process(float x)
    {
        const float a = absf(x);
        env = flush(env + (a > env ? att : rel) * (a - env));
        const float thr = 0.89f;
        if (env > thr) x *= thr / env;
        const float m = absf(x);
        if (m <= 0.8f) return x;
        const float t = (m - 0.8f) * 5.0f;
        const float y = 0.8f + 0.19f * t / (1.0f + t);
        return x < 0.0f ? -y : y;
    }
};

// Two-grain delay-line pitch shifter (for shimmer). Ratio 2 = octave up, 0.5 = octave down.
struct Pitch {
    static constexpr uint32_t kWindow = 2048u;
    static constexpr uint32_t floats() { return kWindow + 16u; }
    Line l;
    float ph, inc;
    bool attach(Pool &pool) { ph = 0.0f; return l.attach(pool, floats()); }
    void clear() { l.clear(); ph = 0.0f; }
    void set_ratio(float r) { inc = (1.0f - r) / (float)kWindow; }
    float process(float x)
    {
        l.push(x);
        ph = wrap01(ph + inc);
        const float p2 = wrap01(ph + 0.5f);
        const float w1 = 0.5f - 0.5f * sin01(wrap01(ph + 0.25f));   // sin^2(pi ph): 0 at the seam
        const float a = l.lin(2.0f + ph * (float)kWindow);
        const float b = l.lin(2.0f + p2 * (float)kWindow);
        return w1 * a + (1.0f - w1) * b;
    }
};

// A Dattorro-style plate/hall: pre-delay, input low cut and bandwidth, four input diffusers,
// a two-branch modulated tank with damping. SIZE scales every length (0.4 room .. 1.6 hall),
// fixed per module so the pool is known at compile time.
struct Plate {
    static constexpr uint32_t L(uint32_t base, float size) { return (uint32_t)((float)base * size * 1.6128f + 0.5f); }
    static constexpr uint32_t kExc = 48u;
    static constexpr uint32_t floats(float size, float max_pre_ms)
    {
        return (uint32_t)(max_pre_ms * 48.0f) + 8u
             + L(142, size) + L(107, size) + L(379, size) + L(277, size) + 16u
             + L(672, size) + L(908, size) + 2u * (kExc + 4u)
             + L(4453, size) + L(1800, size) + L(3720, size)
             + L(4217, size) + L(2656, size) + L(3163, size) + 16u;
    }

    Line pre, da1, da2, db1, db2;
    Diffuser in1, in2, in3, in4, ma, mb, aa, ab;
    LowPass1 bw, dampa, dampb;
    HighPass1 lowcut;
    Lfo lfo;
    Smooth pre_d;
    float decay, depth, fba, fbb, half_s;
    uint32_t la1, la2, lb1, lb2, lma, lmb;
    uint32_t t_b1a, t_b1b, t_bab, t_b2, t_a1, t_aaa, t_a2;
    uint32_t t2_a1a, t2_a1b, t2_aaa, t2_a2, t2_b1, t2_bab, t2_b2;

    bool attach(Pool &p, float size, float max_pre_ms)
    {
        bool ok = pre.attach(p, (uint32_t)(max_pre_ms * 48.0f) + 8u);
        ok = in1.attach(p, L(142, size), 0, 0.75f) && ok;
        ok = in2.attach(p, L(107, size), 0, 0.75f) && ok;
        ok = in3.attach(p, L(379, size), 0, 0.625f) && ok;
        ok = in4.attach(p, L(277, size), 0, 0.625f) && ok;
        lma = L(672, size); lmb = L(908, size);
        ok = ma.attach(p, lma, kExc, -0.7f) && ok;
        ok = mb.attach(p, lmb, kExc, -0.7f) && ok;
        la1 = L(4453, size); la2 = L(3720, size); lb1 = L(4217, size); lb2 = L(3163, size);
        ok = da1.attach(p, la1) && ok;
        ok = aa.attach(p, L(1800, size), 0, 0.5f) && ok;
        ok = da2.attach(p, la2) && ok;
        ok = db1.attach(p, lb1) && ok;
        ok = ab.attach(p, L(2656, size), 0, 0.5f) && ok;
        ok = db2.attach(p, lb2) && ok;
        // output taps (Dattorro's table), both sides summed to mono
        t_b1a = L(266, size); t_b1b = L(2974, size); t_bab = L(1913, size); t_b2 = L(1996, size);
        t_a1 = L(1990, size); t_aaa = L(187, size); t_a2 = L(1066, size);
        t2_a1a = L(353, size); t2_a1b = L(3627, size); t2_aaa = L(1228, size); t2_a2 = L(2673, size);
        t2_b1 = L(2111, size); t2_bab = L(335, size); t2_b2 = L(121, size);
        half_s = (float)(lma + la1 + L(1800, size) + la2) * kInvRate;
        bw.set_hz(9000.0f);
        lowcut.set_hz(120.0f);
        dampa.set_hz(6000.0f);
        dampb.set_hz(6000.0f);
        lfo.ph = 0.0f;
        lfo.set_hz(0.7f);
        pre_d.init(1.0f, 120.0f);
        depth = 0.3f;
        set_decay(2.0f);
        fba = fbb = 0.0f;
        return ok;
    }
    void clear()
    {
        pre.clear(); da1.clear(); da2.clear(); db1.clear(); db2.clear();
        in1.clear(); in2.clear(); in3.clear(); in4.clear(); ma.clear(); mb.clear(); aa.clear(); ab.clear();
        bw.reset(); dampa.reset(); dampb.reset(); lowcut.reset();
        fba = fbb = 0.0f;
        lfo.ph = 0.0f;
        pre_d.snap();
    }
    void set_decay(float seconds)
    {
        const float s = clampf(seconds, 0.1f, 30.0f);
        // the tank applies `decay` twice per half-loop (after damping, and entering the
        // other branch), so each application carries half of the 60 dB / RT60 slope
        decay = clampf(resona_exp2f(-4.98289214f * half_s / s), 0.0f, 0.985f);
    }
    void set_damp_hz(float hz) { dampa.set_hz(hz); dampb.set_hz(hz); }
    void set_lowcut_hz(float hz) { lowcut.set_hz(hz); }
    void set_bandwidth_hz(float hz) { bw.set_hz(hz); }
    void set_predelay_ms(float ms) { pre_d.set(clampf(ms_to_samples(ms), 1.0f, (float)pre.n - 2.0f)); }
    void set_mod(float amount, float hz) { depth = clampf(amount, 0.0f, 1.0f); lfo.set_hz(hz); }

    float process(float x)
    {
        pre.push(x);
        float y = pre.lin(pre_d.next());
        y = bw.process(lowcut.process(y));
        y = in4.process(in3.process(in2.process(in1.process(y))));

        const float m = lfo.sine() * depth * (float)(kExc - 4u) * 0.5f;
        const float pa = fba, pb = fbb;

        // branch A
        float a = ma.process_mod(y + decay * pb, (float)lma + (float)(kExc / 2u) + m);
        float r = da1.at(la1); da1.push(a); a = r;
        a = dampa.process(a) * decay;
        a = aa.process(a);
        r = da2.at(la2); da2.push(a); fba = r;

        // branch B (quadrature modulation)
        float b = mb.process_mod(y + decay * pa, (float)lmb + (float)(kExc / 2u) - m);
        r = db1.at(lb1); db1.push(b); b = r;
        b = dampb.process(b) * decay;
        b = ab.process(b);
        r = db2.at(lb2); db2.push(b); fbb = r;

        const float left = db1.at(t_b1a) + db1.at(t_b1b) - ab.l.at(t_bab) + db2.at(t_b2)
                         - da1.at(t_a1) - aa.l.at(t_aaa) - da2.at(t_a2);
        const float right = da1.at(t2_a1a) + da1.at(t2_a1b) - aa.l.at(t2_aaa) + da2.at(t2_a2)
                          - db1.at(t2_b1) - ab.l.at(t2_bab) - db2.at(t2_b2);
        return 0.3f * (left + right);
    }
};

// A two-spring tank: dispersive all-pass chains inside short feedback delays give the
// "drip" and the metallic chirp of an amp's spring reverb. DWELL drives the tank.
struct Spring {
    static constexpr uint32_t kL1 = 1637u, kL2 = 2113u, kStages = 10u;
    static constexpr uint32_t floats() { return kL1 + kL2 + 8u; }
    Line l1, l2;
    Allpass1 c1[kStages], c2[kStages];
    LowPass1 tone1, tone2;
    HighPass1 hp_in, hp_out;
    float fb, drive;
    bool attach(Pool &p)
    {
        bool ok = l1.attach(p, kL1 + 4u);
        ok = l2.attach(p, kL2 + 4u) && ok;
        for (uint32_t i = 0; i < kStages; ++i) { c1[i].a = -0.62f; c2[i].a = -0.58f; }
        hp_in.set_hz(320.0f);
        hp_out.set_hz(180.0f);
        set_tone_hz(4200.0f);
        set_decay(0.8f);
        drive = 1.0f;
        return ok;
    }
    void clear()
    {
        l1.clear(); l2.clear();
        for (uint32_t i = 0; i < kStages; ++i) { c1[i].reset(); c2[i].reset(); }
        tone1.reset(); tone2.reset(); hp_in.reset(); hp_out.reset();
    }
    void set_tone_hz(float hz) { tone1.set_hz(hz); tone2.set_hz(hz * 0.9f); }
    void set_decay(float f) { fb = clampf(f, 0.0f, 0.9f); }
    void set_dwell(float d01) { drive = 0.6f + 3.4f * clampf(d01, 0.0f, 1.0f); }
    float process(float x)
    {
        const float in = sat(hp_in.process(x) * drive) * 0.5f;
        const float r1 = l1.at(kL1), r2 = l2.at(kL2);
        float v1 = in + fb * r1;
        float v2 = in - fb * r2;
        for (uint32_t i = 0; i < kStages; ++i) { v1 = c1[i].process(v1); v2 = c2[i].process(v2); }
        l1.push(flush(tone1.process(v1)));
        l2.push(flush(tone2.process(v2)));
        return hp_out.process(0.9f * (r1 + r2));
    }
};


// Deterministic xorshift noise: reset() replays the same sequence (the SDK's reset check).
struct Rng {
    uint32_t s, s0;
    void seed(uint32_t v) { s0 = v ? v : 1u; s = s0; }
    void reset() { s = s0; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }   // 0 .. 1
    float bi() { return uni() * 2.0f - 1.0f; }                            // -1 .. 1
};

// Two all-pass chains 90 degrees apart (Olli Niemitalo's design, about 20 Hz .. 20 kHz):
// the analytic signal for single-sideband frequency shifting.
struct Hilbert {
    float xa[4][2], ya[4][2], xb[4][2], yb[4][2], delayed;
    void reset()
    {
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 2; ++k) xa[i][k] = ya[i][k] = xb[i][k] = yb[i][k] = 0.0f;
        delayed = 0.0f;
    }
    static float stage(float x, float a, float *xs, float *ys)
    {
        const float y = flush(a * (x + ys[1]) - xs[1]);
        xs[1] = xs[0]; xs[0] = x;
        ys[1] = ys[0]; ys[0] = y;
        return y;
    }
    void process(float x, float &re, float &im)
    {
        static const float A[4] = {0.47940087f, 0.87621849f, 0.97659759f, 0.99749926f};   // squared coefficients
        static const float B[4] = {0.16175850f, 0.73302893f, 0.94534970f, 0.99059916f};
        float a = x, b = x;
        for (int i = 0; i < 4; ++i) { a = stage(a, A[i], xa[i], ya[i]); b = stage(b, B[i], xb[i], yb[i]); }
        re = delayed;
        delayed = a;
        im = b;
    }
};

// Quadrature oscillator for the frequency shifter: rotation, renormalised now and then.
struct Quadrature {
    float c, s, cw, sw;
    uint32_t tick;
    void reset() { c = 1.0f; s = 0.0f; tick = 0; }
    void set_hz(float hz) { const float w = kTwoPi * hz * kInvRate; cw = resona_cosf(w); sw = resona_sinf(w); }
    void next()
    {
        const float c2 = c * cw - s * sw;
        s = s * cw + c * sw;
        c = c2;
        if ((++tick & 63u) == 0u) { const float g = 1.5f - 0.5f * (c * c + s * s); c *= g; s *= g; }
    }
};

// PolyBLEP correction for a naive saw/square edge at phase t (0..1) with increment dt.
inline float blep(float t, float dt)
{
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// Sample-rate and bit-depth reduction (zero-order hold), as in early samplers.
struct Crusher {
    float hold, ph, ratio, step;
    void reset() { hold = 0.0f; ph = 1.0f; }
    void set(float amount01)
    {
        const float a = clampf(amount01, 0.0f, 1.0f);
        ratio = 1.0f - 0.94f * resona_powf(a, 0.7f);   // down to about 2.9 kHz
        const float bits = 16.0f - 11.0f * a;            // down to 5 bits
        step = resona_exp2f(1.0f - bits);
    }
    float process(float x)
    {
        ph += ratio;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            const float q = x / step;
            hold = (float)(int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f)) * step;
        }
        return hold;
    }
};


// Monophonic pitch tracker for guitar: a Schmitt trigger on the low-passed string times the
// rising crossings. log2hz holds the last confident pitch (glide it yourself); env is the
// input's envelope; `rose` is true on the sample a new cycle starts (analog-style dividers).
struct Tracker {
    Biquad lp, hp;
    Follower tenv, env_f;
    int side;
    float since, log2hz, env;
    bool rose;
    void init()
    {
        lp.set(Biquad::LP, 700.0f, 0.55f);
        hp.set(Biquad::HP, 60.0f, 0.7f);
        tenv.set(1.0f, 30.0f);
        env_f.set(2.0f, 100.0f);
        reset();
    }
    void reset()
    {
        lp.reset(); hp.reset(); tenv.reset(); env_f.reset();
        side = 0; since = 0.0f; log2hz = 7.0f; env = 0.0f; rose = false;
    }
    // `thr`: the level below which nothing is tracked (about 0.004 = -48 dBFS)
    void process(float x, float thr)
    {
        env = env_f.process(x);
        const float f = hp.process(lp.process(x));
        const float h = 0.25f * tenv.process(f) + 1.0e-5f;
        since += 1.0f;
        rose = false;
        if (side <= 0 && f > h) {
            side = 1;
            const float hz = kRate / since;
            if (hz > 65.0f && hz < 1400.0f && env > thr) log2hz = resona_log2f(hz);
            since = 0.0f;
            rose = true;
        } else if (side >= 0 && f < -h) {
            side = -1;
        }
    }
};

}  // namespace fx

#endif
