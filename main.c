// pwl-test: demo / control CLI for the PiecewiseOrionSynth peripheral
// (pwl_synth, peripheral 33 by Toivo Henningsson) on TinyQV.
//
// A 'pwl>' console in the style of the prism-test CLI: play notes and
// chords, pick waveform presets, drive hardware sweeps, percussion,
// stereo, raw register access, demo songs - plus a small ms-timestamped
// event sequencer that demo songs run on and that a future MIDI-file
// converter can target.
//
// Audio: PWM on uo_out[7] (Mike's audio PMOD).  In stereo mode
// uo_out[7] = right, uo_out[6] = left (alternating pins).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <csr.h>
#include <uart.h>
#include <pwl_synth.h>
#include <tqv_fs.h>
#include "pwl_test.h"

// ==========================================================================
// Note progression output.  The TUI hooks this to stream notes into its
// per-channel Notes tab; on the plain console it stays NULL and the
// notes print inline as before (see pwl_test.h).
// ==========================================================================
void (*pwl_note_out)(int channel, const char *text);
void (*pwl_note_clear)(void);

// 'play q' / 'demo <n> q': no note streaming while the song runs (the
// drawing delays pwl_env_service on fast songs)
volatile uint8_t seq_quiet;

bool seq_quiet_arg(const char *s)
{
    return !strcmp(s, "q") || !strcmp(s, "-q") || !strcmp(s, "quiet");
}

// Note stream: to the Notes tab when hooked, else to stdout
static void note_out(int channel, const char *text)
{
    if (seq_quiet)
        return;
    if (pwl_note_out)
        pwl_note_out(channel, text);
    else
        printf("%s ", text);
}

// Events the console never printed (percussion, single 'note' commands
// that print their own detail line): report them to the tab only
static void note_tab(int channel, const char *text)
{
    if (seq_quiet)
        return;
    if (pwl_note_out)
        pwl_note_out(channel, text);
}

// ==========================================================================
// Small console plumbing (same conventions as the prism> loop)
// ==========================================================================
char *cli_readline(void)
{
    static char buf[96];
    unsigned len = 0;

    for (;;) {
        int c = uart_getc();

        if (c == -1) {
            pwl_env_service();      // ADSR attack->decay transitions
            continue;
        }
        if (c == '\r' || c == '\n') {
            putchar('\n');
            buf[len] = '\0';
            return buf;
        }
        if (c == '\b' || c == 0x7F) {
            if (len) {
                --len;
                printf("\b \b");
            }
            continue;
        }
        if (c >= ' ' && c < 0x7F && len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
            putchar(c);
        }
    }
}

int cli_split(char *line, char *argv[], int max)
{
    int argc = 0;

    while (*line && argc < max) {
        while (*line == ' ')
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ')
            ++line;
    }
    return argc;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    uint32_t v = strtoul(s, &end, 0);

    if (end == s || *end) {
        printf("bad number: '%s'\n", s);
        return false;
    }
    *out = v;
    return true;
}

// Parse "C4", "A#3", "Eb5" or a plain MIDI number; -1 on error
static int parse_note(const char *s)
{
    static const int8_t base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
    int semi, oct;

    if (s[0] >= '0' && s[0] <= '9')
        return atoi(s);

    char c = s[0] & ~0x20;                  // upper-case
    if (c < 'A' || c > 'G')
        return -1;
    semi = base[c - 'A'];
    ++s;
    if (*s == '#') { ++semi; ++s; }
    else if (*s == 'b') { --semi; ++s; }
    if (*s == '-' || (*s >= '0' && *s <= '9'))
        oct = atoi(s);
    else
        return -1;
    return 12 * (oct + 1) + semi;           // C4 = 60
}

const char *note_name(int midi)
{
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B" };
    static char buf[8];

    snprintf(buf, sizeof(buf), "%s%d", names[midi % 12], midi / 12 - 1);
    return buf;
}

// ==========================================================================
// Waveform presets (from the pwl_synth documentation's cookbook)
// ==========================================================================
typedef struct {
    const char *name;
    const char *desc;
    uint8_t  slope_r, slope_f, pwm_offset;
    uint16_t mode;
} preset_t;

static const preset_t presets[] = {
    { "tri",     "triangle (darkens as amp drops)",     0,      0,      0,   0 },
    { "nes",     "4-bit triangle (NES style)",          0,      0,      0,   PWL_MODE_OSC_SYNC(PWL_SYNC_4BIT) },
    { "square",  "square wave",                         12*16,  12*16,  0,   0 },
    { "saw",     "sawtooth-like",                       0,      12*16,  0,   0 },
    { "pulse25", "25% duty pulse",                      12*16,  12*16,  128, 0 },
    { "pulse12", "12.5% duty pulse",                    12*16,  12*16,  192, 0 },
    { "sine",    "sine approximation",                  8,      8,      0,   0 },
    { "clamp",   "clamped triangle (bright, loud)",     16,     16,     0,   0 },
    { "organ",   "1x+2x octave stack, soft",            8,      8,      0,   PWL_MODE_FREQ_MULTS(2) },
    { "power",   "distorted power chord (ch0 only)",    0,      12*16,  0,   PWL_MODE_FREQ_MULTS(3) | PWL_MODE_COMMON_SAT },
    { "third",   "just major third stack (ch0 only)",   8,      8,      0,   PWL_MODE_FREQ_MULTS(5) | PWL_MODE_DETUNE_5TH | PWL_MODE_COMMON_SAT },
    { "noise",   "LFSR noise (18-bit on ch 0/3)",       0,      0,      0,   PWL_MODE_WF(PWL_WF_NOISE) },
    { "orion",   "Orion Wave (rich harmonics)",         3,      0x1B,   255, PWL_MODE_WF(PWL_WF_ORION) },
    { "smooth",  "PWL osc triangle (no quantization)",  16,     16,     0,   PWL_MODE_WF(PWL_WF_PWL_OSC) },
    // Bowed string: a saw is the bowed-string spectrum, but a 12x saw is
    // all buzz - 6x rolls the upper harmonics off about where a violin
    // does.  The PWL oscillator matters more than the shape here: a
    // quiet sustained tone is exactly where the linear osc's phase
    // quantization is audible.  The pwm offset skews the wave for the
    // reedy midrange.  Bow it with 'adsr <ch> 10 0 7 11' + 'vib'.
    { "violin",  "bowed string (soft saw, PWL osc)",    0,      6*16,   32,  PWL_MODE_WF(PWL_WF_PWL_OSC) },
};
#define NUM_PRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

static int find_preset(const char *name)
{
    for (int i = 0; i < NUM_PRESETS; ++i)
        if (!strcmp(name, presets[i].name))
            return i;
    return -1;
}

// Preset table accessors for the TUI (tab completion, watch decode)
int pwl_preset_count(void)
{
    return NUM_PRESETS;
}

const char *pwl_preset_name(int idx)
{
    return (idx >= 0 && idx < NUM_PRESETS) ? presets[idx].name : NULL;
}

const char *pwl_preset_desc(int idx)
{
    return (idx >= 0 && idx < NUM_PRESETS) ? presets[idx].desc : NULL;
}

static void apply_preset(int ch, int p)
{
    pwl_set_waveform(ch, presets[p].slope_r, presets[p].slope_f,
                     presets[p].pwm_offset, presets[p].mode);
}

// Instrument-panel accessors (tui/PwlSynth.cxx)
void pwl_apply_preset(int ch, int idx)
{
    if (ch >= 0 && ch < PWL_NUM_CHANNELS && idx >= 0 && idx < NUM_PRESETS)
        apply_preset(ch, idx);
}

// Match a channel's cached waveform back to a preset, ignoring the
// per-note detune fields the driver rewrites; -1 = custom
int pwl_match_preset(int ch)
{
    const pwl_channel_state_t *c = &pwl_ch[ch];
    uint16_t mask = (uint16_t)~(7u | PWL_MODE_DETUNE_5TH);

    for (int i = 0; i < NUM_PRESETS; ++i)
        if (c->slope_r == presets[i].slope_r &&
            c->slope_f == presets[i].slope_f &&
            c->pwm_offset == presets[i].pwm_offset &&
            (c->mode & mask) == (presets[i].mode & mask))
            return i;
    return -1;
}

// ==========================================================================
// Percussion recipes (docs: fast period sweep up = falling pitch)
// ==========================================================================
static void perc_kick(int ch)
{
    pwl_set_waveform(ch, 16, 16, 0, 0);
    pwl_set_sweeps(ch, pwl_sweep_pa(false, 1, 0, 8), 0);
    pwl_note_on(ch, 60, 63);
    note_tab(ch, "K");
}

static void perc_snare(int ch)
{
    pwl_set_waveform(ch, 0, 0, 0, PWL_MODE_WF(PWL_WF_NOISE));
    pwl_set_sweeps(ch, pwl_sweep_pa(false, 1, 0, 10), 0);
    pwl_note_on(ch, 105, 63);
    note_tab(ch, "S");
}

static void perc_hat(int ch)
{
    pwl_set_waveform(ch, 0, 0, 0, PWL_MODE_WF(PWL_WF_NOISE));
    pwl_set_sweeps(ch, pwl_sweep_pa(false, 0, 0, 8), 0);
    pwl_note_on(ch, 116, 40);
    note_tab(ch, "H");
}

// ==========================================================================
// Sequencer: ms-timestamped events (format in seq.h).  Demo songs are
// tables of these; tools/mid2pwl.py generates the same format from
// MIDI files.
// ==========================================================================
#include "seq.h"

extern const seq_ev_t song_paradise[];      // song_paradise.c (mid2pwl.py)
extern const seq_ev_t song_paradise_v[];    // song_paradise_v.c: lead-vocal
                                            //   melody (MIDI ch3) first
extern const seq_ev_t song_axelf[];         // song_axelf.c (mid2pwl.py)
extern const seq_ev_t song_axelf_a[];       // song_axelf_a.c: + ADSR
extern const seq_ev_t song_gmlast[];        // song_gmlast.c: satb split
                                            //   of a solo piano piece
extern const seq_ev_t song_willie[];        // song_willie.c: vocal-priority
extern const seq_ev_t song_skybells[];       // song_skybells.c: demo 16
                                            //   mix + pitch-env scoops

// Neutral starting state for a demo/song: silence everything and clear
// the per-channel caches (sweeps, detune).  Without this, note_on
// re-arms whatever sweep the PREVIOUS demo cached - e.g. Sunburst's
// stab decay left on the pad channel made the next song's opening
// chords die in ~120ms.
static void seq_reset(void)
{
    if (pwl_note_clear)         // new piece: start the TUI note tracks empty
        pwl_note_clear();

    for (int ch = 0; ch < PWL_NUM_CHANNELS; ++ch) {
        pwl_adsr_off(ch);           // before note_off: hard silence, and
        pwl_note_off(ch);           //   no stale envelope for the next song
        pwl_pitch_env_off(ch);
        pwl_timbre_env_off(ch);
        pwl_vibrato_off(ch);
        pwl_set_sweeps(ch, 0, 0);
        pwl_set_arp(ch, 0, 0);
        pwl_ch[ch].relative_detune = 0;
    }
}

