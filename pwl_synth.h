#pragma once

// PiecewiseOrionSynth (pwl_synth) peripheral driver for TinyQV.
//
// Peripheral 33 on the ttsky25a-tinyQV design: a 4-channel piecewise
// linear synth by Toivo Henningsson, with 8 octaves (~15Hz..3.9kHz at
// 64MHz), 1MHz sample rate, PWM output on uo_out[7] (left; uo_out[6]
// is right in stereo mode - every other pin alternates l/r).
//
// Each channel plays two detunable sub-channels of one waveform:
//   phase -> [detune] -> triangle -> [+pwm_offset] -> slope_r/slope_f
//         -> amp clamp -> output accumulator
// Waveforms: linear osc, noise (LFSR), PWL osc, Orion Wave.
// Hardware sweeps (period / amp-to-target / slopes / pwm_offset) run
// envelopes without CPU help.
//
// Register access notes (see docs/user_peripherals/33_pwl_synth.md):
//  - Registers are 6..13 bits; use 16-bit accesses.
//  - phase, counter and cfg live at ODD byte addresses: the peripheral
//    uses address bit 0 as a register-bank select while the access size
//    stays 16-bit.  TinyQV issues these unaligned halfword accesses
//    directly (verified by the register selftest in pwl-test).
//  - Reads synchronize to the synth's 64-cycle sample loop and may
//    stall the bus for up to 64 clocks.
//  - All registers reset to 0 (amp=0 means silent; oscillators always run).

#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"

#define PWL_SYNTH_PERIPHERAL_NUM 33
#define PWL_SYNTH_BASE_ADDRESS  ((uintptr_t)PERI_BASE_ADDRESS(PWL_SYNTH_PERIPHERAL_NUM))

#define PWL_NUM_CHANNELS        4

// --------------------------------------------------------------------------
// Register map (byte offsets; channel registers repeat every 16 bytes)
// --------------------------------------------------------------------------
#define PWL_REG_PERIOD          0x00    // RW 13-bit {period_exp[2:0], mantissa[9:0]}
#define PWL_REG_PHASE           0x01    // RW 12-bit oscillator phase (odd address!)
#define PWL_REG_AMP             0x02    // RW  6-bit amplitude clamp (63 = full)
#define PWL_REG_SLOPE_R         0x04    // RW  8-bit rising slope  (16 = 2x)
#define PWL_REG_SLOPE_F         0x06    // RW  8-bit falling slope
#define PWL_REG_PWM_OFFSET      0x08    // RW  8-bit triangle DC offset (duty)
#define PWL_REG_MODE            0x0A    // W  12-bit mode (see PWL_MODE_*)
#define PWL_REG_SWEEP_PA        0x0C    // W  13-bit period/amp sweep
#define PWL_REG_SWEEP_WS        0x0E    // W  13-bit pwm_offset/slope sweep

#define PWL_REG_COUNTER_LO      0x03    // RW 12-bit sample counter [11:0]
#define PWL_REG_COUNTER_HI      0x13    // RW 12-bit sample counter [23:12]
#define PWL_REG_CFG             0x23    // W   2-bit global config

#define PWL_CHANNEL_STRIDE      0x10

// --------------------------------------------------------------------------
// MODE register fields
// --------------------------------------------------------------------------
#define PWL_MODE_DETUNE_EXP(n)  ((uint16_t)((n) & 7u))          // 0 = off, each +1 doubles
#define PWL_MODE_WF(w)          ((uint16_t)((((w) & 1u) << 3) | (((w) & 2u) << 7)))
#define PWL_MODE_FREQ_MULTS(n)  ((uint16_t)(((n) & 7u) << 4))   // or stereo_pos
#define PWL_MODE_STEREO_POS(n)  PWL_MODE_FREQ_MULTS(n)
#define PWL_MODE_COMMON_SAT     ((uint16_t)1u << 7)             // channel 0 only
#define PWL_MODE_OSC_SYNC(n)    ((uint16_t)(((n) & 3u) << 9))
#define PWL_MODE_DETUNE_5TH     ((uint16_t)1u << 11)            // channels 0, 2

// Waveform values for PWL_MODE_WF()
#define PWL_WF_OSC              0       // linear-phase oscillator (triangle family)
#define PWL_WF_NOISE            1       // LFSR noise (ch 0/3: 18-bit, ch 1/2: 11-bit)
#define PWL_WF_PWL_OSC          2       // piecewise-linear-phase osc (kills quantization)
#define PWL_WF_ORION            3       // Orion Wave (bit-shuffled harmonics)

