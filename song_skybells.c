// pwl-tui demo 16: "Skybells" - an upbeat 46-second anthem written to
// exercise EVERYTHING the driver knows at once, instrument changes
// mid-channel included:
//
//   ch0  lead    A section: pulse25 pluck (detune shimmer, snappy ADSR)
//                B section: SWITCHES TO VIOLIN - bowed sustain envelope,
//                a -2 semi scoop into every note (pitch env), bow bite
//                (timbre slope env) and 6Hz vibrato that blooms 250ms in
//                finale: back to the pluck for the hook reprise
//   ch1  bass    saw, tight punchy ADSR, driving eighths (root/octave/
//                fifth bounce) - and tubular-bell PRIME in the break
//   ch2  arp     pulse12 offbeat arpeggio with a PWM-offset timbre pop
//                on every pluck; twice it SWITCHES to a solo orion bell
//                for accent strikes, and in the break it rings the
//                bell's inharmonic +17-semi partial
//   ch3  drums   kick/snare/hat percussion recipes (each hit reprograms
//                the channel: pitch-sweep kick, dual-band snare), and
//                the bell's noise mallet tick in the break
//
// Form (133 BPM, eighth = 225ms, bar = 1.8s, Am F C G):
//   intro 2 bars -> A hook 8 -> bell tease 2 -> B anthem 8 (violin)
//   -> tubular-bells breakdown 2 (the feature strikes: A4 then E5,
//   hum + prime + partial + mallet across all four channels)
//   -> reprise 2 -> final layered bell over the last chord, ring out.

#include <stdint.h>
#include "seq.h"

#define Q8 225                          // eighth note at 133 BPM

// Lead slot: 0 = rest, 1 = gate off, else midi note (ch0)
#define L_(n) { 0, (n) == 0 ? EV_NOP : (n) == 1 ? EV_OFF : EV_ON, 0, \
                (uint8_t)((n) < 2 ? 0 : (n)), 55 }
// Offbeat arp pluck: 0 = rest (ch2)
#define A_(n) { 0, (n) == 0 ? EV_NOP : EV_ON, 2, (uint8_t)(n), 36 }
// Bass eighth (ch1)
#define B_(n, v) { 0, EV_ON, 1, (uint8_t)(n), v }

// One groove bar: backbeat (kick 1&3, snare 2&4, hats on the ands),
// bass eighths bouncing root/octave/fifth, arp tones a1..a4 on the
// offbeats, lead melody m1..m8 on the eighth grid.  Every slot leads
// with Q8, so a bar is exactly 8 slots = 1.8s and bars just stack.
#define SKBAR(...) SKBAR_(__VA_ARGS__)     // expand ARP_x chord packs
#define SKBAR_(b, a1, a2, a3, a4, m1, m2, m3, m4, m5, m6, m7, m8)   \
  { Q8, EV_KICK,  3 }, B_(b, 52),          L_(m1),                  \
  { Q8, EV_HAT,   3 }, B_(b, 38), A_(a1),  L_(m2),                  \
  { Q8, EV_SNARE, 3 }, B_((b)+12, 44),     L_(m3),                  \
  { Q8, EV_HAT,   3 }, B_(b, 38), A_(a2),  L_(m4),                  \
  { Q8, EV_KICK,  3 }, B_(b, 50),          L_(m5),                  \
  { Q8, EV_HAT,   3 }, B_((b)+7, 40), A_(a3), L_(m6),               \
  { Q8, EV_SNARE, 3 }, B_((b)+12, 44),     L_(m7),                  \
  { Q8, EV_HAT,   3 }, B_(b, 38), A_(a4),  L_(m8),

// Arp tone sets, one octave of each chord
#define ARP_AM 57, 60, 64, 69
#define ARP_F  53, 57, 60, 65
#define ARP_C  60, 64, 67, 72
#define ARP_G  55, 59, 62, 67