// Returns 1 if stopped by keypress
int seq_play(const seq_ev_t *ev, const char *name)
{
    printf("~~~ %s ~~~  (any key stops)\n", name);
    seq_reset();
    int stopped = 0;

    for (; ev->cmd != EV_END && !stopped; ++ev) {
        uint32_t start = read_time();
        while ((uint32_t)(read_time() - start) < (uint32_t)ev->dt_ms * 1000u) {
            pwl_env_service();
            if (uart_getc() != -1) {
                stopped = 1;
                break;
            }
        }
        if (stopped)
            break;

        switch (ev->cmd) {
        case EV_ON:
            pwl_note_on(ev->ch, ev->a, ev->b);
            note_out(ev->ch, note_name(ev->a));
            break;
        case EV_OFF:      pwl_note_off(ev->ch); break;
        case EV_PRESET:   apply_preset(ev->ch, ev->a); break;
        case EV_SWEEP_PA: pwl_set_sweeps(ev->ch,
                              (uint16_t)(ev->a | (ev->b << 8)),
                              pwl_ch[ev->ch].sweep_ws); break;
        case EV_SWEEP_WS: pwl_set_sweeps(ev->ch, pwl_ch[ev->ch].sweep_pa,
                              (uint16_t)(ev->a | (ev->b << 8))); break;
        case EV_DETUNE:   pwl_ch[ev->ch].relative_detune = (int8_t)ev->a; break;
        case EV_ADSR:
            if (ev->a == 0xFF)
                pwl_adsr_off(ev->ch);
            else
                pwl_set_adsr(ev->ch, ev->a >> 4, ev->a & 15,
                             ev->b >> 4, ev->b & 15);
            break;
        case EV_PENV:
            if (ev->a == 0 && ev->b == 0)
                pwl_pitch_env_off(ev->ch);
            else
                pwl_set_pitch_env(ev->ch, (int8_t)ev->a, ev->b & 15,
                                  ev->b >> 4, false);
            break;
        case EV_TPWM:
            pwl_set_timbre_env(ev->ch, (int16_t)((int8_t)ev->a) * 2, ev->b,
                               pwl_ch[ev->ch].tenv_slope_delta,
                               pwl_ch[ev->ch].tenv_slope_rate,
                               pwl_ch[ev->ch].tenv_slope_dir);
            break;
        case EV_TSLOPE:
            pwl_set_timbre_env(ev->ch, pwl_ch[ev->ch].tenv_pwm_delta,
                               pwl_ch[ev->ch].tenv_pwm_rate,
                               (int8_t)ev->a, ev->b & 15,
                               (ev->b >> 4) & 3);
            break;
        case EV_VIBRATO:
            if (ev->a == 0)
                pwl_vibrato_off(ev->ch);
            else
                pwl_set_vibrato(ev->ch, ev->a, (uint8_t)((ev->b & 63) * 2),
                                (uint16_t)((ev->b >> 6) * 250));
            break;
        case EV_SLUR: {
            char sname[12];

            pwl_note_slur(ev->ch, ev->a, ev->b);
            snprintf(sname, sizeof(sname), "~%s", note_name(ev->a));
            note_out(ev->ch, sname);
            break;
        }
        case EV_ARPIV:    pwl_set_arp(ev->ch, ev->a, ev->b); break;
        case EV_ARPRATE:  pwl_set_arp_rate(ev->ch, ev->a); break;
        case EV_KICK:     perc_kick(ev->ch); break;
        case EV_SNARE:    perc_snare(ev->ch); break;
        case EV_HAT:      perc_hat(ev->ch); break;
        case EV_PERIOD:   pwl_write(ev->ch, PWL_REG_PERIOD,
                                    (uint16_t)(ev->a | (ev->b << 8))); break;
        case EV_AMP:      pwl_write(ev->ch, PWL_REG_AMP, ev->a & 63); break;
        case EV_VIB: {
            // Blocking vibrato: wobble the raw mantissa around the note
            // (period writes never click, per the synth docs)
            uint16_t fp  = pwl_note_fperiod(ev->a);
            uint16_t man = fp & 0x3FF;
            uint16_t hi  = (uint16_t)((fp & ~0x3FF) |
                                      (man + 20 > 1023 ? 1023 : man + 20));
            uint16_t lo  = (uint16_t)((fp & ~0x3FF) |
                                      (man < 20 ? 0 : man - 20));
            char vname[12];

            pwl_note_on(ev->ch, ev->a, 60);
            snprintf(vname, sizeof(vname), "%s~", note_name(ev->a));
            note_out(ev->ch, vname);
            for (int c = 0; c < ev->b && !stopped; ++c) {
                for (int half = 0; half < 2 && !stopped; ++half) {
                    pwl_write(ev->ch, PWL_REG_PERIOD, half ? lo : hi);
                    uint32_t vs = read_time();
                    while ((uint32_t)(read_time() - vs) < 48000u) {
                        pwl_env_service();
                        if (uart_getc() != -1) {
                            stopped = 1;
                            break;
                        }
                    }
                }
            }
            pwl_write(ev->ch, PWL_REG_PERIOD, fp);
            break;
        }
        default: break;
        }
    }
    pwl_all_off();
    printf("\n%s\n", stopped ? "stopped" : "done");
    return stopped;
}

// --------------------------------------------------------------------------
// Demo 1: waveform tour - the same phrase in each preset
// --------------------------------------------------------------------------
static const int tour_order[] = { 0, 1, 2, 3, 4, 6, 7, 13, 12 };

static void demo_tour(void)
{
    printf("~~~ waveform tour ~~~  (any key stops)\n");
    seq_reset();
    static const uint8_t phrase[] = { 60, 64, 67, 72 };

    for (unsigned p = 0; p < sizeof(tour_order)/sizeof(tour_order[0]); ++p) {
        int pi = tour_order[p];
        printf("[%s] ", presets[pi].name);
        apply_preset(0, pi);
        pwl_ch[0].relative_detune = 0;
        for (unsigned n = 0; n < sizeof(phrase); ++n) {
            pwl_note_on(0, phrase[n], 63);
            note_out(0, note_name(phrase[n]));
            uint32_t start = read_time();
            while ((uint32_t)(read_time() - start) < 280000u) {
                if (uart_getc() != -1) {
                    pwl_all_off();
                    printf("\nstopped\n");
                    return;
                }
            }
        }
        pwl_note_off(0);
        printf("\n");
    }
    pwl_all_off();
    printf("done\n");
}

// --------------------------------------------------------------------------
// Demo 2: chord fade-in with detune (from the docs example, Dm7-ish),
// then a power-chord sting on channel 0
// --------------------------------------------------------------------------
#define FADE_IN(t)  (uint8_t)(PWL_SWEEP_PA(false, 0, t, 12) & 0xFF), \
                    (uint8_t)(PWL_SWEEP_PA(false, 0, t, 12) >> 8)

static const seq_ev_t demo_chord[] = {
    { 0,   EV_PRESET,   0, 0, 0, },     // tri on all channels
    { 0,   EV_PRESET,   1, 0, 0, },
    { 0,   EV_PRESET,   2, 0, 0, },
    { 0,   EV_PRESET,   3, 0, 0, },
    // fade each note in via amp sweep toward target 7 (=63)
    { 0,   EV_SWEEP_PA, 0, FADE_IN(7) },
    { 0,   EV_ON,       0, 50, 0 },     // D3
    { 500, EV_SWEEP_PA, 1, FADE_IN(7) },
    { 0,   EV_ON,       1, 55, 0 },     // G3
    { 500, EV_SWEEP_PA, 2, FADE_IN(7) },
    { 0,   EV_ON,       2, 60, 0 },     // C4
    { 500, EV_SWEEP_PA, 3, FADE_IN(7) },
    { 0,   EV_ON,       3, 63, 0 },     // Eb4
    { 2500, EV_OFF,     0 },
    { 0,   EV_OFF,      1 },
    { 0,   EV_OFF,      2 },
    { 0,   EV_OFF,      3 },
    // power chord sting: ch0 common_sat + (3x,2x) mults, one octave down
    { 400, EV_PRESET,   0, 9 },
    { 0,   EV_ON,       0, 45, 63 },    // sounds ~A3 power chord
    { 1200, EV_SWEEP_PA, 0,
      (uint8_t)(PWL_SWEEP_PA(false, 0, 0, 10) & 0xFF),
      (uint8_t)(PWL_SWEEP_PA(false, 0, 0, 10) >> 8) },
    { 1500, EV_OFF,     0 },
    { 0,   EV_END }
};

// --------------------------------------------------------------------------
// Demo 3: four-channel groove - bass, lead w/ detune, drums
// --------------------------------------------------------------------------
#define BAR(bass, l1, l2, l3, l4)                          \
    { 0,   EV_ON,    1, bass, 55 }, { 0, EV_KICK, 3 },     \
    { 0,   EV_ON,    2, l1, 50 },                          \
    { 250, EV_HAT,   3 },                                  \
    { 250, EV_SNARE, 3 }, { 0, EV_ON, 2, l2, 50 },         \
    { 250, EV_HAT,   3 },                                  \
    { 250, EV_KICK,  3 }, { 0, EV_ON, 2, l3, 50 },         \
    { 250, EV_HAT,   3 },                                  \
    { 250, EV_SNARE, 3 }, { 0, EV_ON, 2, l4, 50 },         \
    { 250, EV_HAT,   3 },                                  \
    { 250, EV_OFF,   2 },

static const seq_ev_t demo_groove[] = {
    { 0, EV_PRESET, 1, 3 },             // bass: saw
    { 0, EV_PRESET, 2, 4 },             // lead: pulse25
    { 0, EV_DETUNE, 2, 12 },            // rich detune on the lead
    BAR(38, 62, 65, 69, 65)             // A1 bass, A-minor lead
    BAR(38, 62, 65, 69, 72)
    BAR(41, 65, 69, 72, 69)             // F2
    BAR(43, 67, 71, 74, 71)             // G2
    { 0, EV_OFF, 1 }, { 0, EV_OFF, 2 },
    { 0, EV_END }
};