// osc_sync values for PWL_MODE_OSC_SYNC().  NOTE: the peripheral docs
// list 1 = 4-bit and 2 = hard sync, but the silicon decodes the field
// as {bit10 = soft/4bit, bit9 = sync enable}: four_bit_mode_en =
// bit10 && !bit9 (pwl_synth.sv:443).  Verified on hardware (Saleae
// histogram: value 2 quantizes on ch 0/3; value 1 hard-syncs to the
// previous channel's oscillator).
#define PWL_SYNC_OFF            0
#define PWL_SYNC_HARD           1       // reset phase on prev channel wrap (ch 0/3)
#define PWL_SYNC_4BIT           2       // quantize to 4 bits (NES triangle, ch 0/3)
#define PWL_SYNC_SOFT           3       // invert phase on prev channel wrap (ch 0/3)

// CFG register bits
#define PWL_CFG_STEREO_EN       ((uint16_t)1u << 0)
#define PWL_CFG_STEREO_POS_EN   ((uint16_t)1u << 1)

// --------------------------------------------------------------------------
// Sweep encodings.
// rate: 0 = off, 1 = max, 2..15 = every 2*2^rate samples (at 1MHz).
// Period sweeps 4x as often as the others.
// --------------------------------------------------------------------------
// Constant-expression forms (usable in static initializers)
#define PWL_SWEEP_PA(period_down, period_rate, amp_target, amp_rate)     \
    ((uint16_t)(((amp_rate) & 15u) | (((amp_target) & 7u) << 4) |        \
                (((period_rate) & 15u) << 8) |                           \
                (((period_down) ? 1u : 0u) << 12)))

// slope_dir: 3 = both, 1 = slope_r only, 2 = slope_f only,
//            0 = opposite directions (slope_f flipped)
#define PWL_SWEEP_WS(pwm_down, pwm_rate, slope_down, slope_dir, slope_rate) \
    ((uint16_t)(((slope_rate) & 15u) | (((slope_down) ? 1u : 0u) << 4) |    \
                (((slope_dir) & 3u) << 5) | (((pwm_rate) & 15u) << 8) |     \
                (((pwm_down) ? 1u : 0u) << 12)))

static inline uint16_t pwl_sweep_pa(bool period_down, uint8_t period_rate,
                                    uint8_t amp_target, uint8_t amp_rate)
{
    return PWL_SWEEP_PA(period_down, period_rate, amp_target, amp_rate);
}

static inline uint16_t pwl_sweep_ws(bool pwm_down, uint8_t pwm_rate,
                                    bool slope_down, uint8_t slope_dir,
                                    uint8_t slope_rate)
{
    return PWL_SWEEP_WS(pwm_down, pwm_rate, slope_down, slope_dir, slope_rate);
}

// --------------------------------------------------------------------------
// Raw register access (16-bit; the synth latches the addressed register
// whatever the access size)
// --------------------------------------------------------------------------
// Inline asm rather than pointer dereference: the odd-address registers
// make GCC split a known-misaligned 16-bit access into two byte
// accesses (even with -mno-strict-align), and byte accesses are wrong
// here - an 8-bit write clobbers the whole addressed register with
// unspecified upper bits, and the second byte would hit the NEXT
// register.  A forced sh/lhu keeps it one 16-bit bus transaction;
// TinyQV's bus passes the byte address and size straight through.
static inline void pwl_write_raw(uint32_t offset, uint16_t value)
{
    asm volatile ("sh %0, 0(%1)"
                  :
                  : "r"(value), "r"(PWL_SYNTH_BASE_ADDRESS + offset)
                  : "memory");
}

static inline uint16_t pwl_read_raw(uint32_t offset)
{
    uint32_t value;

    asm volatile ("lhu %0, 0(%1)"
                  : "=r"(value)
                  : "r"(PWL_SYNTH_BASE_ADDRESS + offset)
                  : "memory");
    return (uint16_t)value;
}

static inline void pwl_write(int channel, uint32_t reg, uint16_t value)
{
    pwl_write_raw((uint32_t)(channel * PWL_CHANNEL_STRIDE) + reg, value);
}

static inline uint16_t pwl_read(int channel, uint32_t reg)
{
    return pwl_read_raw((uint32_t)(channel * PWL_CHANNEL_STRIDE) + reg);
}

static inline uint32_t pwl_read_counter(void)
{
    uint32_t lo = pwl_read_raw(PWL_REG_COUNTER_LO);
    uint32_t hi = pwl_read_raw(PWL_REG_COUNTER_HI);
    return (hi << 12) | (lo & 0xFFF);
}

