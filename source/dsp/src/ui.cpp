// ui.cpp: the module's screen. Shared by all sound modules (master in "Sound Modules/_shared/").
// One knob per parameter, four to a page, in parameter order.
//
//   SELECTOR  turn: move the selection       click: next page
//   CUSTOMIZE turn: change the selected      click/long press: back to its default
//
// A parameter marked RESONA_PARAM_CHOICE in params.h moves one step per detent and shows its
// choice name (DOT EIGHTH, HARMONIC ...) under the knob instead of a number.
#include "ui.h"

namespace {

bool is_choice(uint32_t i)
{
    return kParamExt[i].flags & RESONA_PARAM_CHOICE;
}

const char *choice_name(uint32_t i, float v)
{
    const resona_param_ext &e = kParamExt[i];
    int32_t k = (int32_t)(v - kParams[i].min + 0.5f);
    if (k < 0) k = 0;
    if (k >= (int32_t)e.choice_count) k = (int32_t)e.choice_count - 1;
    return e.choices[k];
}

}  // namespace

void screen_init(Screen &s, const char *title)
{
    s.form.init(title);
    for (uint32_t i = 0; i < P_COUNT; ++i) {
        s.knob[i].init(46 + 76 * (int32_t)(i % 4u), 84, kParams[i].name, kParams[i].min,
                       kParams[i].max, kParams[i].def, kParams[i].unit);
        if (is_choice(i) || (kParamExt[i].flags & RESONA_PARAM_STEPPED)) {
            s.knob[i].step(is_choice(i) ? 1.0f : kParamExt[i].step).decimals(0);
        } else {
            if (kParamExt[i].flags & RESONA_PARAM_LOG) s.knob[i].log_scale();
            // seconds, Hz of an LFO, dB: small ranges need a decimal (4.5 HZ, 3.2 S)
            if (kParams[i].max - kParams[i].min <= 25.0f) s.knob[i].decimals(1);
        }
        s.form.add(&s.knob[i], (int)(i / 4u));
    }
    s.form.settle();
}

void screen_set(Screen &s, uint32_t index, float value)
{
    if (index < P_COUNT) s.knob[index].set(value);
}

void screen_tick(Screen &s)
{
    s.form.tick();
}

void screen_draw(const Screen &s, const resona_ui *ui)
{
    s.form.draw(ui);
    const int page = s.form.page();
    for (uint32_t i = 0; i < P_COUNT; ++i) {
        if (!is_choice(i) || (int)(i / 4u) != page) continue;
        const int32_t x = s.knob[i].x, y = s.knob[i].y;
        const char *name = choice_name(i, s.knob[i].value());
        const uint16_t c = s.form.selected() == (int)i ? resona::ui::col::CYAN : resona::ui::col::INK;
        ui->rect(ui->ctx, x - 37, y + 32, 74, 18, resona::ui::col::BG);
        const uint32_t sc = resona::ui::detail::text_w(2, name) <= 72 ? 2u : 1u;
        resona::ui::detail::text_c(ui, x, y + (sc == 2u ? 34 : 38), sc, c, name);
    }
}

int screen_event(Screen &s, const resona_event &e)
{
    const int changed = s.form.handle(e);
    if (changed >= 0 && changed < (int)P_COUNT && is_choice((uint32_t)changed)) {
        // snap to a whole choice
        const float v = s.knob[changed].value();
        s.knob[changed].set((float)(int32_t)(v + 0.5f));
    }
    return changed;
}

float screen_value(const Screen &s, uint32_t index)
{
    return s.knob[index].value();
}