// --------------------------------------------------------------------------
// Demo 4: Fur Elise (Beethoven, A section) - two passes.
//   Pass 1: sine melody (ch2, gentle detune) over triangle bass arpeggios
//           (ch1) - the RH/LH counterpoint of the score.
//   Pass 2: organ preset (1x+2x octave stack) up an octave, NES 4-bit
//           bass, slope sweeps for phrasing.
//   Ending: rolled A-minor chord across three channels, hardware fade.
// --------------------------------------------------------------------------
#define E8 170                                  // eighth note (ms)
#define MEL(n)   { E8, EV_ON, 2, n, 52 }        // melody eighth (ch2)
#define MEL2(n)  { E8, EV_ON, 2, (n) + 12, 45 } // pass-2 melody, octave up
#define WBASS(n) { 0,  EV_ON, 1, n, 38 }        // bass under a melody note
#define BASS(n)  { E8, EV_ON, 1, n, 38 }        // bass eighth alone
#define PA_B(pd, pr, at, ar) \
    (uint8_t)(PWL_SWEEP_PA(pd, pr, at, ar) & 0xFF), \
    (uint8_t)(PWL_SWEEP_PA(pd, pr, at, ar) >> 8)
#define WS_B(wd, wr, sd, dir, sr) \
    (uint8_t)(PWL_SWEEP_WS(wd, wr, sd, dir, sr) & 0xFF), \
    (uint8_t)(PWL_SWEEP_WS(wd, wr, sd, dir, sr) >> 8)

// One statement of the A section: the famous run, three arpeggio bars,
// the run again, and the closing figure (second_ending: E4 C5 B4 -> A4)
#define ELISE_RUN \
    MEL(76), MEL(75), MEL(76), MEL(75), MEL(76), MEL(71), MEL(74), MEL(72)
#define ELISE_BODY(MELX)                                              \
    MELX(69), WBASS(45), BASS(52), BASS(57),                          \
    MELX(60), MELX(64), MELX(69),                                     \
    MELX(71), WBASS(40), BASS(52), BASS(56),                          \
    MELX(64), MELX(68), MELX(71),                                     \
    MELX(72), WBASS(45), BASS(52), BASS(57),                          \
    MELX(64), MELX(76), MELX(75)

#define ELISE_RUN2 \
    MEL2(76), MEL2(75), MEL2(76), MEL2(75), MEL2(76), MEL2(71), MEL2(74), MEL2(72)
#define ELISE_CLOSE(MELX)                                             \
    MELX(69), WBASS(45), BASS(52), BASS(57),                          \
    MELX(60), MELX(64), MELX(69),                                     \
    MELX(71), WBASS(40), BASS(52), BASS(56),                          \
    MELX(64), MELX(72), MELX(71)

static const seq_ev_t demo_elise[] = {
    // pass 1: sine melody, triangle bass, light detune
    { 0, EV_PRESET, 2, 6 },                     // sine
    { 0, EV_PRESET, 1, 0 },                     // tri
    { 0, EV_DETUNE, 2, 6 },
    { 0, EV_DETUNE, 1, (uint8_t)PWL_DETUNE_OFF },
    ELISE_RUN, ELISE_BODY(MEL), ELISE_RUN, ELISE_CLOSE(MEL),
    { E8, EV_ON, 2, 69, 52 }, WBASS(45), BASS(52), BASS(57),
    // phrase end: darken the melody into the repeat
    { 0, EV_SWEEP_WS, 2, WS_B(false, 0, true, 3, 8) },
    { 2 * E8, EV_SWEEP_WS, 2, 0, 0 },
    { 0, EV_OFF, 2 }, { 0, EV_OFF, 1 },

    // pass 2: organ octave stack, NES bass, brighter
    { 2 * E8, EV_PRESET, 2, 8 },                // organ (1x+2x mults)
    { 0, EV_PRESET, 1, 1 },                     // NES 4-bit tri
    { 0, EV_DETUNE, 2, 12 },
    ELISE_RUN2, ELISE_BODY(MEL2), ELISE_RUN2, ELISE_CLOSE(MEL2),
    { E8, EV_ON, 2, 81, 45 }, WBASS(45), BASS(52), BASS(57),

    // ending: rolled A minor, then hardware fade to silence
    { 2 * E8, EV_PRESET, 0, 6 },
    { 0, EV_ON, 1, 45, 40 },                    // A2
    { 130, EV_ON, 0, 64, 40 },                  // E4
    { 130, EV_ON, 2, 69, 45 },                  // A4 (organ adds A5)
    { 500, EV_SWEEP_PA, 0, PA_B(false, 0, 0, 13) },
    { 0, EV_SWEEP_PA, 1, PA_B(false, 0, 0, 13) },
    { 0, EV_SWEEP_PA, 2, PA_B(false, 0, 0, 13) },
    { 2600, EV_END }
};

// --------------------------------------------------------------------------
// Demo 5: jungle adventure - the sounds MIDI can't ask for.
//   dawn: Orion Wave pad shimmer fading in over noise wind
//   birds: fast hardware period sweeps (chirps up and down)
//   tribal: kick/snare groove under a vibrato creature call
//   the swing: octave-long slides up to a wailing vibrato and back
//   night: everything fades, one last owl
// --------------------------------------------------------------------------
#define CHIRP_UP(note, len)                                    \
    { len, EV_ON, 2, note, 46 },                               \
    { 0, EV_SWEEP_PA, 2, PA_B(true, 5, 7, 0) },                \
    { 90, EV_SWEEP_PA, 2, 0, 0 }, { 0, EV_OFF, 2 }
#define CHIRP_DN(note, len)                                    \
    { len, EV_ON, 3, note, 40 },                               \
    { 0, EV_SWEEP_PA, 3, PA_B(false, 6, 7, 0) },               \
    { 70, EV_SWEEP_PA, 3, 0, 0 }, { 0, EV_OFF, 3 }

static const seq_ev_t demo_jungle[] = {
    // --- dawn: orion pad shimmer + wind ---------------------------------
    { 0, EV_PRESET, 0, 12 },                    // orion wave
    { 0, EV_DETUNE, 0, 24 },                    // heavy shimmer
    { 0, EV_ON, 0, 45, 0 },                     // A2, silent
    { 0, EV_SWEEP_PA, 0, PA_B(false, 0, 3, 13) }, // slow fade-in
    { 0, EV_PRESET, 3, 11 },                    // noise wind
    { 400, EV_ON, 3, 95, 10 },
    { 0, EV_SWEEP_PA, 3, PA_B(true, 12, 1, 14) },  // wind slowly rises
    { 4200, EV_SWEEP_PA, 3, PA_B(false, 12, 1, 14) },
    { 2600, EV_OFF, 3 },
    { 0, EV_SWEEP_PA, 3, 0, 0 },

    // --- birds ----------------------------------------------------------
    { 300, EV_PRESET, 2, 6 },                   // sine chirps
    { 0, EV_PRESET, 3, 6 },
    { 0, EV_DETUNE, 2, (uint8_t)PWL_DETUNE_OFF },
    { 0, EV_DETUNE, 3, (uint8_t)PWL_DETUNE_OFF },
    CHIRP_UP(93, 200), CHIRP_UP(96, 260), CHIRP_UP(91, 480),
    CHIRP_DN(89, 350), CHIRP_UP(95, 220), CHIRP_DN(86, 300),
    CHIRP_UP(98, 400), CHIRP_UP(93, 180), CHIRP_DN(90, 500),

    // --- tribal groove + creature call ----------------------------------
    { 600, EV_PRESET, 2, 2 },                   // square creature
    { 0, EV_DETUNE, 2, 0 },
    { 0, EV_KICK, 1 }, { 250, EV_HAT, 3 }, { 250, EV_SNARE, 3 },
    { 250, EV_HAT, 3 }, { 250, EV_KICK, 1 }, { 250, EV_HAT, 3 },
    { 250, EV_SNARE, 3 }, { 250, EV_HAT, 3 },
    { 250, EV_VIB, 2, 52, 5 },                  // E3 growl, 5 vib cycles
    { 0, EV_KICK, 1 }, { 250, EV_HAT, 3 }, { 250, EV_SNARE, 3 },
    { 250, EV_HAT, 3 }, { 250, EV_KICK, 1 }, { 250, EV_HAT, 3 },
    { 250, EV_SNARE, 3 }, { 250, EV_HAT, 3 },
    { 250, EV_VIB, 2, 59, 4 },                  // B3 answer
    { 0, EV_OFF, 2 },

    // --- the swing: big slides (octave up, wail, octave down) -----------
    { 500, EV_PRESET, 2, 3 },                   // saw
    { 0, EV_ON, 2, 57, 55 },                    // A3
    { 0, EV_SWEEP_PA, 2, PA_B(true, 8, 7, 0) },   // slide up (~1s/octave)
    { 950, EV_SWEEP_PA, 2, 0, 0 },
    { 0, EV_VIB, 2, 69, 6 },                    // wail on A4
    { 0, EV_SWEEP_PA, 2, PA_B(false, 8, 7, 0) },  // slide back down
    { 950, EV_SWEEP_PA, 2, 0, 0 },
    { 0, EV_ON, 2, 57, 55 },
    { 400, EV_OFF, 2 },
    // echo answer, quieter
    { 400, EV_ON, 2, 57, 30 },
    { 0, EV_SWEEP_PA, 2, PA_B(true, 8, 7, 0) },
    { 950, EV_SWEEP_PA, 2, 0, 0 },
    { 0, EV_VIB, 2, 69, 3 },
    { 0, EV_OFF, 2 },

    // --- night falls -----------------------------------------------------
    { 600, EV_SWEEP_PA, 0, PA_B(false, 0, 0, 13) },  // pad fades out
    { 1200, EV_PRESET, 2, 6 },
    { 0, EV_VIB, 2, 76, 3 },                    // one last owl (E5)
    { 0, EV_SWEEP_PA, 2, PA_B(false, 0, 0, 11) },
    { 1500, EV_OFF, 2 },
    { 800, EV_END }
};

// --------------------------------------------------------------------------
// Demo 6: "Sunburst Run" - an original up-tempo piece, ~95 s, all four
// voices working the whole time:
//
//   ch0  bass     saw, plucked (fast amp sweep), root/fifth per bar
//   ch1  harmony  off-beat stabs; preset 'power' gives (3x,2x) frequency
//                 multipliers, so ONE note sounds a two-note chord
//   ch2  lead     the tune; per-note slope sweeps act as a filter envelope
//   ch3  drums    kick / snare / hat on the eighth grid
//
// 150 BPM: eighth = 200 ms, bar = 1.6 s.  Progression is D - A - Bm - G
// (I-V-vi-IV in D major).  Bars are written on an eighth grid by BARG();
// a melody slot of 0 is a rest that lets the previous note ring on.
// --------------------------------------------------------------------------
#define UB_E        200                 // eighth note
#define UB_BASS_AMP 44
#define UB_HARM_AMP 30
#define UB_LEAD_AMP 52

#define B_(n)  { 0, (n) ? EV_ON : EV_NOP, 0, (n), UB_BASS_AMP }
#define H_(n)  { 0, (n) ? EV_ON : EV_NOP, 1, (n), UB_HARM_AMP }
#define L_(n)  { 0, (n) ? EV_ON : EV_NOP, 2, (n), UB_LEAD_AMP }