// --------------------------------------------------------------------------
// Notes.  API takes MIDI note numbers (69 = A4 = 440Hz at 64MHz clock);
// usable range 11..106 (B-1..Bb7), clamped.  pwl_note_fperiod() returns
// the 13-bit f_period value.
// --------------------------------------------------------------------------
uint16_t pwl_note_fperiod(int midi_note);

// --------------------------------------------------------------------------
// Channel state cache + note control.
//
// mode / slopes / pwm_offset and the sweep values are cached per channel
// so hardware sweeps can trash the live registers during a note; the
// next pwl_note_on() restores the programmed sound.  (Same scheme as
// Toivo's reference driver.)
// --------------------------------------------------------------------------
typedef struct {
    uint8_t  slope_r, slope_f;
    uint8_t  pwm_offset;
    uint16_t mode;
    uint16_t sweep_pa, sweep_ws;
    int8_t   relative_detune;   // semitones added before auto-detune;
                                //   PWL_DETUNE_OFF disables detuning
    uint8_t  on;

    // ADSR envelope (pwl_set_adsr).  When enabled, the engine owns the
    // amp fields of sweep_pa: note-on runs A (hardware sweep to the
    // velocity peak), pwl_env_service() re-aims to D->S at the peak,
    // note-off runs R.  The period-sweep fields of the cached sweep_pa
    // are preserved, so pitch bends coexist with envelopes.
    uint8_t  env_enabled;
    uint8_t  env_a, env_d, env_s, env_r;    // rates 0-15 (0 = instant),
                                            //   sustain 0-7 (amp = 9*s)
    uint8_t  env_state;                     // PWL_ENV_*
    uint8_t  env_peak;                      // this note's peak (0-63)
    uint8_t  env_from;                      // amp at current stage start
    uint8_t  env_last;                      // last amp value written
    uint8_t  env_aim_amp;                   // amp fields of the last sweep_pa
                                            //   aim (so other writers compose)
    uint32_t env_t0;                        // us: current stage start
    uint32_t env_total;                     // us: current stage duration

    // Pitch envelope (pwl_set_pitch_env).  Note-on writes the period of
    // note+offset and arms the hardware period sweep toward the real
    // pitch; pwl_env_service() stops it at a precomputed deadline and
    // writes the exact target period (read-free, like the ADSR).  On
    // envelope channels note-off can arm a free-running release slide.
    int8_t   penv_offset;   // semitones the slide starts away (0 = off)
    uint8_t  penv_rate;     // period sweep rate 1-15
    uint8_t  penv_rel_rate; // release slide rate (0 = off)
    uint8_t  penv_rel_up;   // release slides up (default: pitch falls)
    uint8_t  penv_live;     // {sign,rate} armed in hardware (<<8), 0 = none
    uint8_t  penv_pending;  // stop-write scheduled at penv_stop_t
    uint16_t penv_target;   // exact f_period to land on
    uint32_t penv_stop_t;   // us deadline for the stop-write

    // Timbre envelope (pwl_set_timbre_env).  Note-on starts pwm_offset /
    // slopes displaced by the deltas and hardware-sweeps them home; the
    // stop-writes restore the exact programmed waveform, so clamping at
    // 0/255 can never leave the sound off-nominal.
    int16_t  tenv_pwm_delta;    // -255..255 start displacement (0 = off)
    uint8_t  tenv_pwm_rate;
    int8_t   tenv_slope_delta;  // start displacement per dir (0 = off)
    uint8_t  tenv_slope_rate;
    uint8_t  tenv_slope_dir;    // 3 both, 1 r only, 2 f only, 0 opposite
    uint8_t  tenv_pending;      // bit0 pwm, bit1 slopes: stop-writes due
    uint32_t tenv_pwm_stop_t;
    uint32_t tenv_slope_stop_t;

    // Vibrato LFO (pwl_set_vibrato).  The other envelopes shape the
    // start of a note; this one is the only modulator that runs while
    // the note is HELD, so it is software all the way: pwl_env_service()
    // writes the period register on a triangle around the note's pitch.
    uint8_t  vib_rate;      // LFO rate in 0.1Hz units (0 = off)
    uint8_t  vib_depth;     // peak deviation in cents (as the API took it)
    uint8_t  vib_steps;     // the same depth in raw f_period steps
    uint16_t vib_delay_ms;  // onset delay (and the fade-in length)
    uint16_t vib_base;      // f_period the swing centers on (0 = not armed)
    int16_t  vib_last;      // last delta written, to skip redundant writes
    uint32_t vib_t0;        // us: note start
    uint32_t vib_next;      // us: next LFO step due

    // Arpeggiator (pwl_set_arp): 2-3 chord tones cycled by
    // pwl_env_service() with plain period writes, so ONE voice fakes a
    // chord; the amp envelope gates once per chord and breathes over
    // the whole cycle.  Not for channels using vibrato or pitch
    // envelopes (all three write the period register).
    uint8_t  arp_i2, arp_i3;    // semitone offsets above the root (0 = off)
    uint8_t  arp_step_ms;       // ms per tone (0 = default 30)
    uint8_t  arp_n, arp_idx;    // cycle table state
    uint16_t arp_fp[3];         // precomputed f_periods for this chord
    uint32_t arp_next;          // us: next step due
} pwl_channel_state_t;

