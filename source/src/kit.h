// kit.h: what every Creative App shares. MIT.
//
//   kit::Audio    the App's own effect graph (Guitar -> its dsp pack -> Output): started and
//                 retried until the pack is installed, parameters cached and re-sent, and the
//                 CPU-limit back-off (trip_guard.h) when the OS bypasses the effect
//   kit::Levels   the input/output meters, smoothed for animation (0..1)
//   kit::Saver    debounced saving of a settings struct to the App's own store
//   kit::Canvas   drawing: shapes, centred text, gradients, straight into the frame
//
// The master copy is "Apps/_kit/"; each App keeps a copy in src/ so it builds alone.
#pragma once
#include <resona/platform.h>
#include <resona/ui.h>
#include <resona/math.h>
#include "own_audio.h"
#include "trip_guard.h"

namespace kit {

/* ---- audio ----------------------------------------------------------------------------- */

struct Audio {
    own_audio::Graph g;
    trip_guard::Guard guard;
    int ready, error;        // error: 0, or a RESONA_ERR_* (RESONA_ERR_PACK_ID = not installed)
    uint32_t next;           // when to look again (ms)
    float val[12];           // the last value of each parameter: re-sent whenever audio (re)starts
    uint32_t count;
};

inline void audio_trip(const resona_platform *P, Audio &a, uint32_t now)
{
    a.ready = 0;
    a.error = RESONA_ERR_BUDGET;
    a.next = now + trip_guard::tripped(a.guard, now);
    own_audio::stop(P, a.g);
}

// Call every frame. Once a second: build and enable the graph if it is not running, or check
// the running effect for a fault.
inline void audio_tick(const resona_platform *P, Audio &a, const char *pack_id, uint32_t params, uint32_t now)
{
    if ((int32_t)(now - a.next) < 0) return;
    a.next = now + 1000;
    a.count = params;
    if (a.ready) {
        if (trip_guard::faulted(P, a.g.h[0])) audio_trip(P, a, now);
        return;
    }
    int rc = own_audio::start(P, a.g, pack_id);
    if (rc < 0) { a.error = rc; return; }
    resona_effect_info fi = {};
    rc = P->effect_info(a.g.h[0], &fi);
    if (rc == RESONA_OK && fi.param_count != params) rc = RESONA_ERR_STATE;
    if (rc == RESONA_OK) rc = P->effect_enable(a.g.h[0], 1);
    if (rc != RESONA_OK) {
        a.error = rc < 0 ? rc : RESONA_ERR_STATE;
        own_audio::stop(P, a.g);
        return;
    }
    a.ready = 1;
    a.error = 0;
    for (uint32_t i = 0; i < params && i < 12; ++i) P->effect_param_set(a.g.h[0], i, a.val[i]);
}

inline void audio_set(const resona_platform *P, Audio &a, uint32_t n, float v)
{
    if (n >= 12) return;
    a.val[n] = v;
    if (a.ready) P->effect_param_set(a.g.h[0], n, v);
}

// Pass every event through. Returns true when it was an audio/pack event.
inline bool audio_event(const resona_platform *P, Audio &a, const resona_app_event *ev, uint32_t now)
{
    if (ev->kind == RESONA_APP_EV_AUDIO) {
        if (a.ready && (ev->u.audio.tripped_role >= 0 || trip_guard::faulted(P, a.g.h[0]))) audio_trip(P, a, now);
        return true;
    }
    if (ev->kind == RESONA_APP_EV_CHANGED && ev->u.changed.what == RESONA_CHANGED_MODELS) {
        if (!a.ready && a.error != RESONA_ERR_BUDGET) a.next = now;   // a pack was installed: look now
        return true;
    }
    return false;
}

// What to tell the player, or nullptr when audio runs. `install` e.g. "INSTALL MEOW VOICE IN LAB".
inline const char *audio_message(const Audio &a, const char *install)
{
    if (a.ready) return nullptr;
    if (a.error == RESONA_ERR_BUDGET) return "CPU LIMIT";
    if (a.error == RESONA_ERR_PACK_ID || a.error == 0) return install;
    if (a.error == RESONA_ERR_UNSUPPORTED) return "NO AUDIO HERE (PREVIEW OR OLD BOX OS)";
    return "AUDIO ERROR";
}

/* ---- meters ---------------------------------------------------------------------------- */

struct Levels {
    float in, out;       // 0..1, fast attack, slow release
    float hit;           // jumps to 1 on a hard pick, then decays: for one-shot animations
    uint8_t raw_in, raw_out, cpu, last_in;
};

inline void levels_tick(const resona_platform *P, Levels &l, uint32_t ms)
{
    P->meters(&l.raw_in, &l.raw_out, &l.cpu);
    const float i = l.raw_in / 9.0f, o = l.raw_out / 9.0f;
    const float rel = resona_expf(-(float)ms / 180.0f);
    l.in = i > l.in ? i : l.in * rel + i * (1.0f - rel);
    l.out = o > l.out ? o : l.out * rel + o * (1.0f - rel);
    if (l.raw_in >= 5 && l.raw_in >= l.last_in + 2) l.hit = 1.0f;
    l.hit *= resona_expf(-(float)ms / 250.0f);
    l.last_in = l.raw_in;
}

/* ---- settings -------------------------------------------------------------------------- */

struct Saver {
    uint32_t changed;
    int pending, failed;
};

inline void save_later(Saver &s, uint32_t now) { s.changed = now; s.pending = 1; }

// Call every frame: writes `data` 1.2 s after the last change. A failed write is remembered in
// `failed` (show it if you like); it never touches the audio state.
template <typename T> inline void save_tick(const resona_platform *P, Saver &s, const T &data, uint32_t now)
{
    if (!s.pending || now - s.changed < 1200u) return;
    s.pending = 0;
    s.failed = P->app_store_write(0, &data, sizeof data) != RESONA_OK;
}

// Loads `data` if the store holds a record with this magic as its first uint32_t field.
template <typename T> inline bool load(const resona_platform *P, T &data, uint32_t magic)
{
    T tmp;
    if (P->app_store_read(0, &tmp, sizeof tmp) != RESONA_OK) return false;
    if (*reinterpret_cast<const uint32_t *>(&tmp) != magic) return false;
    data = tmp;
    return true;
}

/* ---- drawing --------------------------------------------------------------------------- */

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline uint16_t rgb(int r, int g, int b)
{
    r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline uint16_t mix(uint16_t a, uint16_t b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    const int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    const int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) | (((int)(ag + (bg - ag) * t)) << 5) | (int)(ab + (bb - ab) * t));
}