// One bar: four drum slots (on-beats D1/D3, off-beats D2/D4), bass on
// beats 1 and 3, harmony stabs on every off-beat, melody on all eighths
#define BARG(D1,D2,D3,D4, b1,b2,h1,h2, m1,m2,m3,m4,m5,m6,m7,m8) \
    { UB_E, D1, 3 }, B_(b1),          L_(m1),                   \
    { UB_E, D2, 3 },          H_(h1), L_(m2),                   \
    { UB_E, D3, 3 },                  L_(m3),                   \
    { UB_E, D4, 3 },          H_(h1), L_(m4),                   \
    { UB_E, D1, 3 }, B_(b2),          L_(m5),                   \
    { UB_E, D2, 3 },          H_(h2), L_(m6),                   \
    { UB_E, D3, 3 },                  L_(m7),                   \
    { UB_E, D4, 3 },          H_(h2), L_(m8),

#define UBAR(...) BARG(EV_KICK, EV_HAT, EV_SNARE, EV_HAT, __VA_ARGS__)
#define HBAR(...) BARG(EV_NOP,  EV_HAT, EV_NOP,   EV_HAT, __VA_ARGS__)
#define QBAR(...) BARG(EV_NOP,  EV_NOP, EV_NOP,   EV_NOP, __VA_ARGS__)

// Chord shapes: bass roots / fifths, and the stab note (sounds an
// octave + a fifth higher through the (3x,2x) multipliers)
#define CH_D   38, 45, 50, 50           // D2 A2 | D4+A4
#define CH_A   45, 52, 57, 57           // A2 E3 | A4+E5
#define CH_Bm  47, 54, 59, 59           // B2 F#3 | B4+F#5
#define CH_G   43, 50, 55, 55           // G2 D3 | G4+D5

#define UB_VERSE                                                   \
    UBAR(CH_D,   0,74, 0,76, 78, 0,76, 0)                          \
    UBAR(CH_A,   0,73, 0,76,  0,73,69, 0)                          \
    UBAR(CH_Bm,  0,74, 0,78, 76, 0,74, 0)                          \
    UBAR(CH_G,   0,71,69,67,  0,69,71, 0)                          \
    UBAR(CH_D,   0,74, 0,76, 78, 0,81, 0)                          \
    UBAR(CH_A,   0,76, 0,73,  0,69,73, 0)                          \
    UBAR(CH_Bm,  0,78, 0,74, 71, 0,74, 0)                          \
    UBAR(CH_G,   0,79,78,76,  0,74, 0, 0)

#define UB_PRECHORUS                                               \
    UBAR(CH_Bm, 71, 0,74, 0,78, 0,74, 0)                           \
    UBAR(CH_G,  67, 0,71, 0,74, 0,79, 0)                           \
    UBAR(CH_D,  74, 0,78, 0,81, 0,78, 0)                           \
    UBAR(CH_A,  76, 0,81, 0,85, 0,83,81)

#define UB_CHORUS                                                  \
    UBAR(CH_D,  81, 0,78,81,  0,83,81, 0)                          \
    UBAR(CH_A,  76, 0,81, 0, 78, 0,76, 0)                          \
    UBAR(CH_Bm, 78, 0,83, 0, 81, 0,78, 0)                          \
    UBAR(CH_G,  79, 0,78,76,  0,74,76, 0)                          \
    UBAR(CH_D,  81, 0,78,81,  0,83,86, 0)                          \
    UBAR(CH_A,  85, 0,81, 0, 78, 0,76, 0)                          \
    UBAR(CH_Bm, 78, 0,83, 0, 86, 0,83, 0)                          \
    UBAR(CH_G,  79, 0,81, 0, 83, 0,86, 0)

// Voice setup used at the top and after the break
#define UB_VOICES                                                  \
    { 0, EV_PRESET, 0, 3 },                 /* bass: saw       */  \
    { 0, EV_PRESET, 1, 9 },                 /* stabs: (3x,2x)  */  \
    { 0, EV_PRESET, 2, 4 },                 /* lead: pulse 25% */  \
    { 0, EV_DETUNE, 0, (uint8_t)PWL_DETUNE_OFF },                  \
    { 0, EV_DETUNE, 1, 12 },                                       \
    { 0, EV_DETUNE, 2, 6 },                                        \
    { 0, EV_SWEEP_PA, 0, PA_B(false, 0, 0, 12) }, /* bass pluck */ \
    { 0, EV_SWEEP_PA, 1, PA_B(false, 0, 0, 11) }, /* short stab */ \
    { 0, EV_SWEEP_PA, 2, PA_B(false, 0, 0, 13) }, /* lead ring  */

static const seq_ev_t demo_sunburst[] = {
    UB_VOICES

    // --- intro: stabs alone, then bass, then hats, then a pickup -----
    QBAR(0, 0,  50, 50,  0, 0, 0, 0,  0, 0, 0, 0)
    QBAR(CH_D,           0, 0, 0, 0,  0, 0, 0, 0)
    HBAR(CH_G,           0, 0, 0, 0,  0, 0, 0, 0)
    HBAR(CH_A,           0, 0, 0, 0, 74, 0,76,78)

    // --- verse 1 ------------------------------------------------------
    UB_VERSE

    // --- pre-chorus: per-note filter opening on the lead --------------
    { 0, EV_SWEEP_WS, 2, WS_B(false, 0, false, 3, 11) },
    UB_PRECHORUS
    { 0, EV_SWEEP_WS, 2, 0, 0 },

    // --- chorus 1: brighter lead --------------------------------------
    { 0, EV_PRESET, 2, 3 },                 // saw lead
    UB_CHORUS

    // --- post-chorus lick: lead runs, band keeps the groove -----------
    { 0, EV_PRESET, 2, 2 },                 // square
    UBAR(CH_D,  86,85,83,81, 78,76,74,73)
    UBAR(CH_A,  69,73,76,81, 85,81,76,73)
    UBAR(CH_Bm, 71,74,78,83, 86,83,78,74)
    UBAR(CH_G,  79,78,76,74, 71,74,76,78)
    // second half of the lick: call and answer, ends on a high hold
    UBAR(CH_D,  81, 0,78, 0, 74, 0,78,81)
    UBAR(CH_A,  85, 0,81, 0, 76, 0,81,85)
    UBAR(CH_Bm, 83,81,78,76, 74,76,78,83)
    UBAR(CH_G,  86, 0, 0,83,  0,81, 0,79)

    // --- break: drums out, Orion pad swells, lead answers w/ vibrato --
    { 0, EV_OFF, 0 }, { 0, EV_OFF, 1 }, { 0, EV_OFF, 2 },
    { 400, EV_PRESET, 1, 12 },              // orion pad
    { 0, EV_DETUNE, 1, 24 },
    { 0, EV_SWEEP_PA, 1, PA_B(false, 0, 3, 13) },
    { 0, EV_ON, 1, 50, 0 },                 // D3 pad, swells in
    { 0, EV_PRESET, 2, 6 },                 // sine lead
    { 0, EV_SWEEP_PA, 2, PA_B(false, 0, 0, 13) },
    { 900, EV_VIB, 2, 74, 4 },
    { 500, EV_VIB, 2, 78, 3 },
    { 500, EV_VIB, 2, 81, 6 },
    { 700, EV_SWEEP_PA, 1, PA_B(false, 0, 0, 12) },
    { 900, EV_OFF, 1 }, { 0, EV_OFF, 2 },

    // --- verse 2: NES-flavoured lead, thinner stabs -------------------
    UB_VOICES
    { 0, EV_PRESET, 2, 1 },                 // 4-bit triangle lead
    { 0, EV_PRESET, 1, 5 },                 // 12.5% pulse stabs
    UB_VERSE

    // --- pre-chorus 2 --------------------------------------------------
    { 0, EV_PRESET, 1, 9 },
    { 0, EV_SWEEP_WS, 2, WS_B(false, 0, false, 3, 11) },
    UB_PRECHORUS
    { 0, EV_SWEEP_WS, 2, 0, 0 },

    // --- chorus 2 -------------------------------------------------------
    { 0, EV_PRESET, 2, 3 },
    UB_CHORUS

    // --- outro: two bars of the hook, then everything rings out --------
    UBAR(CH_D,  81, 0,78,81,  0,83,81, 0)
    UBAR(CH_G,  79, 0,78,76,  0,74,71, 0)
    { UB_E * 2, EV_PRESET, 2, 8 },          // organ octave stack
    { 0, EV_ON, 0, 38, 46 },                // D2
    { 0, EV_ON, 1, 50, 34 },                // D4 + A4
    { 0, EV_ON, 2, 74, 48 },                // D5 (+ D6 from the stack)
    { 0, EV_SWEEP_PA, 0, 0, 0 },            // let it ring...
    { 0, EV_SWEEP_PA, 1, 0, 0 },
    { 0, EV_SWEEP_PA, 2, 0, 0 },
    { 900, EV_SWEEP_PA, 0, PA_B(false, 0, 0, 14) },   // ...then fade
    { 0, EV_SWEEP_PA, 1, PA_B(false, 0, 0, 14) },
    { 0, EV_SWEEP_PA, 2, PA_B(false, 0, 0, 14) },
    { 3000, EV_END }
};

// --------------------------------------------------------------------------
// Demo 12: "Slipstream" - a ~47s groove showcasing the pitch and timbre
// envelopes on top of the ADSR engine:
//   intro     ch2 spins up two octaves into A4 (EV_PENV -24 @ r9) over an
//             organ pad whose PWM offset blooms open (EV_TPWM)
//   groove    saw bass with a slope-envelope pluck (EV_TSLOPE, slope_f
//             only), pulse stabs that wah open (EV_TPWM), lead scooping
//             -4 semis into every note (EV_PENV @ r6)
//   break     -12 scoop into a held E5, vibrato wail, then sine toms that
//             fall INTO pitch (EV_PENV +6 + one-shot hardware-decay ADSR)
//   section B lead notes bloom bright (EV_TSLOPE) and FALL away on every
//             phrase-ending note-off (EV_PENV release slide)
//   outro     final chord; the lead powers down as it fades
// 125 BPM: eighth = 240ms, bar = 1.92s.
// --------------------------------------------------------------------------
#define SL_E        240
#define SL_BASS_AMP 48
#define SL_STAB_AMP 42
#define SL_LEAD_AMP 52

// lead slot: 0 = hold, 1 = note-off (triggers the release fall), else note
#define SL_L(n) { 0, (n) == 0 ? EV_NOP : ((n) == 1 ? EV_OFF : EV_ON), 2, \
                  (uint8_t)((n) <= 1 ? 0 : (n)), SL_LEAD_AMP }
