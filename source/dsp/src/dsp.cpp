// dsp.cpp: Storm Voice.
#include "dsp.h"

namespace {

constexpr float kHard = 0.115f;        // a pick this loud (about -19 dBFS) and sudden is thunder
constexpr float kCooldown = 3.0f * fx::kRate;

void reset_all(Dsp &d)
{
    d.fast.reset(); d.slow.reset(); d.body.reset(); d.calm.reset();
    d.dyn = 0.0f; d.cooldown = 0.0f; d.tick = 0;
    d.rng.reset();
    for (uint32_t k = 0; k < kDrops; ++k) d.drop[k] = {0.0f, 0.05f, 0.0f, 0.99f, 0.0f};
    d.next_drop = 0; d.drop_p = 0.0f; d.rain_g = 0.0f;
    d.hiss.reset(); d.drop_hp.reset();
    d.howl1.reset(); d.howl2.reset();
    d.pitch_drift.reset(); d.gust.reset(); d.sway.reset(); d.flutter.reset(); d.roll_drift.reset();
    d.trem.ph = 0.0f;
    d.blow.clear();
    d.g1 = d.g2 = 0.02f; d.wind_g = 0.0f; d.trem_d = 0.0f; d.blow_d = d.blow_t = 150.0f;
    d.crack = 0.0f; d.crack_amp = 0.0f; d.rumble_t = 0.0f; d.rumble_tau = 0.7f * fx::kRate; d.rumble_amp = 0.0f; d.roll = 0.0f;
    d.recracks = 0; d.rumbling = false;
    for (int k = 0; k < 4; ++k) d.recrack[k] = 0.0f;
    d.crack_bp.reset(); d.rumble_lp.reset(); d.growl_bp.reset(); d.rumble_lp2.reset();
    d.space.clear();
    d.limiter.reset();
}

}  // namespace

bool dsp_init(Dsp &d, fx::Pool &pool)
{
    bool ok = d.blow.attach(pool, kBlowFloats);
    ok = d.space.attach(pool, kSize, 40.0f) && ok;
    if (!ok) return false;
    d.fast.set(0.7f, 60.0f);
    d.slow.set(50.0f, 350.0f);
    d.body.set(60.0f, 1500.0f);     // the rain keeps falling for a moment after you stop
    d.calm.set(250.0f, 3200.0f);    // the wind dies down more slowly still
    d.rng.seed(0x53544F52u);
    d.hiss.set(fx::Biquad::BP, 5200.0f, 0.6f);
    d.drop_hp.set_hz(700.0f);
    d.pitch_drift.init(0x1234u, 0.35f);
    d.gust.init(0x8891u, 0.22f);
    d.sway.init(0x4477u, 0.6f);
    d.flutter.init(0x9911u, 2.5f);
    d.roll_drift.init(0x7711u, 7.0f);
    d.trem.set_hz(3.4f);
    d.crack_bp.set(fx::Biquad::BP, 1500.0f, 0.5f);
    d.rumble_lp.set(fx::Biquad::LP, 95.0f, 0.75f);
    d.rumble_lp2.set_hz(140.0f);
    d.growl_bp.set(fx::Biquad::BP, 260.0f, 0.8f);
    d.space.set_predelay_ms(28.0f);
    d.space.set_damp_hz(3200.0f);       // dark
    d.space.set_bandwidth_hz(5500.0f);
    d.space.set_lowcut_hz(140.0f);
    d.space.set_mod(0.35f, 0.5f);
    d.limiter.init();
    d.thunder = 0.6f;
    d.rain.init(0.5f, 60.0f);
    d.wind.init(0.4f, 60.0f);
    d.space_amt.init(0.4f, 60.0f);
    d.mix.init(1.0f, 30.0f);
    d.level.init(1.0f, 30.0f);
    reset_all(d);
    return true;
}

void dsp_settle(Dsp &d) { d.rain.snap(); d.wind.snap(); d.space_amt.snap(); d.mix.snap(); d.level.snap(); }
void dsp_reset(Dsp &d) { reset_all(d); dsp_settle(d); }

void dsp_param(Dsp &d, uint32_t index, float v)
{
    switch (index) {
    case P_RAIN: d.rain.set(fx::pct(v)); break;
    case P_WIND: d.wind.set(fx::pct(v)); break;
    case P_THUNDER: d.thunder = fx::pct(v); break;
    case P_SPACE: d.space_amt.set(fx::pct(v)); break;
    case P_SIZE: d.space.set_decay(1.4f + 6.6f * fx::pct(v) * fx::pct(v)); break;
    case P_MIX: d.mix.set(fx::pct(v)); break;
    case P_LEVEL: d.level.set(resona_db_to_lin(v)); break;
    default: break;
    }
}