// EV_VIBRATO depth byte: cents (2-cent units) | onset delay code << 6
#define VIB(cents, dly) (uint8_t)((((cents) / 2) & 63) | ((dly) << 6))

// The layered tubular bell (demo 14's recipe): hum an octave down on
// ch0, detuned prime on ch1, the inharmonic +17 semi partial (~the
// tube's 2.76x mode) on ch2, noise mallet tick on ch3
#define BELLHIT(dt, n, v)                                           \
  { dt, EV_ON, 0, (uint8_t)((n) - 12), (uint8_t)((v) / 2) },        \
  { 0,  EV_ON, 1, (uint8_t)(n), v },                                \
  { 0,  EV_ON, 2, (uint8_t)((n) + 17), (uint8_t)((v) * 3 / 5) },    \
  { 0,  EV_ON, 3, 105, 24 }

const seq_ev_t song_skybells[] = {
    // ---- voicing: pluck lead / saw bass / sparkle arp ------------------
    { 0, EV_PRESET, 0, 4 },             // lead: pulse25
    { 0, EV_DETUNE, 0, 6 },             //   shimmer
    { 0, EV_ADSR,   0, 0x0A, 0x48 },    //   instant attack, snappy pluck
    { 0, EV_PRESET, 1, 3 },             // bass: saw
    { 0, EV_DETUNE, 1, 0x80 },          //   tight (detune off)
    { 0, EV_ADSR,   1, 0x09, 0x36 },    //   punchy
    { 0, EV_PRESET, 2, 5 },             // arp: pulse12
    { 0, EV_DETUNE, 2, 0x80 },
    { 0, EV_ADSR,   2, 0x0B, 0x09 },    //   short pluck
    { 0, EV_TPWM,   2, 40, 11 },        //   every pluck pops bright (+80
                                        //     pwm offset, sweeps home)

    // ---- intro: pulse builds, drums thin -------------------------------
    { Q8, EV_KICK, 3 },  B_(45, 48),
    { Q8, EV_HAT,  3 },  B_(45, 34),
    { Q8, EV_HAT,  3 },  B_(57, 40),
    { Q8, EV_HAT,  3 },  B_(45, 34),
    { Q8, EV_KICK, 3 },  B_(45, 46),
    { Q8, EV_HAT,  3 },  B_(52, 36),
    { Q8, EV_HAT,  3 },  B_(57, 40),
    { Q8, EV_HAT,  3 },  B_(45, 34),
    { Q8, EV_KICK, 3 },  B_(45, 50),  A_(57),
    { Q8, EV_HAT,  3 },  B_(45, 36),  A_(60),
    { Q8, EV_HAT,  3 },  B_(57, 42),  A_(64),
    { Q8, EV_HAT,  3 },  B_(45, 36),  A_(69),
    { Q8, EV_KICK, 3 },  B_(45, 48),  A_(72),
    { Q8, EV_HAT,  3 },  B_(52, 38),  A_(69),
    { Q8, EV_SNARE, 3 }, B_(57, 42),           // roll into the hook
    { Q8, EV_SNARE, 3 }, B_(45, 40),

    // ---- A section: the hook, twice ------------------------------------
    SKBAR(45, ARP_AM, 69, 0, 76, 74, 72, 0, 74, 0)
    SKBAR(41, ARP_F,  72, 0, 77, 76, 77, 0, 81, 0)
    SKBAR(36, ARP_C,  79, 0, 76, 0, 72, 74, 76, 0)
    SKBAR(43, ARP_G,  74, 0, 71, 72, 74, 0, 67, 0)
    SKBAR(45, ARP_AM, 69, 0, 76, 74, 72, 0, 74, 0)
    SKBAR(41, ARP_F,  72, 0, 77, 76, 77, 0, 81, 0)
    SKBAR(36, ARP_C,  79, 0, 76, 77, 79, 0, 84, 0)
    SKBAR(43, ARP_G,  83, 81, 79, 77, 74, 76, 79, 1)

    // ---- bell tease: the arp channel BECOMES a bell for two strikes ----
    { Q8, EV_PRESET, 2, 12 },           // orion: rich clangy partials
    { 0, EV_DETUNE, 2, 9 },             //   beating shimmer
    { 0, EV_ADSR,   2, 0x0E, 0x0C },    //   instant strike, ~2s ring
    { 0, EV_TSLOPE, 2, 40, 0x39 },      //   strikes bright, mellows
    { 0, EV_KICK, 3 },  B_(45, 50), { 0, EV_ON, 2, 88, 46 },   // E6 bell
    { Q8, EV_HAT,  3 }, B_(45, 36),
    { Q8, EV_HAT,  3 }, B_(57, 42),
    { Q8, EV_HAT,  3 }, B_(45, 36),
    { Q8, EV_KICK, 3 }, B_(45, 48),
    { Q8, EV_HAT,  3 }, B_(52, 38),
    { Q8, EV_HAT,  3 }, B_(57, 42),
    { Q8, EV_HAT,  3 }, B_(45, 36),
    { Q8, EV_KICK, 3 }, B_(45, 50), { 0, EV_ON, 2, 86, 46 },   // D6 bell
    { Q8, EV_HAT,  3 }, B_(45, 36),
    { Q8, EV_HAT,  3 }, B_(57, 42),
    { Q8, EV_HAT,  3 }, B_(45, 36),
    { Q8, EV_KICK, 3 }, B_(45, 48),
    { Q8, EV_HAT,  3 }, B_(52, 38),
    { Q8, EV_SNARE, 3 }, B_(57, 44),
    { Q8, EV_SNARE, 3 }, B_(45, 42),
    // (still on bar 12's last eighth) re-arm the sparkle arp, and turn
    // the lead into a VIOLIN for the anthem: bowed sustain, scoop-in,
    // bow-bite timbre attack, blooming vibrato
    { 0, EV_PRESET, 2, 5 },
    { 0, EV_DETUNE, 2, 0x80 },
    { 0, EV_ADSR,   2, 0x0B, 0x09 },
    { 0, EV_TSLOPE, 2, 0, 0 },
    { 0, EV_TPWM,   2, 40, 11 },
    { 0, EV_PRESET, 0, 14 },            // violin (PWL osc soft saw)
    { 0, EV_DETUNE, 0, 4 },
    { 0, EV_ADSR,   0, 0x90, 0x7B },    // bow: a9, no decay, s7 r11
    { 0, EV_PENV,   0, (uint8_t)-2, 7 },// finger-slide into each note
    { 0, EV_TSLOPE, 0, 40, 0x39 },      // bow bite: bright, settles
    { 0, EV_VIBRATO, 0, 60, VIB(28, 1) },   // 6Hz, 28 cents, 250ms in

    // ---- B section: the anthem (long bowed notes over the groove) ------
    SKBAR(45, ARP_AM, 81, 0, 0, 0, 0, 0, 0, 0)
    SKBAR(41, ARP_F,  77, 0, 0, 0, 79, 0, 81, 0)
    SKBAR(36, ARP_C,  79, 0, 0, 0, 0, 0, 0, 0)
    SKBAR(43, ARP_G,  83, 0, 81, 0, 79, 0, 74, 0)
    SKBAR(45, ARP_AM, 76, 0, 81, 0, 84, 0, 0, 0)
    SKBAR(41, ARP_F,  84, 0, 83, 0, 81, 0, 77, 0)
    SKBAR(36, ARP_C,  79, 0, 0, 0, 0, 0, 84, 0)
    SKBAR(43, ARP_G,  83, 0, 0, 0, 81, 0, 0, 1)

    // ---- breakdown: THE TUBULAR BELLS ----------------------------------
    // groove stops dead; all four channels become one bell
    { Q8, EV_OFF, 0 }, { 0, EV_OFF, 1 }, { 0, EV_OFF, 2 },
    { 0, EV_VIBRATO, 0, 0, 0 },         // violin trappings off
    { 0, EV_PENV,   0, 0, 0 },
    { 0, EV_TSLOPE, 0, 0, 0 },
    { 0, EV_PRESET, 0, 0 },             // hum: pure triangle
    { 0, EV_DETUNE, 0, 0x80 },
    { 0, EV_ADSR,   0, 0x0F, 0x0D },    //   instant, ~4s hardware ring
    { 0, EV_PRESET, 1, 0 },             // prime
    { 0, EV_DETUNE, 1, 10 },            //   beating shimmer
    { 0, EV_ADSR,   1, 0x0E, 0x0D },
    { 0, EV_TSLOPE, 1, 40, 0x39 },      //   strike bright, mellow fast
    { 0, EV_PRESET, 2, 0 },             // inharmonic partial
    { 0, EV_DETUNE, 2, 8 },
    { 0, EV_ADSR,   2, 0x0D, 0x0C },
    { 0, EV_PRESET, 3, 11 },            // mallet: noise tick
    { 0, EV_DETUNE, 3, 0x80 },
    { 0, EV_SWEEP_PA, 3, 0, 0 },        //   the last hat cached an amp
    { 0, EV_SWEEP_WS, 3, 0, 0 },        //   sweep - a tick must not sweep
    { 0, EV_ADSR,   3, 0x09, 0x00 },
    BELLHIT(0, 69, 58),                 // A4 ... let it bloom
    BELLHIT(1800, 76, 58),              // E5 answers, rings across the bar

    // ---- reprise: groove slams back, pluck hook one more time ----------
    { 1575, EV_PRESET, 0, 4 },          // lead back to the pluck
    { 0, EV_DETUNE, 0, 6 },
    { 0, EV_ADSR,   0, 0x0A, 0x48 },
    { 0, EV_PENV,   0, 0, 0 },
    { 0, EV_PRESET, 1, 3 },             // bass back to saw
    { 0, EV_DETUNE, 1, 0x80 },
    { 0, EV_ADSR,   1, 0x09, 0x36 },
    { 0, EV_TSLOPE, 1, 0, 0 },
    { 0, EV_PRESET, 2, 5 },             // arp sparkle again
    { 0, EV_DETUNE, 2, 0x80 },
    { 0, EV_ADSR,   2, 0x0B, 0x09 },
    { 0, EV_TPWM,   2, 40, 11 },
    SKBAR(45, ARP_AM, 69, 0, 76, 74, 72, 0, 74, 0)
    SKBAR(43, ARP_G,  74, 0, 76, 0, 79, 0, 81, 0)

    // ---- finale: one last chord under one last layered bell ------------
    { Q8, EV_OFF, 0 }, { 0, EV_OFF, 2 },
    { 0, EV_PRESET, 0, 0 },             // bell voicing returns
    { 0, EV_DETUNE, 0, 0x80 },
    { 0, EV_ADSR,   0, 0x0F, 0x0D },
    { 0, EV_PRESET, 2, 0 },
    { 0, EV_DETUNE, 2, 8 },
    { 0, EV_ADSR,   2, 0x0D, 0x0C },
    { 0, EV_PRESET, 3, 11 },
    { 0, EV_SWEEP_PA, 3, 0, 0 },        // hat sweep must not eat the tick
    { 0, EV_SWEEP_WS, 3, 0, 0 },
    { 0, EV_ADSR,   3, 0x09, 0x00 },
    { 0, EV_ADSR,   1, 0x0E, 0x0D },    // bass holds the low A as the
    { 0, EV_DETUNE, 1, 10 },            //   bell prime rings over it
    { 0, EV_TSLOPE, 1, 40, 0x39 },
    BELLHIT(120, 69, 60),               // A4, the name of the song
    { 4200, EV_END }                    // ...ring all the way out
};