#define SL_B(n) { 0, (n) ? EV_ON : EV_NOP, 0, (n), SL_BASS_AMP }
#define SL_S(n) { 0, (n) ? EV_ON : EV_NOP, 1, (n), SL_STAB_AMP }

// slots:  1        2        3        4        5        6        7        8
// drums:  D1       D2       D3       D4       D1       D2       D3       D4
// bass:   b1       -        -        b2       -        b3       -        b4
// stabs:  -        -        s1       -        -        -        s2       -
#define SLBAR(D1,D2,D3,D4, b1,b2,b3,b4, s1,s2, m1,m2,m3,m4,m5,m6,m7,m8) \
    { SL_E, D1, 3 }, SL_B(b1), SL_L(m1),                                \
    { SL_E, D2, 3 },           SL_L(m2),                                \
    { SL_E, D3, 3 }, SL_S(s1), SL_L(m3),                                \
    { SL_E, D4, 3 }, SL_B(b2), SL_L(m4),                                \
    { SL_E, D1, 3 },           SL_L(m5),                                \
    { SL_E, D2, 3 }, SL_B(b3), SL_L(m6),                                \
    { SL_E, D3, 3 }, SL_S(s2), SL_L(m7),                                \
    { SL_E, D4, 3 }, SL_B(b4), SL_L(m8),

#define SLFULL(...) SLBAR(EV_KICK, EV_HAT, EV_SNARE, EV_HAT, __VA_ARGS__)
#define SLHATS(...) SLBAR(EV_NOP,  EV_HAT, EV_NOP,   EV_HAT, __VA_ARGS__)

// bass line + stab note per chord (A dorian vamp)
#define SL_AM 33, 33, 45, 31,  64, 64      // A1 A1 A2 G1 | E4 stabs
#define SL_D9 38, 38, 50, 36,  66, 66      // D2 D2 D3 C2 | F#4
#define SL_F  41, 41, 53, 36,  69, 69      // F2 F2 F3 C2 | A4
#define SL_G  43, 43, 55, 38,  71, 71      // G2 G2 G3 D2 | B4

static const seq_ev_t demo_slipstream[] = {
    // voices
    { 0, EV_PRESET, 0, 3 },                     // bass: saw
    { 0, EV_PRESET, 2, 2 },                     // lead: square
    { 0, EV_DETUNE, 0, (uint8_t)PWL_DETUNE_OFF },
    { 0, EV_DETUNE, 2, 10 },
    { 0, EV_ADSR, 0, 0x1B, 0x39 },              // bass  a1 d11 s3 r9
    { 0, EV_ADSR, 2, 0x5C, 0x5A },              // lead  a5 d12 s5 r10
    { 0, EV_TSLOPE, 0, 60, 0x28 },              // bass pluck: slope_f +60 @ r8

    // intro: two-octave spin-up over a blooming organ pad
    { 0, EV_PRESET, 1, 8 },                     // pad: organ (1x+2x)
    { 0, EV_DETUNE, 1, 12 },
    { 0, EV_ADSR, 1, 0xA0, 0x7B },              // pad swell a10 s7 r11
    { 0, EV_TPWM, 1, 55, 11 },                  // pwm blooms open (+110 @ r11)
    { 0, EV_PENV, 2, (uint8_t)-24, 9 },         // spin-up: 2 octaves @ r9
    { 0, EV_ON, 1, 57, 40 },                    // A3 pad (organ adds A4)
    { 0, EV_ON, 2, 69, 50 },                    // A4, arrives ~2.1s later
    { 1920, EV_HAT, 3 },                        // bar 2: hats wake up
    { 480, EV_HAT, 3 },
    { 480, EV_HAT, 3 },
    { 480, EV_HAT, 3 },
    // groove config: stabs take over ch1, lead gets its doink
    { 240, EV_OFF, 1 },
    { 0, EV_PRESET, 1, 4 },                     // stabs: pulse25
    { 0, EV_ADSR, 1, 0x5C, 0x0A },              // stab a5 d12 s0 r10
    { 0, EV_TPWM, 1, 50, 10 },                  // stab wah (+100 @ r10)
    { 0, EV_PENV, 2, (uint8_t)-4, 6 },          // lead doink: -4 semis @ r6

    // --- section A ---
    SLFULL(SL_AM,  0, 0,69, 0,  72,69, 0,67)
    SLFULL(SL_AM, 64, 0,67,69,   1, 0, 0, 0)
    SLFULL(SL_D9,  0, 0,69, 0,  71,69, 0,66)
    SLFULL(SL_D9, 62, 0,66,69,   1, 0, 0,72)
    SLFULL(SL_AM, 69, 0,72,74,   0,72,69, 0)
    SLFULL(SL_AM, 76, 0,74,72,   1, 0, 0,67)
    SLFULL(SL_F,  69, 0,72,69,   0,65,69, 0)
    SLFULL(SL_G,  71, 0,74,71,  67, 0,64, 1)

    // --- break: the big scoop, a wail, and falling toms ---
    { 0, EV_PENV, 2, (uint8_t)-12, 8 },         // octave scoop @ r8
    SLHATS(33, 0, 0, 0,  0, 0,  76, 0, 0, 0,  0, 0, 0, 0)
    { 240, EV_PENV, 2, 0, 0 },                  // vib must not re-scoop
    { 0, EV_VIB, 2, 76, 4 },                    // E5 wail (blocking ~0.4s)
    { 1200, EV_OFF, 2 },
    { 0, EV_PRESET, 3, 6 },                     // toms: sine
    { 0, EV_ADSR, 3, 0x0C, 0x00 },              // one-shot hw decay (a0 d12 s0)
    { 0, EV_PENV, 3, 6, 7 },                    // toms fall INTO pitch (+6 @ r7)
    { 240, EV_ON, 3, 57, 60 },                  // A3
    { 480, EV_ON, 3, 53, 60 },                  // F3
    { 480, EV_ON, 3, 50, 60 },                  // D3
    { 480, EV_ON, 3, 45, 63 },                  // A2
    { 480, EV_PENV, 3, 0, 0 },                  // hand ch3 back to the drums
    { 0, EV_ADSR, 3, 0xFF, 0 },
    // rebuild bar: bass walks up, lead learns to fall
    { 0, EV_PENV, 2, (uint8_t)-4, 0x86 },       // doink in, FALL out (rel r8)
    { 0, EV_TSLOPE, 2, 60, 0x39 },              // bright bloom on every note
    SLHATS(33, 36, 38, 40,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0)

    // --- section B: phrase-ending offs now fall away ---
    SLFULL(SL_AM,  0, 0,69, 0,  72,69, 0,67)
    SLFULL(SL_AM, 64, 0,67,69,   1, 0, 0, 0)
    SLFULL(SL_D9,  0, 0,74, 0,  76,74, 0,71)
    SLFULL(SL_D9, 69, 0,71,74,   1, 0, 0,72)
    SLFULL(SL_AM, 81, 0,79,77,   0,76,74, 0)
    SLFULL(SL_AM, 76, 0,72,69,   1, 0, 0,64)
    SLFULL(SL_F,  65, 0,69,72,   0,69,65, 0)
    SLFULL(SL_G,  67, 0,71,74,   1, 0,67, 1)

    // --- outro: last chord, lead powers down as it fades ---
    { 0, EV_PENV, 2, (uint8_t)-12, 0x88 },      // scoop in, fall away (r8)
    { 0, EV_ADSR, 2, 0x5D, 0x5C },              // slow release r12
    { 240, EV_KICK, 3 },
    { 0, EV_ON, 0, 33, 50 },                    // A1
    { 0, EV_ON, 1, 64, 45 },                    // E4 stab rings out
    { 0, EV_ON, 2, 69, 60 },                    // A4 (scoops up from A3)
    { 960, EV_HAT, 3 },
    { 720, EV_KICK, 3 },
    { 0, EV_OFF, 2 },                           // ...and falls away
    { 0, EV_OFF, 1 },
    { 240, EV_OFF, 0 },
    { 2400, EV_END }
};

// --------------------------------------------------------------------------
// Demo 14: "flute & bells" - a timbre proof for two asks:
//   Chinese pan flute  sine tone (soft attack, -2 semi scoop into every
//                      note, vibrato on the long ones) layered with the
//                      18-bit noise channel playing quiet breath chiff
//   Tubular Bells      three channels struck together: prime (detuned
//                      triangle), an INHARMONIC partial +17 semis up
//                      (2.67x ~= the tube's 2.76x mode), a hum -12, and
//                      a noise tick for the mallet; instant attacks
//                      with long HARDWARE decays (a=0 -> zero-CPU ring)
// --------------------------------------------------------------------------
#define FLUTE(dt, n, v)  { dt, EV_ON, 2, n, v }, { 0, EV_ON, 3, 108, 9 }
#define BELL(dt, n)      { dt, EV_ON, 0, (uint8_t)((n) - 12), 28 },     \
                         { 0, EV_ON, 1, n, 55 },                        \
                         { 0, EV_ON, 2, (uint8_t)((n) + 17), 32 },      \
                         { 0, EV_ON, 3, 105, 24 }

static const seq_ev_t demo_timbres[] = {
    // --- pan flute: sine + breath noise --------------------------------
    { 0, EV_PRESET, 2, 6 },                 // tone: sine
    { 0, EV_DETUNE, 2, 6 },
    { 0, EV_ADSR, 2, 0x9c, 0x6a },          // a9 d12 s6 r10 (soft breath)
    { 0, EV_PENV, 2, (uint8_t)-2, 7 },      // scoop into each note
    { 0, EV_PRESET, 3, 11 },                // breath: 18-bit noise
    { 0, EV_DETUNE, 3, (uint8_t)PWL_DETUNE_OFF },
    { 0, EV_ADSR, 3, 0x7b, 0x19 },          // chiff then low breath bed
    FLUTE(400, 74, 50),                     // D5   (D pentatonic phrase)
    FLUTE(700, 76, 46),                     // E5
    FLUTE(700, 79, 52),                     // G5
    { 700, EV_PENV, 2, 0, 0 },              // vib note: scoop off
    { 0, EV_ON, 3, 108, 9 },
    { 0, EV_VIB, 2, 81, 6 },                // A5 held with vibrato
    { 0, EV_PENV, 2, (uint8_t)-2, 7 },
    FLUTE(200, 83, 50),                     // B5
    FLUTE(600, 81, 46),                     // A5
    FLUTE(600, 79, 48),                     // G5
    FLUTE(700, 76, 42),                     // E5
    { 650, EV_PENV, 2, 0, 0 },
    { 0, EV_ON, 3, 108, 9 },
    { 0, EV_VIB, 2, 74, 8 },                // D5 long vibrato close
    { 400, EV_OFF, 2 },
    { 0, EV_OFF, 3 },

    // --- tubular bells --------------------------------------------------
    { 900, EV_PRESET, 0, 0 },               // hum/prime/partial: triangle
    { 0, EV_PRESET, 1, 0 },
    { 0, EV_PRESET, 2, 0 },
    { 0, EV_DETUNE, 0, (uint8_t)PWL_DETUNE_OFF },
    { 0, EV_DETUNE, 1, 10 },                // beating shimmer on the prime
    { 0, EV_DETUNE, 2, 8 },
    { 0, EV_PENV, 2, 0, 0 },
    { 0, EV_ADSR, 0, 0x0f, 0x0d },          // hum:    instant, ~4s decay
    { 0, EV_ADSR, 1, 0x0e, 0x0d },          // prime:  ~2s decay
    { 0, EV_ADSR, 2, 0x0d, 0x0c },          // partial: ~1s decay
    { 0, EV_ADSR, 3, 0x09, 0x00 },          // mallet tick: ~65ms
    { 0, EV_TSLOPE, 1, 40, 0x39 },          // strike bright, mellows fast
    BELL(200, 76),                          // E5
    BELL(1500, 71),                         // B4
    BELL(1500, 76),                         // E5
    BELL(1500, 79),                         // G5
    BELL(1500, 78),                         // F#5
    BELL(1500, 76),                         // E5 ... let it ring
    { 4500, EV_END }
};