#define PWL_ENV_IDLE    0
#define PWL_ENV_ATTACK  1       // software-stepped by pwl_env_service()
#define PWL_ENV_DECAY   2       // software-stepped by pwl_env_service()
#define PWL_ENV_HELD    3       // at sustain; no activity
#define PWL_ENV_RELEASE 4       // hardware sweep to zero

#define PWL_DETUNE_OFF  (-128)

extern pwl_channel_state_t pwl_ch[PWL_NUM_CHANNELS];

// Program the channel's sound (cached; applied now if the note is on).
// mode carries waveform / freq_mults / osc_sync / common_sat / detune_5th
// bits - the detune_exp field is overwritten per note when auto-detune
// is enabled.
void pwl_set_waveform(int channel, uint8_t slope_r, uint8_t slope_f,
                      uint8_t pwm_offset, uint16_t mode);

// Cache sweep settings (applied at note-on; pass 0 to clear).
void pwl_set_sweeps(int channel, uint16_t sweep_pa, uint16_t sweep_ws);

// Start a note: silences the channel, restores the cached waveform and
// sweeps, computes detune_exp from the pitch, writes period + amp.
void pwl_note_on(int channel, int midi_note, uint8_t amp);

// Arpeggiator: cache the chord shape (i2 = 0 disables); every note-on
// then cycles root/+i2[/+i3] at the channel's step rate.
void pwl_set_arp(int channel, uint8_t i2, uint8_t i3);
void pwl_set_arp_rate(int channel, uint8_t step_ms);

// Legato slur: retune a sounding note WITHOUT re-articulating (no
// envelope restart, no pitch scoop, no timbre strike; vibrato
// re-centres with its delay restarted).  Full note-on when silent.
void pwl_note_slur(int channel, int midi_note, uint8_t amp);

// Stop a note: kills the amp sweep, then amp = 0.
void pwl_note_off(int channel);
void pwl_all_off(void);

// Global stereo configuration (cfg register write-only; shadowed here).
void pwl_set_cfg(uint16_t cfg);
uint16_t pwl_get_cfg(void);

// --------------------------------------------------------------------------
// ADSR amplitude envelopes.
//
// Rates use the hardware sweep scale: 1 = fastest (one amp step per
// 32 samples; full swing ~2ms), then 5..15 double each step
// (rate 9 ~64ms, 12 ~0.5s, 15 ~4s full swing); 0 = instant.  Rates
// 2-4 are clamped to 1 (hardware quirk).  Sustain is 0-7 (amp = 9*s).
// The attack peak comes from pwl_note_on()'s amp argument, so velocity
// shapes the envelope.
//
// pwl_env_service() must be called from idle/wait loops; it steps the
// attack and decay ramps in software (isolated amp writes, >=1ms apart,
// one channel per call).  Only the release tail uses a hardware sweep;
// sustain and release then run with zero CPU.
//
// The engine is READ-FREE: it tracks amp in a software shadow and,
// when a note retriggers a releasing channel, estimates the current
// amp from the linear release model instead of reading it back.
//
// KNOWN SILICON BEHAVIOR (root cause unresolved; not caused by this
// engine): a note-on from silence (amp 0) can take ~200-350ms to
// actually sound.  All registers read back correct during the gap and
// the output then starts on its own (timing suggests an internal
// ~2^18 us counter epoch).  It affects every driver path equally --
// legacy and envelope, all channels, with or without sweeps, reads,
// or detune -- and is uniform, so sequenced songs simply shift by a
// constant, inaudible offset.  Retriggering a sounding channel changes
// pitch immediately.  Reproduce with: off, wait, note.
// --------------------------------------------------------------------------
void pwl_set_adsr(int channel, uint8_t attack, uint8_t decay,
                  uint8_t sustain, uint8_t release);
void pwl_adsr_off(int channel);
void pwl_env_service(void);

