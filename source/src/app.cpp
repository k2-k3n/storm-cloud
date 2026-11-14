// Storm Cloud: your playing is the weather. MIT.
//
//   SELECTOR   turn: RAIN (the rain gauge by the house)    click: THUNDER on/off (the cloud's bolt)
//   CUSTOMIZE  turn: WIND (the windsock)                    click: SPACE small / wide / huge (the
//                                                           mountains)       long press: reset
//
// The sound is the nested dsp/ pack (Storm Voice); this file is the sky. The App cannot read the
// pack's state, so the weather follows the input meter: play softly and it drizzles, dig in and
// the cloud turns black, a hard pick is lightning, and nine quiet seconds bring the sun back out
// with a rainbow. The valley and cloud are baked by tools/encode_weather.py.
//
// The Web, Mobile and MIDI controllers (controller/, CCs on channel 1, see README.md) reach every
// setting the knobs reach, plus the thunder's loudness, MIX and LEVEL, and four hold pads:
// DOWNPOUR (all the rain), GALE (all the wind), STORM (everything, huge) and SUN (the dry guitar
// under a rainbow).
#include <resona/platform.h>
#include <resona/startup_helpers.h>
#include "splash_data.h"
#include "kit.h"
#include "art.h"
#include "weather.h"
#include "controller_link.h"