void dsp_process(Dsp &d, const float *in, float *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i) {
        const float x = in[i];
        const float ef = d.fast.process(x);
        const float es = d.slow.process(x);
        const float body = d.body.process(x);     // linear level: the weather is as loud as you are
        const float calm = d.calm.process(x);
        const float rain = d.rain.next(), wind = d.wind.next(), mix = d.mix.next();

        // a hard, sudden pick: thunder (at most one every 3 s)
        if (d.cooldown > 0.0f) d.cooldown -= 1.0f;
        if (ef > kHard && ef > 1.7f * es + 0.01f && d.cooldown <= 0.0f && d.thunder > 0.001f) {
            d.cooldown = kCooldown;
            const float hit = fx::clampf(ef * 2.2f, 0.2f, 0.6f) * d.thunder;
            d.crack = 1.0f; d.crack_amp = hit;
            d.recracks = 2u + (d.rng.next() & 1u);
            float t = 0.0f;
            for (uint32_t k = 0; k < d.recracks; ++k) { t += (0.03f + 0.09f * d.rng.uni()) * fx::kRate; d.recrack[k] = t; }
            d.rumbling = true; d.rumble_t = 0.0f;
            d.rumble_tau = (0.6f + 0.4f * d.rng.uni()) * fx::kRate;
            d.rumble_amp = hit;
        }

        // control rate: the storm's size, filter tunings, the gusts
        if ((d.tick++ & 7u) == 0u) {
            d.dyn = fx::clampf(resona_sqrtf(body * 7.0f), 0.0f, 1.0f);
            d.drop_p = (6.0f + 520.0f * d.dyn * d.dyn * (0.25f + 0.75f * rain)) * fx::kInvRate;
            d.rain_g = body * (0.5f + 1.5f * d.dyn) * rain;
            const float gust = 0.6f + 0.4f * d.gust.next();
            // the howl climbs as you dig in, and wanders by itself
            const float oct = 0.75f * d.pitch_drift.next() + 1.5f * d.dyn + 0.5f * wind + 0.25f * (gust - 0.6f);
            const float hz = 240.0f * resona_exp2f(oct);
            const float pi_fs = fx::kPi * fx::kInvRate;
            d.g1 = fx::tan_small(fx::clampf(hz, 120.0f, 3000.0f) * pi_fs);
            d.g2 = fx::tan_small(fx::clampf(hz * 1.52f, 150.0f, 4500.0f) * pi_fs);
            d.wind_g = (calm * 0.7f + body * 0.5f) * gust * resona_powf(wind, 0.6f);
            // the guitar is blown about: tremolo depth follows the gusts, a slow doppler sway
            d.trem.set_hz(3.2f + 1.6f * d.flutter.next());
            d.trem_d = 0.32f * wind * mix * gust;
            d.blow_t = 190.0f + 110.0f * wind * mix * d.sway.next();
            d.roll = 0.62f + 0.38f * d.roll_drift.next();
        }

        // rain: droplets (tiny decaying pings at random pitches) over a soft hiss
        const float n = d.rng.bi();
        if (d.rng.uni() < d.drop_p) {
            Drop &p = d.drop[d.next_drop];
            d.next_drop = d.next_drop + 1u == kDrops ? 0u : d.next_drop + 1u;
            const float r = d.rng.uni();
            const bool low = d.rng.uni() < 0.14f;        // the odd fat drop on a roof
            p.inc = (low ? 350.0f + 500.0f * r : 1900.0f * resona_exp2f(1.9f * r)) * fx::kInvRate;
            p.dec = low ? 0.9982f : 0.9925f + 0.005f * d.rng.uni();
            const float a = d.rng.uni();
            p.amp = (0.25f + 0.75f * a * a) * (low ? 0.7f : 1.0f);
            p.env = 1.0f; p.ph = 0.0f;
        }
        float drops = 0.0f;
        for (uint32_t k = 0; k < kDrops; ++k) {
            Drop &p = d.drop[k];
            if (p.env < 1.0e-4f) continue;
            drops += fx::sin01(p.ph) * p.env * p.amp;
            p.ph = fx::wrap01(p.ph + p.inc);
            p.inc *= 0.99985f;                           // each ping falls a little: "plip"
            p.env *= p.dec;
        }
        const float rain_out = (d.hiss.process(n) * 0.32f + d.drop_hp.process(drops) * 0.62f) * d.rain_g;

        // wind: two resonant bands of noise a fifth apart
        float lp, b1, b2;
        const float n2 = d.rng.bi();
        d.howl1.process(n2, d.g1, 0.085f, lp, b1);
        d.howl2.process(n2, d.g2, 0.13f, lp, b2);
        const float wind_out = (b1 * 0.11f + b2 * 0.07f) * d.wind_g * 8.3f;

        // thunder: a crack that splits two or three times, then a rolling rumble under 120 Hz
        float thunder = 0.0f;
        if (d.crack > 1.0e-4f || d.rumbling) {
            for (uint32_t k = 0; k < d.recracks; ++k) {
                if (d.recrack[k] > 0.0f && (d.recrack[k] -= 1.0f) <= 0.0f) d.crack = fx::maxf(d.crack, 0.55f + 0.3f * d.rng.uni());
            }
            const float n3 = d.rng.bi();
            const float cr = d.crack_bp.process(n3 * d.crack) * d.crack_amp * 1.1f;
            d.crack *= 0.99915f;                         // about 25 ms
            float ru = 0.0f;
            if (d.rumbling) {
                d.rumble_t += 1.0f;
                const float t = d.rumble_t;
                const float e = (1.0f - resona_expf(-t / (0.06f * fx::kRate))) * resona_expf(-t / d.rumble_tau);
                if (t > 7.0f * d.rumble_tau) d.rumbling = false;
                const float low = d.rumble_lp2.process(d.rumble_lp.process(n3));
                ru = (low * 16.0f + d.growl_bp.process(n3) * 0.55f) * e * d.roll * d.rumble_amp;
            }
            thunder = cr + ru;
        }

        // the guitar, blown about
        d.blow.push(x);
        d.blow_d += 0.0004f * (d.blow_t - d.blow_d);
        const float blown = d.blow.cub(d.blow_d) * (1.0f - d.trem_d * (0.5f + 0.5f * d.trem.sine()));
        const float g = x + mix * (blown - x);           // MIX 0 is the guitar alone, untouched

        // the open air around it all
        const float weather = rain_out + wind_out + thunder;
        const float wet = d.space.process(blown + 0.5f * thunder + 0.3f * wind_out) * d.space_amt.next() * 0.85f;

        out[i] = d.limiter.process((g + mix * (weather + wet)) * d.level.next());
    }
}
