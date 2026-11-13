// main.cpp: the glue between the Resona module interface and the module's DSP. Shared by all
// sound modules (master copy in "Sound Modules/_shared/"); the module itself is params.h,
// dsp.h and dsp.cpp.
//
// Long buffers (delay lines, reverb tanks) live in one PSRAM pool the host allocates before
// init(). If it is missing the module passes audio through untouched instead of failing.
#include <resona/math.h>
#include <resona/module.h>
#include <resona/parameters.h>
#include <resona/ui.h>

#include "dsp.h"
#include "ui.h"

namespace {

struct State {
    Dsp dsp;
    resona_parameter_inbox changes;
    Screen screen;
    float values[P_COUNT];
    uint32_t ready;
};

const resona_buffer_request kRequests[] = {
    {1, RESONA_MEMORY_PSRAM, kPoolFloats * (uint32_t)sizeof(float), 128},
};
const resona_memory_profile kMemory = {RESONA_MEMORY_VERSION, sizeof(resona_memory_profile), 1, kRequests};

void init(void *s, const resona_host *host)
{
    State *st = static_cast<State *>(s);
    st->changes = {};
    const resona_buffer *b = host ? resona_buffer_find(host->memory, 1) : nullptr;
    fx::Pool pool = {nullptr, 0};
    if (b && b->bytes >= kPoolFloats * sizeof(float)) {
        pool.p = static_cast<float *>(b->data);
        pool.left = kPoolFloats;
    }
    st->ready = dsp_init(st->dsp, pool) ? 1u : 0u;
    screen_init(st->screen, MODULE_TITLE);
    for (uint32_t i = 0; i < P_COUNT; ++i) {
        st->values[i] = kParams[i].def;
        dsp_param(st->dsp, i, kParams[i].def);
    }
    dsp_settle(st->dsp);
}

void reset(void *s)
{
    State *st = static_cast<State *>(s);
    if (st->ready) dsp_reset(st->dsp);
}

void set_param(void *s, uint32_t index, float value)
{
    State *st = static_cast<State *>(s);
    if (index < P_COUNT) {
        st->values[index] = value;
        resona_parameter_publish(&st->changes, index, value);
        screen_set(st->screen, index, value);
    }
}

void process(void *s, const float *const *inputs, float *const *outputs, uint32_t frames)
{
    const float *in = inputs[0];
    float *out = outputs[0];
    State *st = static_cast<State *>(s);
    float values[12];
    const uint32_t pending = resona_parameter_consume(&st->changes, values);
    for (uint32_t i = 0; i < P_COUNT; ++i) {
        if (pending & (1u << i)) dsp_param(st->dsp, i, values[i]);
    }
    if (st->ready) {
        dsp_process(st->dsp, in, out, frames);
    } else if (out != in) {
        for (uint32_t i = 0; i < frames; ++i) out[i] = in[i];
    }
    screen_tick(st->screen);
}

void ui_draw(void *s, const resona_ui *ui)
{
    screen_draw(static_cast<const State *>(s)->screen, ui);
}

void on_event(void *s, const resona_event *e)
{
    State *st = static_cast<State *>(s);
    const int changed = screen_event(st->screen, *e);
    if (changed >= 0 && changed < (int)P_COUNT) {
        const float v = screen_value(st->screen, (uint32_t)changed);
        st->values[changed] = v;
        resona_parameter_publish(&st->changes, (uint32_t)changed, v);
    }
}

float get_param(const void *s, uint32_t index)
{
    const State *st = static_cast<const State *>(s);
    return index < (uint32_t)P_COUNT ? st->values[index] : 0.0f;
}

const resona_module kModule = {
    RESONA_MODULE_ABI, sizeof(resona_module),
    MODULE_ID, MODULE_NAME, MODULE_VERSION,
    RESONA_LAYOUT_MONO, RESONA_RATE_48000,
    sizeof(State), P_COUNT, kParams, kParamExt,
    init, reset, set_param, process, ui_draw, on_event, get_param,
    nullptr, &kMemory, RESONA_MEMORY_FAST, nullptr,
};

}  // namespace

extern "C" RESONA_EXPORT const resona_module *resona_module_get(void)
{
    return &kModule;
}