namespace {

const resona_platform *P;
const char kPack[] = "org.resona.creative.storm-cloud.voice";   // dsp/resona.project.json
enum { RAIN, WIND, THUNDER, SPACE, SIZE, MIX, LEVEL, PARAMS };
const float kSpaceAmt[3] = {26.0f, 40.0f, 54.0f}, kSpaceSize[3] = {20.0f, 55.0f, 100.0f};
const char *const kSpaceName[3] = {"SMALL", "WIDE", "HUGE"};

struct SavedV1 { uint32_t magic; int rain, wind, thunder, space; };
struct Saved { uint32_t magic; int rain, wind, thunder, space, thunder_amt, mix, level; };
const uint32_t kMagicV1 = 0x53544D31u;   // "STM1"
const uint32_t kMagic = 0x53544D32u;     // "STM2"
const uint32_t kNever = 0x80000000u;

// The controllers' MIDI map: CC on channel 1 (controller/shared/profile.js says the same).
enum { CC_RAIN = 20, CC_WIND, CC_THUNDER, CC_THUNDER_AMT, CC_SPACE, CC_MIX, CC_LEVEL,
       CC_DOWNPOUR = 30, CC_GALE, CC_STORM, CC_SUN };

struct State {
    kit::Audio audio;
    kit::Levels lv;
    kit::Saver saver;
    Saved s;
    uint32_t ms, idle, blink_at, next_blink, next_draw, seed;
    uint32_t rain_at, wind_at, space_at, strike_at, touched_at;   // when each was last touched (ms + kNever = never)
    int touched, strike_x, strike_flip, struck;
    float storm, dark, wet;      // how hard you play (smoothed), how black the sky is, how soaked the ground
    uint16_t pal_s[kArtColours], pal_c[kArtColours];   // this frame's colours: the scene's, the cloud's
    int downpour, gale, gust, sun;   // pads held on a controller (gust: STORM)
    ctl::Link link;
};

const int kBinds = 11;
ctl::Bind kBind[kBinds];
void bind(State *st)
{
    Saved &s = st->s;
    const ctl::Bind b[kBinds] = {
        {CC_RAIN, ctl::RANGE, 0, 100, &s.rain}, {CC_WIND, ctl::RANGE, 0, 100, &s.wind},
        {CC_THUNDER, ctl::STEPS, 0, 1, &s.thunder}, {CC_THUNDER_AMT, ctl::RANGE, 0, 100, &s.thunder_amt},
        {CC_SPACE, ctl::STEPS, 0, 2, &s.space}, {CC_MIX, ctl::RANGE, 0, 100, &s.mix},
        {CC_LEVEL, ctl::RANGE, -12, 6, &s.level},
        {CC_DOWNPOUR, ctl::TRIGGER, 0, 1, nullptr}, {CC_GALE, ctl::TRIGGER, 0, 1, nullptr},
        {CC_STORM, ctl::TRIGGER, 0, 1, nullptr}, {CC_SUN, ctl::TRIGGER, 0, 1, nullptr},
    };
    for (int i = 0; i < kBinds; ++i) kBind[i] = b[i];
}

uint32_t rnd(State *st) { st->seed = st->seed * 1664525u + 1013904223u; return st->seed >> 8; }
uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

// The pads override the settings while held: STORM is everything at once; SUN wins over all of them.
int rain_now(const State *st) { return st->sun ? 0 : (st->downpour || st->gust ? 100 : st->s.rain); }
int wind_now(const State *st) { return st->sun ? 0 : (st->gale || st->gust ? 100 : st->s.wind); }
int space_now(const State *st) { return st->gust ? 2 : st->s.space; }
bool held(const State *st) { return st->downpour || st->gale || st->gust; }

void push(State *st)
{
    kit::audio_set(P, st->audio, RAIN, (float)rain_now(st));
    kit::audio_set(P, st->audio, WIND, (float)wind_now(st));
    kit::audio_set(P, st->audio, THUNDER, st->gust ? 100.0f : (st->s.thunder ? (float)st->s.thunder_amt : 0.0f));
    kit::audio_set(P, st->audio, SPACE, kSpaceAmt[space_now(st)]);
    kit::audio_set(P, st->audio, SIZE, kSpaceSize[space_now(st)]);
    kit::audio_set(P, st->audio, MIX, st->sun ? 0.0f : (float)st->s.mix);
    kit::audio_set(P, st->audio, LEVEL, (float)st->s.level);
}

void defaults(State *st) { st->s = {kMagic, 50, 40, 1, 1, 60, 100, 0}; }

// 1 just after a touch, fading to 0 over `ms`: a turned knob shows what it does for a moment
float since(const State *st, uint32_t at, float ms)
{
    if (at == kNever) return 0.0f;
    const float t = (float)(st->ms - at) / ms;
    return t >= 1.0f ? 0.0f : (t < 0.6f ? 1.0f : (1.0f - t) / 0.4f);
}

void strike(State *st)
{
    st->strike_at = st->ms;
    st->struck = 1;
    st->strike_x = 104 + (int)(rnd(st) % 96u);
    st->strike_flip = (int)(rnd(st) & 1u);
}

/* ---- drawing ---------------------------------------------------------------------------- */

// a sprite that fades in through an ordered dither (level 0..16): the rainbow
void blit_dither(kit::Canvas &c, const kit::Sprite &sp, const uint16_t *pal, int x, int y, int level)
{
    static const uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    x += sp.ox; y += sp.oy;
    for (int j = 0; j < sp.h; ++j) {
        const int Y = y + j;
        if (Y < 0 || Y >= kit::Canvas::H) continue;
        const uint8_t *row = sp.px + (uint32_t)j * sp.w;
        for (int i = 0; i < sp.w; ++i) {
            const int X = x + i;
            if (row[i] && X >= 0 && X < kit::Canvas::W && kBayer[(Y & 3) * 4 + (X & 3)] < level)
                c.px[(uint32_t)Y * c.stride + (uint32_t)X] = pal[row[i]];
        }
    }
}

void draw(State *st)
{
    kit::Canvas c;
    if (!c.begin(P)) return;
    const uint16_t ink = kit::rgb(22, 14, 34), cream = kit::rgb(255, 250, 236);
    const uint32_t ms = st->ms;

    // the weather right now
    const int rain = rain_now(st), wind = wind_now(st), space = space_now(st);
    const float rain_set = (float)rain * 0.01f, wind_set = (float)wind * 0.01f;
    const float tease_r = since(st, st->rain_at, 2600.0f), tease_w = since(st, st->wind_at, 2600.0f);
    const float drive_r = st->storm > tease_r ? st->storm : tease_r, drive_w = st->storm > tease_w ? st->storm : tease_w;
    const float rainv = rain ? drive_r * (0.2f + 0.8f * rain_set) : 0.0f;
    const float windv = drive_w * wind_set;
    const float dark = st->dark;
    const uint32_t ts = st->struck ? ms - st->strike_at : 100000u;      // since the lightning
    const bool sunny = (st->idle > 9000u && dark < 0.08f) || (st->sun && dark < 0.3f);
    const uint32_t bright = st->sun ? 10440u : st->idle;      // SUN: straight to the rainbow and the shades

    // this frame's palettes: calm .. storm, and white for an instant when lightning strikes
    float flash = 0.0f;
    if (ts < 80u) flash = 0.64f * (1.0f - ts / 115.0f);
    else if (ts >= 130u && ts < 310u) flash = 0.35f * (1.0f - (ts - 130u) / 180.0f);
    const float dc = kit::clampf(dark * 1.3f, 0.0f, 1.0f);
    const uint16_t white = kit::rgb(236, 240, 255);
    for (int i = 1; i < kArtColours; ++i) {
        st->pal_s[i] = kit::mix(kit::mix(kArtPalette[i], kArtStorm[i], dark), white, flash * .18f);
        st->pal_c[i] = kit::mix(kit::mix(kArtPalette[i], kArtStorm[i], dc), white, flash * 0.6f);
    }
    const uint16_t *ps = st->pal_s, *pc = st->pal_c;

    const float intro = kit::clampf((float)ms / 350.0f, 0.0f, 1.0f);
    const float reveal = intro * intro * (3.0f - 2.0f * intro);
    uint16_t valley[sizeof kValleyPalette / sizeof kValleyPalette[0]];
    const uint16_t deep = kit::rgb(25, 42, 72);
    const uint16_t mist = kit::rgb(161, 193, 219);
    const float haze = space == 2 ? 0.31f : (space == 1 ? 0.13f : 0.0f);
    for (unsigned i = 0; i < sizeof kValleyPalette / sizeof kValleyPalette[0]; ++i) {
        const uint16_t original = kValleyPalette[i];
        // Lift the valley without flattening the terrain or the cottage's warm light.
        const uint16_t color = kit::rgb(((original >> 11) * 8) + 33,
                                      (((original >> 5) & 63) * 4) + 40,
                                      ((original & 31) * 8) + 43);
        const int blue_bias = (original & 31) - (original >> 11);
        const float distance = kit::clampf((blue_bias - 5) * 0.10f, 0.0f, 1.0f);
        const uint16_t shaded = kit::mix(color, mist, haze * distance);
        const uint16_t dusk = kit::mix(shaded, deep, dark * 0.04f);
        valley[i] = kit::mix(dusk, white, flash * 0.13f);
    }
    c.backdrop(kValley, valley);
    if (bright > 9000u) blit_dither(c, kArt_rainbow, ps, 0, 0, kit::clampi((int)((bright - 9000u) / 90u), 0, 16));
    if (dark < 0.3f) c.blit((ms / 600u) & 1u ? kArt_sun_1 : kArt_sun_0, ps, kSunX - (int)(dark * 60.0f), kSunY + (int)(dark * 50.0f));   // it ducks behind the cloud

    // the cloud
    const bool shocked = ts < 650u, fuming = !shocked && ts < 1600u;
    const bool blowing = !shocked && !fuming && windv > 0.42f;
    const bool angry = fuming || (!shocked && !blowing && dark > 0.62f);
    const bool worried = !shocked && !blowing && !angry && dark > 0.2f;
    const int bob = (int)((2.0f + 3.4f * st->storm) * resona_sinf((float)(ms % 5600u) * (6.2832f / 5600.0f)));
    const float drift = windv * 6.0f * resona_sinf((float)(ms % 3800u) * (6.2832f / 3800.0f));
    const float recoil = ts < 260u ? 5.0f * (1.0f - ts / 260.0f) : 0.0f;
    const int cx = kCloudX + (int)drift, cy = kCloudY + bob - (int)recoil;
    uint16_t cloud[sizeof kCloudPalette / sizeof kCloudPalette[0]];
    for (unsigned i = 0; i < sizeof kCloudPalette / sizeof kCloudPalette[0]; ++i) {
        const uint16_t base = kCloudPalette[i];
        const uint16_t raised = kit::rgb((base >> 11) * 8 + 20,
                                      ((base >> 5) & 63) * 4 + 25,
                                      (base & 31) * 8 + 32);
        const uint16_t tone = kit::mix(raised, deep, dc * 0.34f);
        cloud[i] = kit::mix(tone, white, flash * 0.52f);
    }
    for (int y = 0; y < kFeltCloud.h; ++y) {
        const int screen_y = cy + kFeltCloud.oy + y;
        if (screen_y < 0 || screen_y >= c.H) continue;
        const uint8_t *row = kFeltCloud.px + y * kFeltCloud.w;
        uint16_t *dst = c.px + screen_y * c.stride + cx + kFeltCloud.ox;
        for (int x = 0; x < kFeltCloud.w; ++x) {
            if (!row[x]) continue;
            const bool edge = x == 0 || x == kFeltCloud.w - 1 || !row[x - 1] || !row[x + 1];
            dst[x] = edge ? kit::mix(dst[x], cloud[row[x]], .72f) : cloud[row[x]];
        }
    }
    st->pal_c[kArtFeature] = cream;
    const int fx = cx, fy = cy + 4;
    const uint32_t since_blink = ms - st->blink_at;
    const kit::Sprite *eye = &kArt_eye_open, *mouth = &kArt_mouth_smile;
    if (shocked) { eye = &kArt_eye_shock; mouth = &kArt_mouth_shock; }
    else if (blowing) { eye = &kArt_eye_blow; mouth = &kArt_mouth_blow; }
    else if (angry) { eye = &kArt_eye_angry; mouth = &kArt_mouth_frown; }
    else if (worried) { eye = &kArt_eye_worried; mouth = &kArt_mouth_wavy; }
    else if (sunny) { eye = &kArt_eye_happy; mouth = &kArt_mouth_grin; }
    else if (since_blink < 70u || (since_blink >= 140u && since_blink < 200u)) eye = &kArt_eye_half;
    else if (since_blink < 140u) eye = &kArt_eye_shut;
    c.blit(*eye, pc, fx - 19, fy);
    c.blit(*eye, pc, fx + 19, fy, true);
    if (!shocked && !blowing && !angry) { c.blit(kArt_blush, pc, fx - 33, fy + 6); c.blit(kArt_blush, pc, fx + 33, fy + 6); }
    c.blit(*mouth, pc, fx + (blowing ? 3 : 0), fy + 10);
    if (blowing) { const int puff = (int)((ms / 45u) % 22u); c.blit(kArt_gust_1, pc, fx + 12 + puff, fy + 12 + puff / 4); }
    if (sunny) {   // the sun is out: on go the shades
        const int drop = kit::clampi(8 - (int)((bright - 9000u) / 40u), 0, 8);
        c.blit(kArt_shades, pc, fx, fy - 4 - drop * 2);
    }
    // THUNDER is on: the cloud keeps a bolt handy (it has just thrown it while lightning is out)
    if ((st->s.thunder || st->gust) && ts > 900u) c.blit(kArt_bolt_small, pc, cx + 52 + (int)((ms / 180u) & 1u), cy + 22 + (int)((ms / 420u) & 1u));

    // Draw a soft fringe and a sharp branching core over the valley.
    if (flash > .04f) {
        const int x = kit::clampi(st->strike_x, 24, 290);
        const int side = st->strike_flip ? -1 : 1;
        const int y = cy + 25;
        const uint16_t fringe = kit::mix(kit::rgb(77, 106, 159), kit::rgb(155, 181, 246), flash);
        const uint16_t core = kit::mix(kit::rgb(140, 165, 222), white, flash);
        c.thick(x, y, x - 13 * side, y + 24, 3, fringe);
        c.thick(x - 13 * side, y + 24, x + 2 * side, y + 20, 3, fringe);
        c.thick(x + 2 * side, y + 20, x - 21 * side, y + 58, 3, fringe);
        c.line(x, y, x - 13 * side, y + 24, core);
        c.line(x - 13 * side, y + 24, x + 2 * side, y + 20, core);
        c.line(x + 2 * side, y + 20, x - 21 * side, y + 58, core);
        c.line(x - 5 * side, y + 32, x + 12 * side, y + 42, fringe);
        c.line(x + 12 * side, y + 42, x + 17 * side, y + 53, fringe);
        c.line(x - 15 * side, y + 48, x - 30 * side, y + 53, fringe);
    }

    static const kit::Sprite *const kGauge[6] = {&kArt_gauge_0, &kArt_gauge_1, &kArt_gauge_2, &kArt_gauge_3, &kArt_gauge_4, &kArt_gauge_5};
    c.blit(*kGauge[kit::clampi((rain + 19) / 20, 0, 5)], ps, kGaugeX, kGaugeY);
    static const kit::Sprite *const kSock[3][2] = {{&kArt_sock_0_0, &kArt_sock_0_1}, {&kArt_sock_1_0, &kArt_sock_1_1}, {&kArt_sock_2_0, &kArt_sock_2_1}};
    const int sock = wind < 30 ? 0 : (wind < 68 ? 1 : 2);
    c.blit(*kSock[sock][(ms / (windv > 0.3f ? 110u : 420u)) & 1u], ps, kSockX, kSockY);
    if (st->wet > 0.25f) c.blit(kArt_puddle_0, ps, 150, 153);
    if (st->wet > 0.5f) c.blit(kArt_puddle_1, ps, 222, 146);
    if (st->wet > 0.75f) c.blit(kArt_puddle_0, ps, 96, 141);

    // the sheep jumps out of its wool at a thunder clap, and chews otherwise
    const bool jumpy = ts < 900u;
    const int leap = jumpy ? (int)(7.0f * resona_sinf(3.1416f * (float)ts / 900.0f)) : 0;
    c.blit(jumpy ? kArt_sheep_1 : kArt_sheep_0, ps, kSheepX, kSheepY - leap + (!jumpy && ((ms / 700u) & 3u) == 0u ? 1 : 0));

    // the kid: umbrella up in the rain, inside out in a gale
    const int pose = windv > 0.55f ? 2 : (rainv > 0.1f ? 1 : 0);
    const int hop = ts < 300u ? -3 : 0;
    const int px = kPersonX, py = kPersonY + hop;
    const kit::Sprite *kid = pose == 2 ? &kArt_person_blown : (pose == 1 ? &kArt_person_brolly : &kArt_person_calm);
    const kit::Sprite *brolly = pose == 2 ? &kArt_umbrella_flipped : (pose == 1 ? &kArt_umbrella_open : &kArt_umbrella_closed);
    const int wob = pose == 2 ? (int)((ms / 120u) & 1u) : 0;
    if (pose) c.blit(*brolly, ps, px + kHand[pose][0] + wob, py - 36 + kHand[pose][1]);
    c.blit(*kid, ps, px, py);
    if (!pose) c.blit(*brolly, ps, px + kHand[0][0], py - 36 + kHand[0][1] + 9);   // furled, like a walking stick

    // wind: streaks and leaves blowing through
    const int gusts = windv > 0.2f ? 1 + (int)(windv * 3.9f) : 0;
    for (int k = 0; k < gusts; ++k) {
        const uint32_t period = 1500u + (uint32_t)k * 370u;
        const uint32_t lap = (ms + (uint32_t)k * 911u) / period;
        const int x = (int)(((ms + (uint32_t)k * 911u) % period) * 400u / period) - 40;
        const int y = 62 + (int)(hash(lap * 7u + (uint32_t)k) % 76u);
        c.blit(k & 1 ? kArt_gust_1 : kArt_gust_0, pc, x, y);
    }
    const int leaves = windv > 0.3f ? 2 + (int)(windv * 4.0f) : 0;
    for (int k = 0; k < leaves; ++k) {
        const uint32_t period = 1100u + (uint32_t)k * 230u;
        const uint32_t t = (ms + (uint32_t)k * 517u) % period, lap = (ms + (uint32_t)k * 517u) / period;
        const int x = (int)(t * 360u / period) - 20;
        const int y = 70 + (int)(hash(lap * 13u + (uint32_t)k * 5u) % 60u) + (int)(6.0f * resona_sinf((float)t * 0.012f + (float)k));
        c.blit(kArt_leaf, ps, x, y, ((t / 150u) & 1u) != 0u);
    }

    // rain: a drizzle under the cloud, then the whole sky once the storm front is in
    const uint16_t drop_hi = kit::mix(kit::rgb(196, 218, 255), white, flash), drop_lo = kit::mix(kit::rgb(120, 158, 224), white, flash);
    const int drops = (int)(rainv * 145.0f);
    const int far_fraction = (int)(kit::clampf((dark - .22f) * 2.3f, 0, 1) * 255);
    const float slant = 0.08f + 0.5f * windv;
    for (int k = 0; k < drops; ++k) {
        const uint32_t h = hash((uint32_t)k * 2654435761u + 77u);
        const bool wide = (k & 1) != 0;                        // every other drop belongs to the storm front
        if (wide && (int)((h >> 24) & 255u) >= far_fraction) continue;
        const int top = wide ? 14 : cy + 27;
        const int x0 = wide ? (int)(h % 380u) - 60 : cx - 66 + (int)(h % 132u);
        const int depth = (int)((h >> 9) % 34u);
        const int span = 150 - top + depth;
        const uint32_t speed = (wide ? 120u : 210u) + ((h >> 16) % 85u);      // px per second
        const int fall = (int)((ms * speed / 1000u + (h >> 4)) % (uint32_t)span);
        const int y = top + fall;
        const int x = x0 + (int)((float)fall * slant);
        if (x < 0 || x >= 320) continue;
        const int ground = kHillY[x] + 3 + depth;
        if (y >= ground) {                                    // it has landed: a little splash
            if (y < ground + 4) { c.put(x - 2, ground - 1, drop_hi); c.put(x + 2, ground - 1, drop_hi); c.put(x, ground, drop_lo); }
            continue;
        }
        const int length = wide ? 3 + (int)((h >> 20) % 4u) : 6 + (int)((h >> 20) % 7u);
        for (int j = 0; j < length; ++j) {
            const int rx = x - (int)((float)j * slant), ry = y - j;
            if (ry < top || rx < 0 || rx >= c.W || ry >= c.H) continue;
            uint16_t &pixel = c.px[ry * c.stride + rx];
            const float strength = (wide ? .30f : .86f) * (1.0f - (float)j / length);
            pixel = kit::mix(pixel, drop_hi, strength);
        }
    }

    // a knob's value shows for a moment after it is turned, by the thing it changes
    if (st->touched && ms - st->touched_at < 1300u) {
        char num[16];
        if (st->touched == 1) { P->format(num, sizeof num, "%d", st->s.rain); c.text_s(kGaugeX + 6 - c.text_w(1, num) / 2, kGaugeY - 11, 1, cream, ink, num); }
        else if (st->touched == 2) { P->format(num, sizeof num, "%d", st->s.wind); c.text_s(kSockX + 4 - c.text_w(1, num) / 2, kSockY - 10, 1, cream, ink, num); }
        else c.text_s(206 - c.text_w(1, kSpaceName[st->s.space]) / 2, 104, 1, cream, ink, kSpaceName[st->s.space]);
    }

#if !defined(RESONA_LAB_HOST)
    c.banner(kit::audio_message(st->audio, "INSTALL STORM VOICE IN LAB"), ink, kit::rgb(252, 236, 150));
#endif
    if (intro < 1.0f) {
        for (int y = 0; y < c.H; ++y) {
            uint16_t *row = c.px + y * c.stride;
            for (int x = 0; x < c.W; ++x) row[x] = kit::mix(0, row[x], reveal);
        }
    }
    c.present(P);
}

/* ---- the App ---------------------------------------------------------------------------- */

int init(void *state, const resona_platform *platform)
{
    State *st = static_cast<State *>(state);
    P = platform;
    const int startup = resona_startup_splash_apply(P, kSplash, sizeof kSplash);
    if (startup < 0 && startup != RESONA_ERR_UNSUPPORTED)
        P->log("Startup animation could not be saved");
    defaults(st);
    Saved v;
    SavedV1 v1;
    if (kit::load(P, v, kMagic)) st->s = v;
    else if (kit::load(P, v1, kMagicV1)) { st->s.rain = v1.rain; st->s.wind = v1.wind; st->s.thunder = v1.thunder; st->s.space = v1.space; }
    Saved &s = st->s;
    s.rain = kit::clampi(s.rain, 0, 100); s.wind = kit::clampi(s.wind, 0, 100); s.thunder = s.thunder ? 1 : 0;
    s.space = kit::clampi(s.space, 0, 2); s.thunder_amt = kit::clampi(s.thunder_amt, 0, 100);
    s.mix = kit::clampi(s.mix, 0, 100); s.level = kit::clampi(s.level, -12, 6);
    bind(st);
    st->seed = 0x57E47u;
    st->blink_at = 0u - 1000u;
    st->next_blink = 3100u;
    st->rain_at = st->wind_at = st->space_at = kNever;
    push(st);
    return 0;
}

void event(void *state, const resona_app_event *ev)
{
    State *st = static_cast<State *>(state);
    if (kit::audio_event(P, st->audio, ev, st->ms)) return;
    ctl::Msg m;
    if (ctl::receive(P, st->link, kBind, kBinds, ev, m)) {
        if (!m.changed) return;
        if (m.cc == CC_DOWNPOUR) st->downpour = m.down;
        else if (m.cc == CC_GALE) st->gale = m.down;
        else if (m.cc == CC_STORM) { st->gust = m.down; if (m.down) strike(st); }
        else if (m.cc == CC_SUN) st->sun = m.down;
        else kit::save_later(st->saver, st->ms);
        if (m.cc == CC_RAIN) { st->rain_at = st->ms; st->touched = 1; st->touched_at = st->ms; }
        else if (m.cc == CC_WIND) { st->wind_at = st->ms; st->touched = 2; st->touched_at = st->ms; }
        else if (m.cc == CC_SPACE) { st->space_at = st->ms; st->touched = 3; st->touched_at = st->ms; }
        else if (m.cc == CC_THUNDER && st->s.thunder) strike(st);
        st->idle = 0;
        push(st);
        return;
    }
    if (ev->kind != RESONA_APP_EV_INPUT) return;
    const int top = ev->u.input.input == RESONA_INPUT_SELECTOR;
    const int kind = (int)ev->u.input.kind, v = ev->u.input.value;
    if (kind == RESONA_EVENT_TURN) {
        if (top) { st->s.rain = kit::clampi(st->s.rain + v * 2, 0, 100); st->rain_at = st->ms; }     // a shower, to show it
        else { st->s.wind = kit::clampi(st->s.wind + v * 2, 0, 100); st->wind_at = st->ms; }          // a gust
        st->touched = top ? 1 : 2;
        st->touched_at = st->ms;
    } else if (kind == RESONA_EVENT_CLICK) {
        if (top) { st->s.thunder ^= 1; if (st->s.thunder) strike(st); }
        else { st->s.space = (st->s.space + 1) % 3; st->space_at = st->ms; st->touched = 3; st->touched_at = st->ms; }
    } else if (kind == RESONA_EVENT_LONG && !top) {
        defaults(st);
        st->rain_at = st->wind_at = st->ms;
    } else {
        return;
    }
    st->idle = 0;
    push(st);
    kit::save_later(st->saver, st->ms);
}

void frame(void *state, uint32_t ms)
{
    State *st = static_cast<State *>(state);
    st->ms += ms;
    kit::audio_tick(P, st->audio, kPack, PARAMS, st->ms);
    kit::levels_tick(P, st->lv, ms);
    kit::save_tick(P, st->saver, st->s, st->ms);
    ctl::sync(P, st->link, kBind, kBinds, st->ms);
    if (st->lv.raw_in >= 2) st->idle = 0; else st->idle += ms;
    if (held(st)) st->rain_at = st->wind_at = st->ms;   // a held pad shows its weather even before you play
    const int rain = rain_now(st), wind = wind_now(st);
    const float dt = (float)ms;

    // the storm builds quickly and blows over slowly
    const float target = kit::clampf(st->lv.in * 1.25f, 0.0f, 1.0f);
    st->storm += (target - st->storm) * (1.0f - resona_expf(-dt / (target > st->storm ? 350.0f : 2600.0f)));
    if ((st->s.thunder || st->gust) && st->lv.hit > 0.95f && (!st->struck || st->ms - st->strike_at > 3000u)) strike(st);

    // how black the sky is: the playing, scaled by how much weather is dialled in; a touched
    // knob or a thunder clap darkens it for a moment too
    const float rain_set = (float)rain * 0.01f, wind_set = (float)wind * 0.01f;
    const float most = rain_set > wind_set ? rain_set : wind_set;
    float want = st->sun ? 0.0f : st->storm * (0.35f + 0.65f * most) * 1.15f;
    const float tr = since(st, st->rain_at, 2600.0f) * (0.25f + 0.5f * rain_set) * (rain ? 1.0f : 0.0f);
    const float tw = since(st, st->wind_at, 2600.0f) * (0.15f + 0.5f * wind_set);
    const float tl = st->struck && st->ms - st->strike_at < 1500u ? 0.85f : 0.0f;
    if (tr > want) want = tr;
    if (tw > want) want = tw;
    if (tl > want) want = tl;
    want = kit::clampf(want, 0.0f, 1.0f);
    st->dark += (want - st->dark) * (1.0f - resona_expf(-dt / (want > st->dark ? 220.0f : 700.0f)));

    // the ground soaks up the rain and dries in the sun
    const float tease = since(st, st->rain_at, 2600.0f);
    const float falling = rain ? (st->storm > tease ? st->storm : tease) * (0.2f + 0.8f * rain_set) : 0.0f;
    st->wet = kit::clampf(st->wet + (falling > 0.05f ? falling * dt / 5000.0f : -dt / 14000.0f), 0.0f, 1.0f);

    if ((int32_t)(st->ms - st->next_blink) >= 0) {
        st->blink_at = st->ms;
        st->next_blink = st->ms + 2600u + rnd(st) % 3000u;
    }
    if ((int32_t)(st->ms - st->next_draw) >= 0) {
        st->next_draw = st->ms + 30u;
        draw(st);
    }
}

const resona_app kApp = {
    RESONA_PLATFORM_VERSION, sizeof(resona_app),
    RESONA_APP_ID, RESONA_APP_NAME, RESONA_APP_VERSION,
    sizeof(State), 0,
    init, event, frame,
};

}  // namespace

extern "C" const resona_app *resona_app_get(void) { return &kApp; }
