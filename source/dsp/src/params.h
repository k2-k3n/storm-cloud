// params.h: Storm Voice. Your playing is the weather. Soft notes bring a drizzle, digging in
// raises the wind and the rain, and a hard pick breaks the storm with a thunder clap.
//
//   input -> blown about (tremolo + doppler, WIND) --+--> dark outdoor reverb (SPACE, SIZE)
//   dynamics -> RAIN (droplets + hiss) + WIND (howl) + THUNDER (crack + rumble on a hard pick)
//   guitar + MIX * (weather + reverb) -> limiter
#ifndef PARAMS_H
#define PARAMS_H

#include <stdint.h>

#include <resona/module.h>

#define MODULE_ID "org.resona.creative.storm-cloud.voice"
#define MODULE_NAME "Storm Voice"
#define MODULE_TITLE "STORM VOICE"
#define MODULE_VERSION "1.0.0"

enum Param : uint32_t { P_RAIN, P_WIND, P_THUNDER, P_SPACE, P_SIZE, P_MIX, P_LEVEL, P_COUNT };

const resona_param kParams[P_COUNT] = {
    {"RAIN", 0.0f, 100.0f, 50.0f, "%"},
    {"WIND", 0.0f, 100.0f, 40.0f, "%"},
    {"THUNDER", 0.0f, 100.0f, 60.0f, "%"},
    {"SPACE", 0.0f, 100.0f, 40.0f, "%"},
    {"SIZE", 0.0f, 100.0f, 55.0f, "%"},
    {"MIX", 0.0f, 100.0f, 100.0f, "%"},
    {"LEVEL", -12.0f, 6.0f, 0.0f, "dB"},
};

#define PX_NONE {0, 0.0f, 0, nullptr}
const resona_param_ext kParamExt[P_COUNT] = {PX_NONE, PX_NONE, PX_NONE, PX_NONE, PX_NONE, PX_NONE, PX_NONE};

#endif