// --------------------------------------------------------------------------
// Demo 15: "Adagio" - the violin instrument, everything the driver knows
// about shaping a note working at once:
//
//   ch2  solo violin  bow bite (timbre env opens bright and settles), a
//                     semitone finger-slide into every note (pitch env),
//                     a SUSTAINING amp envelope - attack then hold, no
//                     decay, because a bow keeps feeding the string -
//                     and 6Hz vibrato that blooms 250ms in
//   ch1  cello        the same instrument down an octave and a bit:
//                     slower bow, longer tail, shallower/later vibrato
//   ch0  section top  in the repeat, doubles the melody an octave up at
//                     a third of the level, detuned and vibrato'd
//                     differently - two players never match, and that
//                     mismatch is what reads as "section"
//
// Andante, quarter = 600ms, D minor.  Notes retrigger without a note-off:
// the envelope re-articulates from the current level, which is exactly
// what a bow change sounds like.
// --------------------------------------------------------------------------
#define P_VIOLIN 14                     // presets[] index (see seq.h)
#define VLN_Q    600                    // quarter note
#define VLN_H    1200
#define VLN_DH   1800

// EV_VIBRATO 'b' field: depth in cents (2-cent units) + delay code
#define VIB(cents, dly)  (uint8_t)(((((cents) / 2)) & 63) | ((dly) << 6))

#define VN(dt, n, v)  { dt, EV_ON, 2, n, v }            // solo
#define VC(dt, n, v)  { dt, EV_ON, 1, n, v }            // cello
#define V2(dt, n, v)  { dt, EV_ON, 2, n, v },           \
                      { 0, EV_ON, 0, (uint8_t)((n) + 12), (uint8_t)((v) / 3) }

static const seq_ev_t demo_adagio[] = {
    { 0, EV_PRESET,  2, P_VIOLIN },
    { 0, EV_DETUNE,  2, 6 },                // the two sub-oscillators beat
    { 0, EV_ADSR,    2, 0xa0, 0x7b },       // a10 d0 s7 r11
    { 0, EV_PENV,    2, (uint8_t)-1, 7 },   // ~22ms slide into pitch
    { 0, EV_TSLOPE,  2, 64, 0x39 },         // bow bite, gone in ~65ms
    { 0, EV_VIBRATO, 2, 60, VIB(26, 1) },   // 6.0Hz +-26c after 250ms
    { 0, EV_PRESET,  1, P_VIOLIN },
    { 0, EV_DETUNE,  1, 4 },
    { 0, EV_ADSR,    1, 0xb0, 0x7c },       // slower bow, longer release
    { 0, EV_PENV,    1, (uint8_t)-1, 8 },
    { 0, EV_VIBRATO, 1, 50, VIB(14, 2) },   // 5.0Hz +-14c after 500ms

    // --- phrase 1: solo over a cello pedal ------------------------------
    VC(500, 50, 38),                        // D3
    VN(100, 69, 44),                        // A4   pickup
    VN(VLN_Q, 74, 52),                      // D5   held - vibrato blooms
    VN(VLN_H, 72, 46),                      // C5
    VC(0, 46, 36),                          // Bb2
    VN(VLN_Q, 70, 48),                      // Bb4
    VN(VLN_H, 69, 44),                      // A4
    VC(0, 45, 36),                          // A2
    VN(VLN_H, 67, 42),                      // G4
    VN(VLN_Q, 69, 44),                      // A4
    VN(VLN_Q, 70, 46),                      // Bb4
    VC(0, 43, 36),                          // G2
    VN(VLN_H, 69, 42),                      // A4
    { VLN_DH, EV_OFF, 2 },
    { 0, EV_OFF, 1 },

    // --- phrase 2: the same line, doubled an octave up ------------------
    { 800, EV_PRESET, 0, P_VIOLIN },
    { 0, EV_DETUNE,  0, 10 },
    { 0, EV_ADSR,    0, 0xb0, 0x7b },
    { 0, EV_PENV,    0, (uint8_t)-1, 8 },
    { 0, EV_VIBRATO, 0, 64, VIB(30, 1) },   // wider and faster than the solo
    VC(0, 50, 38),                          // D3
    V2(200, 74, 48),                        // D5
    V2(VLN_Q, 77, 52),                      // F5   the climb
    VC(0, 46, 36),                          // Bb2
    V2(VLN_H, 76, 48),                      // E5
    V2(VLN_Q, 74, 50),                      // D5
    VC(0, 43, 36),                          // G2
    V2(VLN_H, 72, 46),                      // C5
    V2(VLN_H, 70, 46),                      // Bb4
    VC(0, 45, 36),                          // A2
    V2(VLN_Q, 69, 44),                      // A4
    V2(VLN_Q, 67, 42),                      // G4
    V2(VLN_H, 69, 46),                      // A4
    VC(0, 50, 38),                          // D3
    V2(VLN_H, 74, 50),                      // D5   let it ring
    { 2400, EV_OFF, 2 },
    { 0, EV_OFF, 0 },
    { 0, EV_OFF, 1 },
    { 1500, EV_END }
};

// ==========================================================================
// Register selftest: proves 16-bit access works at even AND odd
// addresses (phase/counter live in the odd bank)
// ==========================================================================
static int st_pass, st_fail;

static void st_check(bool ok, const char *what)
{
    if (ok)
        ++st_pass;
    else
        ++st_fail;
    printf("  %-34s %s\n", what, ok ? "PASS" : "FAIL");
}

static void run_selftest(void)
{
    st_pass = st_fail = 0;
    pwl_all_off();

    // even-bank registers, full width
    pwl_write(1, PWL_REG_PERIOD, 0x1234);
    st_check(pwl_read(1, PWL_REG_PERIOD) == 0x1234, "period write/readback (13-bit)");
    pwl_write(1, PWL_REG_AMP, 0x2A);
    st_check(pwl_read(1, PWL_REG_AMP) == 0x2A, "amp write/readback");
    pwl_write(1, PWL_REG_SLOPE_R, 0xA5);
    st_check(pwl_read(1, PWL_REG_SLOPE_R) == 0xA5, "slope_r write/readback");
    pwl_write(1, PWL_REG_PWM_OFFSET, 0x5A);
    st_check(pwl_read(1, PWL_REG_PWM_OFFSET) == 0x5A, "pwm_offset write/readback");

    // odd-bank: phase (12-bit, changes as the oscillator runs - write
    // then read twice; deltas prove the write landed near the value)
    pwl_write(1, PWL_REG_PERIOD, pwl_note_fperiod(21));    // slow osc
    pwl_write(1, PWL_REG_PHASE, 0x800);
    uint16_t ph = pwl_read(1, PWL_REG_PHASE);
    st_check(ph >= 0x7F0 && ph <= 0x810, "phase write/readback (odd addr)");

    // odd-bank: counter runs at 1MHz; write it and watch it move
    pwl_write_raw(PWL_REG_COUNTER_LO, 0);
    pwl_write_raw(PWL_REG_COUNTER_HI, 0);
    uint32_t c1 = pwl_read_counter();
    uint32_t c2 = pwl_read_counter();
    st_check(c2 > c1 && c2 < 100000, "counter runs (odd addr rw)");

    pwl_write(1, PWL_REG_PERIOD, 0);
    printf("%d/%d checks passed - %s\n", st_pass, st_pass + st_fail,
           st_fail ? "*** FAIL ***" : "*** PASS ***");
}

// ==========================================================================
// Commands
// ==========================================================================
static void cmd_help(void)
{
    printf(
"PWL synth CLI - notes are names (C4, A#3, Eb2) or MIDI numbers\n"
"  note <ch> <note> [amp]   play a note (amp 0-63, default 63)\n"
"  off [ch]                 note off (no arg: all channels)\n"
"  chord <n1> [n2 n3 n4]    play notes on channels 0..3\n"
"  wave <ch> <preset>       set waveform ('wave list' lists presets)\n"
"  detune <ch> <n|off>      auto-detune strength (semitone offset)\n"
"  adsr <ch> <a d s r|off>  amp envelope: attack/decay/release rates\n"
"                           0-15 (0=instant, 1=~2ms .. 15=~4s), sustain\n"
"                           0-7; velocity sets the peak; 'off' disables\n"
"  penv <ch> <semis> <rate> [relrate [up]]   pitch envelope: notes\n"
"                           slide in from +-semis away; optional slide\n"
"                           away on release ('penv <ch> off' disables)\n"
"  vib <ch> <rate> <depth> [delay]           vibrato LFO on a held\n"
"                           note: rate in 0.1Hz (60 = 6Hz), depth in\n"
"                           cents, delay ms before it fades in\n"
"                           ('vib <ch> off' disables)\n"
"  tenv <ch> pwm|slope <delta> <rate>        timbre envelope: notes\n"
"                           start delta away from the preset's\n"
"                           pwm_offset/slopes, sweep back ('tenv' = help)\n"
"  amp <ch> <0-63>          set amplitude directly\n"
"  fade <ch> <tgt> <rate>   amp sweep to target 0-7 (rate 1=fast..15)\n"
"  bend <ch> up|down <rate> period sweep (pitch bend)\n"
"  brighten <ch> <rate>     sweep slopes up   (lowpass opening)\n"
"  darken <ch> <rate>       sweep slopes down (lowpass closing)\n"
"  kick|snare|hat [ch]      percussion one-shots (default ch 3)\n"
"  stereo off|on|pos        mono / stereo voice / stereo position mode\n"
"  pos <ch> <0-7>           stereo position (needs 'stereo pos')\n"
"  phase <ch> <v>           set oscillator phase\n"
"  cnt                      read the 24-bit sample counter\n"
"  rd <off> | wr <off> <v>  raw 16-bit register access\n"
"  regs                     dump readable registers\n"
"  selftest                 register access self test\n"
"  fs [probe]               host filesystem status (tqv.py serves its\n"
"                           --fs-root over this console); probe re-checks\n"
"  ls [dir]                 list host files\n"
"  cat <file>               print a host file\n"
"  play <file.pwl> [q]      play a converted song off the host (the\n"
"                           TUI's MIDI tab 'save' writes these); 'q'\n"
"                           mutes the note display while it plays\n"
"  tui                      full-screen UI (curses over this UART; live\n"
"                           channel registers + per-channel note tracks,\n"
"                           'exit' returns here)\n"
"  demo <1-16> [q]          play a demo ('q' mutes the note display):\n"
"                           1=waveform tour 2=chord+power 3=groove\n"
"                           4=Fur Elise 5=jungle adventure\n"
"                           6=Sunburst Run (full 4-voice song)\n"
"                           7=Another Day in Paradise (vocal+ADSR)\n"
"                           8=  \" (lead-vocal mix, no envelopes)\n"
"                           9=Axel F (from MIDI) 10=  \" +ADSR\n"
"                           11=gmlast (satb piano split + ADSR)\n"
"                           12=Slipstream (pitch+timbre env showcase)\n"
"                           13=Always On My Mind (vocal scoops+ADSR)\n"
"                           14=flute & bells (pan flute / Tubular Bells)\n"
"                           15=Adagio (violin: bow, slide, vibrato)\n"
"                           16=Skybells (upbeat anthem, tubular bells)\n");
}