// A baked, palette-indexed sprite (index 0 is transparent). ox/oy: where its top-left sits
// relative to the position it is drawn at, so cropped art keeps its place in the scene.
struct Sprite {
    uint16_t w, h;
    int16_t ox, oy;
    const uint8_t *px;
};

struct Canvas {
    resona_ui *ui;
    uint16_t *px;
    uint32_t stride;
    static constexpr int W = 320, H = 172;

    bool begin(const resona_platform *P) { ui = P->screen_begin(&px, &stride); return ui && px; }
    void present(const resona_platform *P) { P->screen_present(0, 0, 0, 0); }

    void put(int x, int y, uint16_t c) { if (x >= 0 && x < W && y >= 0 && y < H) px[(uint32_t)y * stride + (uint32_t)x] = c; }
    void rect(int x, int y, int w, int h, uint16_t c)
    {
        int x1 = clampi(x + w, 0, W), y1 = clampi(y + h, 0, H);
        x = clampi(x, 0, W); y = clampi(y, 0, H);
        for (int j = y; j < y1; ++j) for (int i = x; i < x1; ++i) px[(uint32_t)j * stride + (uint32_t)i] = c;
    }
    void clear(uint16_t c) { rect(0, 0, W, H, c); }
    // top-to-bottom gradient over a rectangle: skies, water, walls
    void vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
    {
        for (int j = 0; j < h; ++j) rect(x, y + j, w, 1, mix(top, bottom, h > 1 ? (float)j / (float)(h - 1) : 0.0f));
    }
    void ellipse(int cx, int cy, int rx, int ry, uint16_t c)   // filled
    {
        if (rx <= 0 || ry <= 0) return;
        for (int j = -ry; j <= ry; ++j) {
            const float t = (float)j / (float)ry;
            const int half = (int)((float)rx * resona_sqrtf(1.0f - t * t) + 0.5f);
            rect(cx - half, cy + j, 2 * half + 1, 1, c);
        }
    }
    void circle(int cx, int cy, int r, uint16_t c) { ellipse(cx, cy, r, r, c); }
    void ring(int cx, int cy, int rx, int ry, uint16_t c) { resona_ui_ellipse(ui, cx, cy, rx, ry, c); }   // outline
    void line(int x0, int y0, int x1, int y1, uint16_t c) { resona_ui_line(ui, x0, y0, x1, y1, c); }
    void thick(int x0, int y0, int x1, int y1, int w, uint16_t c)
    {
        for (int k = -(w / 2); k <= w / 2; ++k) { line(x0 + k, y0, x1 + k, y1, c); line(x0, y0 + k, x1, y1 + k, c); }
    }
    void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)   // filled
    {
        const int16_t p[6] = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2};
        resona_ui_poly_fill(ui, p, 3, c);
    }
    void quad(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint16_t c)   // filled, convex
    {
        const int16_t p[8] = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, (int16_t)x3, (int16_t)y3};
        resona_ui_poly_fill(ui, p, 4, c);
    }
    // baked pixel art: a sprite with transparency (optionally mirrored), or an opaque full screen
    void blit(const Sprite &sp, const uint16_t *pal, int x, int y, bool flip = false)
    {
        x += sp.ox; y += sp.oy;
        for (int j = 0; j < sp.h; ++j) {
            const int Y = y + j;
            if (Y < 0 || Y >= H) continue;
            const uint8_t *row = sp.px + (uint32_t)j * sp.w;
            uint16_t *dst = px + (uint32_t)Y * stride;
            for (int i = 0; i < sp.w; ++i) {
                const uint8_t v = row[flip ? sp.w - 1 - i : i];
                const int X = x + i;
                if (v && X >= 0 && X < W) dst[X] = pal[v];
            }
        }
    }
    void backdrop(const Sprite &sp, const uint16_t *pal)   // w x h must be the whole screen
    {
        for (int j = 0; j < H; ++j) {
            const uint8_t *row = sp.px + (uint32_t)j * sp.w;
            uint16_t *dst = px + (uint32_t)j * stride;
            for (int i = 0; i < W; ++i) dst[i] = pal[row[i]];
        }
    }
    // text with a 1 px drop shadow: readable over artwork
    void text_s(int x, int y, int scale, uint16_t c, uint16_t shadow, const char *s)
    {
        text(x + scale, y + scale, scale, shadow, s);
        text(x, y, scale, c, s);
    }
    // the built-in 5x7 pixel face, scale 1..8 (6 x 8 px per character at scale 1)
    void text(int x, int y, int scale, uint16_t c, const char *s) { ui->text(ui->ctx, x, y, (uint32_t)scale, c, s); }
    int text_w(int scale, const char *s) { return resona_ui_text_width((uint32_t)scale, s); }
    void text_c(int cx, int y, int scale, uint16_t c, const char *s) { text(cx - text_w(scale, s) / 2, y, scale, c, s); }
    // a strip along the bottom for the audio message (INSTALL ..., CPU LIMIT)
    void banner(const char *msg, uint16_t bg, uint16_t fg)
    {
        if (!msg) return;
        rect(0, H - 14, W, 14, bg);
        text_c(W / 2, H - 11, 1, fg, msg);
    }
};

}  // namespace kit
