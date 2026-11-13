// dsp.h: Storm Voice's state.
#ifndef DSP_H
#define DSP_H

#include <stdint.h>

#include "fx_core.hpp"
#include "params.h"

constexpr float kSize = 1.5f;              // a big plate: the open air
constexpr uint32_t kBlowFloats = 600u;     // 12 ms for the doppler wobble
constexpr uint32_t kDrops = 6u;
constexpr uint32_t kPoolFloats = kBlowFloats + fx::Plate::floats(kSize, 40.0f);

struct Drop { float ph, inc, env, dec, amp; };

struct Dsp {
    // how hard you play
    fx::Follower fast, slow, body, calm;
    float dyn, cooldown;
    uint32_t tick;
    fx::Rng rng;
    // rain
    Drop drop[kDrops];
    uint32_t next_drop;
    float drop_p, rain_g;
    fx::Biquad hiss;
    fx::HighPass1 drop_hp;
    // wind
    fx::Svf howl1, howl2;
    fx::Drift pitch_drift, gust, sway, flutter;
    fx::Lfo trem;
    fx::Line blow;
    float g1, g2, wind_g, trem_d, blow_d, blow_t;
    // thunder
    float crack, crack_amp, rumble_t, rumble_tau, rumble_amp, roll;
    float recrack[4];
    uint32_t recracks;
    fx::Biquad crack_bp, rumble_lp, growl_bp;
    fx::LowPass1 rumble_lp2;
    fx::Drift roll_drift;
    bool rumbling;
    // the open air
    fx::Plate space;
    fx::Limiter limiter;
    float thunder;
    fx::Smooth rain, wind, space_amt, mix, level;
};

bool dsp_init(Dsp &d, fx::Pool &pool);
void dsp_settle(Dsp &d);
void dsp_reset(Dsp &d);
void dsp_param(Dsp &d, uint32_t index, float value);
void dsp_process(Dsp &d, const float *in, float *out, uint32_t frames);

#endif