static void cmd_wave_list(void)
{
    for (int i = 0; i < NUM_PRESETS; ++i)
        printf("  %-8s %s\n", presets[i].name, presets[i].desc);
}

static void cmd_regs(void)
{
    printf("ch: period phase  amp  sl_r sl_f pwm_o\n");
    for (int ch = 0; ch < PWL_NUM_CHANNELS; ++ch)
        printf(" %d: 0x%04x 0x%03x  %2d   %3d  %3d  %3d\n", ch,
               pwl_read(ch, PWL_REG_PERIOD), pwl_read(ch, PWL_REG_PHASE),
               pwl_read(ch, PWL_REG_AMP), pwl_read(ch, PWL_REG_SLOPE_R),
               pwl_read(ch, PWL_REG_SLOPE_F),
               pwl_read(ch, PWL_REG_PWM_OFFSET));
    printf("counter = %lu\n", (unsigned long)pwl_read_counter());
}

static bool arg_ch(int argc, char *argv[], int idx, int *ch)
{
    uint32_t v;

    if (argc <= idx) {
        printf("missing channel\n");
        return false;
    }
    if (!parse_u32(argv[idx], &v) || v >= PWL_NUM_CHANNELS) {
        printf("channel must be 0-3\n");
        return false;
    }
    *ch = (int)v;
    return true;
}