// Feed-budget counters (rdtime ticks; seq_play resets and reports)
extern uint32_t pwl_svc_ticks, pwl_svc_max, pwl_svc_calls;

// --------------------------------------------------------------------------
// Pitch envelope: every note starts offset_semis away and hardware-
// sweeps to the real pitch.  Because the {exp, mantissa} period format
// is continuous across octave boundaries (1024 raw steps per octave,
// ~85 per semitone), the raw distance times the step time gives an
// exact deadline; the service then disarms the sweep and writes the
// exact target period.  rel_rate arms a free-running slide away from
// the note at note-off (envelope channels only - legacy note-off cuts
// the amp instantly, so there would be nothing to hear).
//
// Period sweep rates: 1-2 = 8us/step (~0.7ms per semitone), then each
// rate doubles from 3 (16us): rate 8 ~44ms/semi, 10 ~175ms/semi,
// 12 ~0.7s/semi, 15 ~5.6s/semi.  Works on both ADSR and legacy
// channels; composes with the amp envelope's sweep_pa aims.
// --------------------------------------------------------------------------
void pwl_set_pitch_env(int channel, int8_t offset_semis, uint8_t rate,
                       uint8_t rel_rate, bool rel_up);
void pwl_pitch_env_off(int channel);

// --------------------------------------------------------------------------
// Timbre envelope: the note starts with pwm_offset and/or the slopes
// displaced by the given deltas and hardware-sweeps back to the
// programmed waveform (sweep_ws); the service's stop-writes restore
// the exact preset values.  slope_dir picks which slopes take the
// displacement: 3 = both, 1 = slope_r only, 2 = slope_f only,
// 0 = opposite (slope_f displaced by -delta).  A slope delta of -16
// starts one "brightness octave" darker; positive pwm deltas start
// thinner (higher duty).  pwm/slope sweep rates: 1-4 = 32us/step,
// rate n >= 5 = 2*2^n us (rate 8 ~0.5ms/step, 11 ~4ms, 13 ~16ms).
// Rates of 0 (or zero deltas) disable that component.
// --------------------------------------------------------------------------
void pwl_set_timbre_env(int channel, int16_t pwm_delta, uint8_t pwm_rate,
                        int8_t slope_delta, uint8_t slope_rate,
                        uint8_t slope_dir);
void pwl_timbre_env_off(int channel);

// --------------------------------------------------------------------------
// Vibrato LFO: a triangle on the period register, written by
// pwl_env_service() (~one write per channel per 5ms, so ~33 points per
// cycle at 6Hz - call the service from idle/wait loops as usual).
// Period writes never click, so this needs no isolation from the amp
// writes and costs nothing but the writes themselves.
//
//   rate_dhz     LFO rate in 0.1Hz units, clamped to 1.0-25.5Hz
//                (60 = 6Hz: the middle of the violin / vocal range)
//   depth_cents  PEAK deviation, 0-127 (violin 15-40, 100 = a semitone)
//   delay_ms     how long a note waits before the vibrato starts, and
//                the length of the depth fade-in that follows (0-5000).
//                The delay is most of what makes vibrato sound played
//                rather than electronic - a bowed note arrives straight
//                and blooms.
//
// It swings around the note's OWN f_period, stays quiet until a pitch
// envelope's slide-in has landed, and stops at note-off, so it composes
// with pwl_set_pitch_env() and the amp ADSR.  It does NOT mix with
// period sweeps (bends) or manual period writes on the same channel:
// the LFO writes absolute values and will undo them.
// --------------------------------------------------------------------------
void pwl_set_vibrato(int channel, uint8_t rate_dhz, uint8_t depth_cents,
                     uint16_t delay_ms);
void pwl_vibrato_off(int channel);

// Arm/re-centre the LFO on an already-sounding note WITHOUT
// re-articulating it (live tweaks: studio panel knobs); note-on arms
// automatically.  Restarts the delay + fade-in on the held note.
void pwl_vibrato_arm(int channel, int midi_note);

// Silent envelope event log for timing debug ('envlog' in pwl-test):
// tag 1 = note_on entry, 2 = amp read a0, 3 = attack aimed (val =
// deadline low 16), 4 = DECAY aimed, 5 = release aimed, 6 = pitch
// slide armed (val = raw period steps), 7 = pitch slide stopped
// (val = target f_period)
typedef struct {
    uint32_t t;
    uint16_t val;
    uint8_t  tag;
} pwl_envlog_t;
#define PWL_ENVLOG_N 32
extern pwl_envlog_t pwl_envlog[PWL_ENVLOG_N];
extern uint8_t pwl_envlog_n;