void cli_execute(int argc, char *argv[])
{
    const char *cmd = argv[0];
    uint32_t v;
    int ch;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        cmd_help();
    else if (!strcmp(cmd, "note")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3)
            return;
        int midi = parse_note(argv[2]);
        if (midi < 0) {
            printf("bad note '%s'\n", argv[2]);
            return;
        }
        uint32_t amp = 63;
        if (argc > 3 && !parse_u32(argv[3], &amp))
            return;
        pwl_note_on(ch, midi, (uint8_t)amp);
        printf("ch%d %s (midi %d) f_period=0x%04x\n", ch, note_name(midi),
               midi, pwl_note_fperiod(midi));
        note_tab(ch, note_name(midi));
    } else if (!strcmp(cmd, "off")) {
        if (argc > 1) {
            if (arg_ch(argc, argv, 1, &ch))
                pwl_note_off(ch);
        } else
            pwl_all_off();
    } else if (!strcmp(cmd, "chord")) {
        for (int i = 1; i < argc && i <= 4; ++i) {
            int midi = parse_note(argv[i]);
            if (midi >= 0) {
                pwl_note_on(i - 1, midi, 55);
                note_out(i - 1, note_name(midi));
            }
        }
        printf("\n");
    } else if (!strcmp(cmd, "wave")) {
        if (argc > 1 && !strcmp(argv[1], "list")) {
            cmd_wave_list();
            return;
        }
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3)
            return;
        int p = find_preset(argv[2]);
        if (p < 0) {
            printf("unknown preset ('wave list' to list)\n");
            return;
        }
        apply_preset(ch, p);
        printf("ch%d = %s\n", ch, presets[p].name);
    } else if (!strcmp(cmd, "adsr")) {
        if (!arg_ch(argc, argv, 1, &ch))
            return;
        if (argc > 2 && !strcmp(argv[2], "off")) {
            pwl_adsr_off(ch);
            printf("ch%d envelope off\n", ch);
            return;
        }
        if (argc < 6) {
            printf("usage: adsr <ch> <a> <d> <s> <r>  (rates 0-15, 0=instant,"
                   " 1=~2ms..15=~4s; sustain 0-7)  or  adsr <ch> off\n");
            return;
        }
        uint32_t a, d, s, r;
        if (!parse_u32(argv[2], &a) || !parse_u32(argv[3], &d) ||
            !parse_u32(argv[4], &s) || !parse_u32(argv[5], &r))
            return;
        pwl_set_adsr(ch, (uint8_t)a, (uint8_t)d, (uint8_t)s, (uint8_t)r);
        printf("ch%d ADSR a=%lu d=%lu s=%lu r=%lu\n", ch,
               (unsigned long)a, (unsigned long)d,
               (unsigned long)s, (unsigned long)r);
    } else if (!strcmp(cmd, "penv")) {
        if (!arg_ch(argc, argv, 1, &ch))
            return;
        if (argc > 2 && !strcmp(argv[2], "off")) {
            pwl_pitch_env_off(ch);
            printf("ch%d pitch env off\n", ch);
            return;
        }
        if (argc < 4) {
            printf("usage: penv <ch> <semis> <rate> [relrate [up]]\n"
                   "  each note slides in from +-semis away (rate 1=~0.7ms\n"
                   "  per semi, 8=~44ms, 10=~175ms, 12=~0.7s); relrate slides\n"
                   "  the pitch away again on note-off (ADSR channels)\n"
                   "  or: penv <ch> off\n");
            return;
        }
        int semis = atoi(argv[2]);
        uint32_t rate, rel = 0;
        if (!parse_u32(argv[3], &rate))
            return;
        if (argc > 4 && !parse_u32(argv[4], &rel))
            return;
        bool up = argc > 5 && !strcmp(argv[5], "up");
        pwl_set_pitch_env(ch, (int8_t)semis, (uint8_t)rate, (uint8_t)rel, up);
        printf("ch%d pitch env: start %+d semis rate %lu, release %s\n",
               ch, semis, (unsigned long)rate,
               rel ? (up ? "rises" : "falls") : "off");
    } else if (!strcmp(cmd, "vib")) {
        if (!arg_ch(argc, argv, 1, &ch))
            return;
        if (argc > 2 && !strcmp(argv[2], "off")) {
            pwl_vibrato_off(ch);
            printf("ch%d vibrato off\n", ch);
            return;
        }
        if (argc < 4) {
            printf("usage: vib <ch> <rate> <depth> [delay_ms]\n"
                   "  rate  in 0.1Hz (60 = 6Hz; violin 55-70, 1.0-25.5Hz)\n"
                   "  depth peak cents (violin 15-40, 100 = a semitone)\n"
                   "  delay ms before it starts, and the fade-in after\n"
                   "        (default 250; the delay is what sells it)\n"
                   "  or: vib <ch> off\n");
            return;
        }
        uint32_t rate, depth, delay = 250;
        if (!parse_u32(argv[2], &rate) || !parse_u32(argv[3], &depth))
            return;
        if (argc > 4 && !parse_u32(argv[4], &delay))
            return;
        pwl_set_vibrato(ch, (uint8_t)rate, (uint8_t)depth, (uint16_t)delay);
        printf("ch%d vibrato %lu.%luHz +-%d cents, %lums delay\n", ch,
               (unsigned long)pwl_ch[ch].vib_rate / 10,
               (unsigned long)pwl_ch[ch].vib_rate % 10,
               pwl_ch[ch].vib_depth,
               (unsigned long)pwl_ch[ch].vib_delay_ms);
    } else if (!strcmp(cmd, "tenv")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3)
            return;
        pwl_channel_state_t *tch = &pwl_ch[ch];
        if (!strcmp(argv[2], "off")) {
            pwl_timbre_env_off(ch);
            printf("ch%d timbre env off\n", ch);
        } else if (!strcmp(argv[2], "pwm") && argc >= 5) {
            uint32_t rate;
            if (!parse_u32(argv[4], &rate))
                return;
            pwl_set_timbre_env(ch, (int16_t)atoi(argv[3]), (uint8_t)rate,
                               tch->tenv_slope_delta, tch->tenv_slope_rate,
                               tch->tenv_slope_dir);
            printf("ch%d pwm env: start %+d, sweep home at rate %lu\n",
                   ch, tch->tenv_pwm_delta, (unsigned long)rate);
        } else if (!strcmp(argv[2], "slope") && argc >= 5) {
            uint32_t rate;
            uint8_t dir = 3;
            if (!parse_u32(argv[4], &rate))
                return;
            if (argc > 5) {
                if (!strcmp(argv[5], "r")) dir = 1;
                else if (!strcmp(argv[5], "f")) dir = 2;
                else if (!strcmp(argv[5], "opp")) dir = 0;
            }
            pwl_set_timbre_env(ch, tch->tenv_pwm_delta, tch->tenv_pwm_rate,
                               (int8_t)atoi(argv[3]), (uint8_t)rate, dir);
            printf("ch%d slope env: start %+d (dir %d), rate %lu\n",
                   ch, tch->tenv_slope_delta, dir, (unsigned long)rate);
        } else {
            printf("usage: tenv <ch> pwm <delta> <rate>\n"
                   "       tenv <ch> slope <delta> <rate> [both|r|f|opp]\n"
                   "       tenv <ch> off\n"
                   "  notes start with pwm_offset/slopes shifted by delta\n"
                   "  and sweep back to the preset (rate 1=32us/step,\n"
                   "  8=0.5ms, 11=4ms, 13=16ms per step)\n");
        }
    } else if (!strcmp(cmd, "detune")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3)
            return;
        if (!strcmp(argv[2], "off"))
            pwl_ch[ch].relative_detune = PWL_DETUNE_OFF;
        else
            pwl_ch[ch].relative_detune = (int8_t)atoi(argv[2]);
        printf("ok (takes effect at next note)\n");
    } else if (!strcmp(cmd, "amp")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3 || !parse_u32(argv[2], &v))
            return;
        pwl_write(ch, PWL_REG_AMP, v & 63);
    } else if (!strcmp(cmd, "fade")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 4)
            return;
        uint32_t tgt, rate;
        if (!parse_u32(argv[2], &tgt) || !parse_u32(argv[3], &rate))
            return;
        pwl_write(ch, PWL_REG_SWEEP_PA,
                  pwl_sweep_pa(false, 0, (uint8_t)tgt, (uint8_t)rate));
    } else if (!strcmp(cmd, "bend")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 4 || !parse_u32(argv[3], &v))
            return;
        bool down = !strcmp(argv[2], "down");
        // period down = pitch up
        pwl_write(ch, PWL_REG_SWEEP_PA,
                  pwl_sweep_pa(!down, (uint8_t)v, 7, 0));
    } else if (!strcmp(cmd, "brighten") || !strcmp(cmd, "darken")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3 || !parse_u32(argv[2], &v))
            return;
        pwl_write(ch, PWL_REG_SWEEP_WS,
                  pwl_sweep_ws(false, 0, cmd[0] == 'd', 3, (uint8_t)v));
    } else if (!strcmp(cmd, "kick") || !strcmp(cmd, "snare") || !strcmp(cmd, "hat")) {
        ch = 3;
        if (argc > 1 && !arg_ch(argc, argv, 1, &ch))
            return;
        if (cmd[0] == 'k') perc_kick(ch);
        else if (cmd[0] == 's') perc_snare(ch);
        else perc_hat(ch);
    } else if (!strcmp(cmd, "stereo")) {
        if (argc < 2)
            printf("cfg = 0x%x\n", pwl_get_cfg());
        else if (!strcmp(argv[1], "off"))
            pwl_set_cfg(0);
        else if (!strcmp(argv[1], "on"))
            pwl_set_cfg(PWL_CFG_STEREO_EN);
        else if (!strcmp(argv[1], "pos"))
            pwl_set_cfg(PWL_CFG_STEREO_EN | PWL_CFG_STEREO_POS_EN);
    } else if (!strcmp(cmd, "pos")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3 || !parse_u32(argv[2], &v))
            return;
        uint16_t mode = (uint16_t)((pwl_ch[ch].mode & ~PWL_MODE_STEREO_POS(7)) |
                                   PWL_MODE_STEREO_POS(v));
        pwl_ch[ch].mode = mode;
        pwl_write(ch, PWL_REG_MODE, mode);
    } else if (!strcmp(cmd, "phase")) {
        if (!arg_ch(argc, argv, 1, &ch) || argc < 3 || !parse_u32(argv[2], &v))
            return;
        pwl_write(ch, PWL_REG_PHASE, (uint16_t)v);
    } else if (!strcmp(cmd, "cnt"))
        printf("counter = %lu (1MHz sample ticks)\n",
               (unsigned long)pwl_read_counter());
    else if (!strcmp(cmd, "rd")) {
        if (argc < 2 || !parse_u32(argv[1], &v))
            return;
        printf("[0x%02lx] = 0x%04x\n", (unsigned long)(v & 63),
               pwl_read_raw(v & 63));
    } else if (!strcmp(cmd, "wr")) {
        uint32_t val;
        if (argc < 3 || !parse_u32(argv[1], &v) || !parse_u32(argv[2], &val))
            return;
        pwl_write_raw(v & 63, (uint16_t)val);
    } else if (!strcmp(cmd, "envlog")) {
        for (int i = 0; i < pwl_envlog_n; ++i)
            printf("  %8lu us  tag%d val=%u\n",
                   (unsigned long)pwl_envlog[i].t, pwl_envlog[i].tag,
                   pwl_envlog[i].val);
        pwl_envlog_n = 0;
    } else if (!strcmp(cmd, "regs"))
        cmd_regs();
    else if (!strcmp(cmd, "selftest"))
        run_selftest();
    else if (!strcmp(cmd, "fs")) {
        // Status of the filesystem tqv.py's console serves.  Nothing
        // file related works from a plain terminal (the web console):
        // the probe settles that once and the rest fail fast.
        if (argc > 1 && !strcmp(argv[1], "probe"))
            tqv_fs_reprobe();
        if (tqv_fs_available())
            printf("host fs: %s\n", tqv_fs_host());
        else
            printf("host fs: none (run the console from tqv.py, then"
                   " 'fs probe')\n");
    } else if (!strcmp(cmd, "play")) {
        // Play a .pwl table (the TUI's 'save' output) off the host:
        // {'P','W','L','1', u32 count} + count seq_ev_t records
        FILE *f;
        seq_ev_t *seq;
        uint32_t hdr[2], count;
        long got;

        if (argc < 2) {
            printf("usage: play <file.pwl>\n");
            return;
        }
        if ((f = fopen(argv[1], "r")) == NULL) {
            printf("play: cannot open %s\n", argv[1]);
            return;
        }
        if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "PWL1", 4) != 0) {
            printf("play: %s is not a .pwl table\n", argv[1]);
            fclose(f);
            return;
        }
        count = hdr[1];
        if (count == 0 || count > 65536 ||
            (seq = malloc(count * sizeof(seq_ev_t))) == NULL) {
            printf("play: bad event count %lu\n", (unsigned long)count);
            fclose(f);
            return;
        }
        printf("loading %lu events...\n", (unsigned long)count);
        got = (long)fread(seq, sizeof(seq_ev_t), count, f);
        fclose(f);
        if (got != (long)count) {
            printf("play: short read (%ld of %lu events)\n", got,
                   (unsigned long)count);
            free(seq);
            return;
        }
        seq[count - 1].cmd = EV_END;    // whatever the file said, we end
        seq_quiet = (argc > 2 && seq_quiet_arg(argv[2]));
        seq_play(seq, argv[1]);
        seq_quiet = 0;
        free(seq);
    } else if (!strcmp(cmd, "ls")) {
        static char list[512];
        int n = tqv_fs_list(argc > 1 ? argv[1] : ".", list, sizeof(list) - 1);

        if (n < 0) {
            printf("ls: no host filesystem\n");
            return;
        }
        list[n] = '\0';
        fputs(list, stdout);
    } else if (!strcmp(cmd, "cat")) {
        // Through stdio on purpose: proves fopen/fgets reach the host
        FILE *f;
        char line[128];

        if (argc < 2) {
            printf("usage: cat <file>\n");
            return;
        }
        if ((f = fopen(argv[1], "r")) == NULL) {
            printf("cat: cannot open %s\n", argv[1]);
            return;
        }
        while (fgets(line, sizeof(line), f) != NULL)
            fputs(line, stdout);
        fclose(f);
    }
    else if (!strcmp(cmd, "tui"))
        tui_run();
    else if (!strcmp(cmd, "tuitest"))
        tui_smoke_test();
    else if (!strcmp(cmd, "demo")) {
        v = 1;
        if (argc > 1 && !parse_u32(argv[1], &v))
            return;
        seq_quiet = (argc > 2 && seq_quiet_arg(argv[2]));
        if (v == 1)
            demo_tour();
        else if (v == 2)
            seq_play(demo_chord, "chord fade + power sting");
        else if (v == 3)
            seq_play(demo_groove, "four-channel groove");
        else if (v == 4)
            seq_play(demo_elise, "Fur Elise (Beethoven)");
        else if (v == 5)
            seq_play(demo_jungle, "jungle adventure");
        else if (v == 6)
            seq_play(demo_sunburst, "Sunburst Run (4-voice, ~95s)");
        else if (v == 7)
            seq_play(song_paradise,
                     "Another Day in Paradise (vocal mix + ADSR, ~4:50)");
        else if (v == 8)
            seq_play(song_paradise_v,
                     "Another Day in Paradise (lead vocal mix, ~4:50)");
        else if (v == 9)
            seq_play(song_axelf, "Axel F (MIDI reduction, ~3:00)");
        else if (v == 10)
            seq_play(song_axelf_a, "Axel F (ADSR envelopes, ~3:00)");
        else if (v == 11)
            seq_play(song_gmlast,
                     "gmlast (4-voice satb split + ADSR, ~2:50)");
        else if (v == 12)
            seq_play(demo_slipstream,
                     "Slipstream (pitch+timbre envelopes, ~47s)");
        else if (v == 13)
            seq_play(song_willie,
                     "Always On My Mind (vocal scoops + ADSR, ~3:35)");
        else if (v == 14)
            seq_play(demo_timbres, "flute & bells (timbre proof, ~22s)");
        else if (v == 15)
            seq_play(demo_adagio,
                     "Adagio (violin: bow, slide, vibrato, ~23s)");
        else if (v == 16)
            seq_play(song_skybells,
                     "Skybells (all-features anthem + tubular bells, ~46s)");
        else
            printf("demos: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
        seq_quiet = 0;
    } else
        printf("unknown command (try 'help')\n");
}

// ==========================================================================
// Main
// ==========================================================================
int main(void)
{
    // Unbuffered stdout: the prompt and the character echo have to reach
    // the terminal as they are written.  (newlib line-buffers as soon as
    // it can malloc a buffer, which it now can - the local memmap gives
    // this build a real heap for the TUI.)
    setvbuf(stdout, NULL, _IONBF, 0);

    // Route uo_out[7:6] to the synth: per-pin function select = 33
    // (uo_out[7] = right / mono audio PMOD, uo_out[6] = left).
    // Keep 0 (UART TX) on uo_out[0] and debug enabled elsewhere.
    set_debug_sel(0xFF);
    set_gpio_func(7, PWL_SYNTH_PERIPHERAL_NUM);
    set_gpio_func(6, PWL_SYNTH_PERIPHERAL_NUM);

    printf("\n\x1b[93mPWL synth CLI (PiecewiseOrionSynth, peripheral %d at %08lx)\x1b[0m\n",
           PWL_SYNTH_PERIPHERAL_NUM, (unsigned long)PWL_SYNTH_BASE_ADDRESS);
    printf("counter = %lu\n", (unsigned long)pwl_read_counter());

    // Settle the host filesystem before the first prompt.  The window
    // covers tqv.py still finishing its start-up: it only serves
    // requests once its console is up, which can be after we boot.
    if (tqv_fs_probe_wait(2000))
        printf("host fs: %s\n", tqv_fs_host());
    else
        printf("host fs: none (plain terminal; 'fs probe' re-checks)\n");

    printf("type 'help' for commands\n");

    for (int ch = 0; ch < PWL_NUM_CHANNELS; ++ch) {
        pwl_ch[ch].relative_detune = 0;     // auto-detune on by default
        apply_preset(ch, 0);                // triangle
    }

    for (;;) {
        printf("pwl> ");
        char *line = cli_readline();
        char *argv[8];
        int argc = cli_split(line, argv, 8);
        if (argc)
            cli_execute(argc, argv);
    }
}
