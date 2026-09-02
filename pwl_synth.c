// PiecewiseOrionSynth (pwl_synth, peripheral 33) driver for TinyQV.
// See pwl_synth.h for the register map and API notes.

#include "pwl_synth.h"
#include "csr.h"

extern uint32_t clock_hz;   // real project clock (pwl_test.h / main.c)

#ifdef PWL_ENV_DEBUG
#include <stdio.h>
#define ENV_DBG(...) printf(__VA_ARGS__)
#else
#define ENV_DBG(...)
#endif

// Silent RAM event log (printing disturbs the timing being debugged)
pwl_envlog_t pwl_envlog[PWL_ENVLOG_N];
uint8_t pwl_envlog_n;

static void envlog(uint8_t tag, uint16_t val)
{
    if (pwl_envlog_n < PWL_ENVLOG_N) {
        pwl_envlog[pwl_envlog_n].t = read_time();
        pwl_envlog[pwl_envlog_n].tag = tag;
        pwl_envlog[pwl_envlog_n].val = val;
        ++pwl_envlog_n;
    }
}

pwl_channel_state_t pwl_ch[PWL_NUM_CHANNELS];

static uint16_t pwl_cfg_shadow;

// ==========================================================================
// Note table: chromatic mantissas starting on B (from the reference
// driver; tuned so that {oct 4, A} = 440.1Hz at fs = 1MHz).
//   f = fs * 2^(oct-5) / (1024 + mantissa)
// ==========================================================================
static const uint16_t pwl_note_mantissa[12] = {
    1001,       // B
    887, 780,   // C  C#
    679, 583,   // D  D#
    493,        // E
    408, 327,   // F  F#
    252, 180,   // G  G#
    112, 49,    // A  A#
};

uint16_t pwl_note_fperiod(int midi_note)
{
    // midi 69 (A4) => t = 58 => index 10 (A), octave 4
    int t = midi_note - 11;

    if (t < 0)
        t = 0;
    if (t > 95)
        t = 95;

    int idx = t % 12;
    int oct = t / 12;
    uint32_t mantissa = pwl_note_mantissa[idx];

    // Scale the 64MHz-referenced period to the real clock (tqv.py
    // --freq 90): a faster clock needs a longer period for the same
    // pitch.  Renormalize into the octave exponent on overflow.
    if (clock_hz != 64000000u) {
        mantissa = (uint32_t)(((uint64_t)mantissa * clock_hz) / 64000000u);
        while (mantissa > 1023u && oct > 0) {
            mantissa >>= 1;
            oct--;
        }
        if (mantissa > 1023u)
            mantissa = 1023u;
    }

    // The two top octaves step the phase by 2/4: round the mantissa so
    // the period stays an integer number of samples (avoids warble)
    if (oct >= 6)
        mantissa &= (uint32_t)(-1 << (oct - 5));

    return (uint16_t)(mantissa | ((7 - oct) << 10));
}

// ==========================================================================
// Channel control
// ==========================================================================
static void pwl_arp_arm(int channel, int midi_note, uint32_t now);
static void pwl_arp_service(uint32_t now);

static void pwl_apply_waveform(int channel)
{
    const pwl_channel_state_t *ch = &pwl_ch[channel];

    pwl_write(channel, PWL_REG_SLOPE_R, ch->slope_r);
    pwl_write(channel, PWL_REG_SLOPE_F, ch->slope_f);
    pwl_write(channel, PWL_REG_PWM_OFFSET, ch->pwm_offset);
    pwl_write(channel, PWL_REG_MODE, ch->mode);
}

void pwl_set_waveform(int channel, uint8_t slope_r, uint8_t slope_f,
                      uint8_t pwm_offset, uint16_t mode)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->slope_r = slope_r;
    ch->slope_f = slope_f;
    ch->pwm_offset = pwm_offset;
    ch->mode = mode;

    if (ch->on)
        pwl_apply_waveform(channel);
}

void pwl_set_sweeps(int channel, uint16_t sweep_pa, uint16_t sweep_ws)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->sweep_pa = sweep_pa;
    ch->sweep_ws = sweep_ws;

    if (ch->on) {
        // An explicit sweep write takes the registers over from any
        // in-flight pitch/timbre slide (no stop-writes later)
        ch->penv_live = 0;
        ch->penv_pending = 0;
        ch->tenv_pending = 0;
        pwl_write(channel, PWL_REG_SWEEP_PA, sweep_pa);
        pwl_write(channel, PWL_REG_SWEEP_WS, sweep_ws);
    }
}

// ==========================================================================
// ADSR envelope engine (see pwl_synth.h for the model)
// ==========================================================================

// Hardware quirk: amp sweep rates 2-4 behave like 1; encode around it
// (the pwm/slope sweeps share the same scheduler slot timing)
static uint8_t pwl_amp_rate(uint8_t r)
{
    return (r >= 2 && r <= 4) ? 1 : (r & 15);
}

// Every sweep_pa write goes through this composition: the period
// fields come from the pitch envelope's live slide when one is armed,
// else from the channel's cached sweep_pa (so user pitch bends
// survive); the amp fields are the caller's.
static uint16_t pwl_pa_effective(const pwl_channel_state_t *ch,
                                 uint16_t amp_bits)
{
    uint16_t period_bits = ch->penv_live
        ? (uint16_t)((uint16_t)ch->penv_live << 8)
        : (uint16_t)(ch->sweep_pa & 0x1F00u);

    return (uint16_t)(period_bits | amp_bits);
}

// The amp fields a non-envelope channel carries in its cached sweep_pa
static uint16_t pwl_pa_amp_bits(const pwl_channel_state_t *ch)
{
    return ch->env_enabled ? ch->env_aim_amp
                           : (uint16_t)(ch->sweep_pa & 0x7Fu);
}

// Aim the hardware amp sweep; remembers the amp fields so pitch-slide
// stop-writes can recompose the register without disturbing them
static void pwl_env_aim(int channel, uint8_t amp_target_t, uint8_t rate)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->env_aim_amp = (uint8_t)(((amp_target_t & 7u) << 4) |
                                pwl_amp_rate(rate));
    pwl_write(channel, PWL_REG_SWEEP_PA, pwl_pa_effective(ch, ch->env_aim_amp));
}

// The sweep_ws value that should be live right now: the timbre
// envelope's homeward sweeps while one is running, else the cached
// user value
static uint16_t pwl_ws_effective(const pwl_channel_state_t *ch)
{
    uint16_t v = 0;

    if (!ch->tenv_pending)
        return ch->sweep_ws;
    if (ch->tenv_pending & 1)
        v |= (uint16_t)(((ch->tenv_pwm_delta > 0) ? 0x1000u : 0) |
                        ((uint16_t)pwl_amp_rate(ch->tenv_pwm_rate) << 8));
    if (ch->tenv_pending & 2)
        v |= (uint16_t)(((ch->tenv_slope_delta > 0) ? 0x10u : 0) |
                        ((uint16_t)(ch->tenv_slope_dir & 3u) << 5) |
                        pwl_amp_rate(ch->tenv_slope_rate));
    return v;
}

void pwl_set_adsr(int channel, uint8_t attack, uint8_t decay,
                  uint8_t sustain, uint8_t release)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->env_enabled = 1;
    ch->env_a = attack & 15;
    ch->env_d = decay & 15;
    ch->env_s = sustain & 7;
    ch->env_r = release & 15;
    ch->env_state = PWL_ENV_IDLE;
}

void pwl_adsr_off(int channel)
{
    pwl_ch[channel].env_enabled = 0;
    pwl_ch[channel].env_state = PWL_ENV_IDLE;
}

// Microseconds per sweep step at a given amp rate (~1us per sample)
static uint32_t pwl_amp_step_us(uint8_t rate)
{
    rate = pwl_amp_rate(rate);
    if (rate <= 1)
        return 32;                      // max: one step per 32 samples
    return (uint32_t)2u << rate;        // one step per 2*2^rate samples
}

// Period sweeps run in 4x as many scheduler slots: max one step per
// 8 samples (rates 1-2), then 2*2^rate samples from rate 3 up
static uint32_t pwl_period_step_us(uint8_t rate)
{
    rate &= 15;
    if (rate == 0)
        return 0;
    if (rate <= 2)
        return 8;
    return (uint32_t)2u << rate;
}

// ==========================================================================
// Pitch / timbre envelopes (see pwl_synth.h for the model)
// ==========================================================================
void pwl_set_pitch_env(int channel, int8_t offset_semis, uint8_t rate,
                       uint8_t rel_rate, bool rel_up)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->penv_offset = offset_semis;
    ch->penv_rate = rate & 15;
    ch->penv_rel_rate = rel_rate & 15;
    ch->penv_rel_up = rel_up;
}

void pwl_pitch_env_off(int channel)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->penv_offset = 0;
    ch->penv_rate = 0;
    ch->penv_rel_rate = 0;
    ch->penv_rel_up = 0;
    if (ch->penv_live || ch->penv_pending) {
        uint8_t pend = ch->penv_pending;

        ch->penv_live = 0;
        ch->penv_pending = 0;
        pwl_write(channel, PWL_REG_SWEEP_PA,
                  pwl_pa_effective(ch, pwl_pa_amp_bits(ch)));
        if (pend)               // land the interrupted slide now
            pwl_write(channel, PWL_REG_PERIOD, ch->penv_target);
    }
}

void pwl_set_timbre_env(int channel, int16_t pwm_delta, uint8_t pwm_rate,
                        int8_t slope_delta, uint8_t slope_rate,
                        uint8_t slope_dir)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    if (pwm_delta < -255)
        pwm_delta = -255;
    if (pwm_delta > 255)
        pwm_delta = 255;
    ch->tenv_pwm_delta = pwm_delta;
    ch->tenv_pwm_rate = pwm_rate & 15;
    ch->tenv_slope_delta = slope_delta;
    ch->tenv_slope_rate = slope_rate & 15;
    ch->tenv_slope_dir = slope_dir & 3;
}

void pwl_timbre_env_off(int channel)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];
    uint8_t pend = ch->tenv_pending;

    ch->tenv_pwm_delta = 0;
    ch->tenv_pwm_rate = 0;
    ch->tenv_slope_delta = 0;
    ch->tenv_slope_rate = 0;
    if (pend) {
        ch->tenv_pending = 0;
        pwl_write(channel, PWL_REG_SWEEP_WS, pwl_ws_effective(ch));
        if (pend & 1)           // land interrupted slides on the preset
            pwl_write(channel, PWL_REG_PWM_OFFSET, ch->pwm_offset);
        if (pend & 2) {
            pwl_write(channel, PWL_REG_SLOPE_R, ch->slope_r);
            pwl_write(channel, PWL_REG_SLOPE_F, ch->slope_f);
        }
    }
}

static uint8_t pwl_clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Note-on hook: returns the f_period to start the note at and, when a
// slide is wanted, arms the read-free stop (penv_live goes live with
// the caller's next composed sweep_pa write)
static uint16_t pwl_pitch_env_start(int channel, int midi_note, uint32_t now)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];
    uint16_t target = pwl_note_fperiod(midi_note);

    ch->penv_live = 0;
    ch->penv_pending = 0;
    if (ch->penv_offset != 0 && ch->penv_rate != 0) {
        uint16_t start = pwl_note_fperiod(midi_note + ch->penv_offset);

        if (start != target) {
            uint16_t steps = (uint16_t)(start > target ? start - target
                                                       : target - start);

            // raw period must fall to reach a higher pitch: sign = down
            ch->penv_live = (uint8_t)((start > target ? 0x10u : 0) |
                                      ch->penv_rate);
            ch->penv_target = target;
            ch->penv_stop_t = now + (uint32_t)steps *
                              pwl_period_step_us(ch->penv_rate);
            ch->penv_pending = 1;
            envlog(6, steps);
            return start;
        }
    }
    return target;
}

// Note-on hook: write the displaced pwm_offset / slopes and schedule
// their homeward stop-writes.  Runs right after pwl_apply_waveform, so
// the displaced values overwrite the nominal ones.
static void pwl_timbre_env_start(int channel, uint32_t now)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->tenv_pending = 0;
    if (ch->tenv_pwm_delta != 0 && ch->tenv_pwm_rate != 0) {
        uint8_t start = pwl_clamp_u8(ch->pwm_offset + ch->tenv_pwm_delta);

        if (start != ch->pwm_offset) {
            uint16_t steps = (uint16_t)(start > ch->pwm_offset
                                            ? start - ch->pwm_offset
                                            : ch->pwm_offset - start);

            pwl_write(channel, PWL_REG_PWM_OFFSET, start);
            ch->tenv_pwm_stop_t = now + (uint32_t)steps *
                                  pwl_amp_step_us(ch->tenv_pwm_rate);
            ch->tenv_pending |= 1;
        }
    }
    if (ch->tenv_slope_delta != 0 && ch->tenv_slope_rate != 0) {
        int dr = 0, df = 0;

        switch (ch->tenv_slope_dir & 3) {
        case 3: dr = df = ch->tenv_slope_delta; break;
        case 1: dr = ch->tenv_slope_delta; break;
        case 2: df = ch->tenv_slope_delta; break;
        case 0: dr = ch->tenv_slope_delta; df = -dr; break;
        }

        uint8_t rs = pwl_clamp_u8(ch->slope_r + dr);
        uint8_t fs = pwl_clamp_u8(ch->slope_f + df);

        if (rs != ch->slope_r || fs != ch->slope_f) {
            int steps = ch->tenv_slope_delta;

            if (steps < 0)
                steps = -steps;
            pwl_write(channel, PWL_REG_SLOPE_R, rs);
            pwl_write(channel, PWL_REG_SLOPE_F, fs);
            ch->tenv_slope_stop_t = now + (uint32_t)steps *
                                    pwl_amp_step_us(ch->tenv_slope_rate);
            ch->tenv_pending |= 2;
        }
    }
}

// ==========================================================================
// Vibrato LFO.  Everything else here shapes the START of a note and
// hands off to hardware; a held vibrato has no hardware to hand off to
// (the sweeps only ramp one way), so this is a software triangle on the
// period register.
// ==========================================================================
#define PWL_VIB_STEP_US     5000u   // one period write per channel per 5ms
#define PWL_VIB_MIN_RATE    10u     // 1.0Hz: below this the phase math
                                    //   would overflow 32 bits
#define PWL_VIB_MAX_DELAY   5000u   // ms; also bounds the fade-in math

// Disarm the LFO.  A sounding note may sit up to the last delta
// off-pitch (the LFO writes absolute periods), so put it back on the
// note first - unless a pitch slide owns the register right now.
static void pwl_vib_disarm(int channel)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    if (ch->vib_base != 0 && ch->on && ch->vib_last != 0 &&
        !ch->penv_pending)
        pwl_write(channel, PWL_REG_PERIOD, ch->vib_base);
    ch->vib_base = 0;
    ch->vib_last = 0;
}

void pwl_set_vibrato(int channel, uint8_t rate_dhz, uint8_t depth_cents,
                     uint16_t delay_ms)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    if (rate_dhz != 0 && rate_dhz < PWL_VIB_MIN_RATE)
        rate_dhz = PWL_VIB_MIN_RATE;
    if (depth_cents > 127)
        depth_cents = 127;
    if (delay_ms > PWL_VIB_MAX_DELAY)
        delay_ms = PWL_VIB_MAX_DELAY;

    ch->vib_rate = rate_dhz;
    ch->vib_depth = depth_cents;
    // f_period runs 1024 raw steps per octave, so 1024/1200 per cent
    ch->vib_steps = (uint8_t)(((uint32_t)depth_cents * 1024u + 600u) / 1200u);
    ch->vib_delay_ms = delay_ms;
    if (rate_dhz == 0 || ch->vib_steps == 0)
        pwl_vib_disarm(channel);    // also snaps a wobbling note on-pitch
}

void pwl_vibrato_off(int channel)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->vib_rate = 0;
    ch->vib_depth = 0;
    ch->vib_steps = 0;
    pwl_vib_disarm(channel);
}

// Note-on hook: center the swing on the pitch the note LANDS on (with a
// pitch envelope the period starts somewhere else) and restart the
// delay, so every note blooms the same way.
static void pwl_vibrato_start(int channel, uint16_t base, uint32_t now)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->vib_last = 0;
    ch->vib_base = 0;
    if (ch->vib_rate != 0 && ch->vib_steps != 0) {
        ch->vib_base = base;
        ch->vib_t0 = now;
        ch->vib_next = now;
    }
}

// Arm (or re-centre) the LFO on a note that is ALREADY sounding, without
// re-articulating it - the live-tweak path (studio panel knobs, CLI while
// a note rings).  The delay + fade-in restart, so the wobble blooms into
// the held note.  No-op when the channel is silent (note-on arms it
// itself) or vibrato is disabled.
void pwl_vibrato_arm(int channel, int midi_note)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];
    uint16_t base = pwl_note_fperiod(midi_note);

    if (!ch->on || ch->vib_rate == 0 || ch->vib_steps == 0)
        return;
    // Start from on-pitch: the restarted delay window writes nothing,
    // so a leftover wobble offset would otherwise hang there
    if (ch->vib_last != 0 && !ch->penv_pending)
        pwl_write(channel, PWL_REG_PERIOD, base);
    pwl_vibrato_start(channel, base, read_time());
}

// Triangle rather than sine: at 6Hz and a few tens of cents the two are
// indistinguishable, and this costs no table and no multiply-heavy
// interpolation.
static void pwl_vibrato_service(uint32_t now)
{
    for (int channel = 0; channel < PWL_NUM_CHANNELS; ++channel) {
        pwl_channel_state_t *ch = &pwl_ch[channel];
        uint32_t period_us, delay_us, quarter, el, t, ph;
        int32_t tri, delta, value;

        if (ch->vib_base == 0 || !ch->on)
            continue;
        if (ch->penv_pending)               // let the slide-in land first
            continue;
        if ((int32_t)(now - ch->vib_next) < 0)
            continue;
        ch->vib_next = now + PWL_VIB_STEP_US;

        delay_us = (uint32_t)ch->vib_delay_ms * 1000u;
        el = now - ch->vib_t0;
        if (el < delay_us)
            continue;                       // straight tone, no wobble yet
        t = el - delay_us;

        period_us = 10000000u / ch->vib_rate;       // rate is in 0.1Hz
        quarter = period_us / 4u;
        if (quarter == 0)
            continue;
        ph = t % period_us;

        // 0 -> +1 over the first quarter, +1 -> -1, then -1 -> 0
        if (ph < quarter)
            tri = (int32_t)((ph * 1024u) / quarter);
        else if (ph < 3u * quarter)
            tri = 1024 - (int32_t)(((ph - quarter) * 1024u) / quarter);
        else
            tri = -1024 + (int32_t)(((ph - 3u * quarter) * 1024u) / quarter);

        delta = tri * (int32_t)ch->vib_steps / 1024;

        // Fade the depth in over the delay again: the swell into the
        // vibrato is as much of the "played" cue as the vibrato itself
        if (delay_us != 0 && t < delay_us)
            delta = delta * (int32_t)t / (int32_t)delay_us;

        if (delta == ch->vib_last)
            continue;
        ch->vib_last = (int16_t)delta;

        value = (int32_t)ch->vib_base + delta;
        if (value < 0)
            value = 0;
        if (value > 0x1FFF)
            value = 0x1FFF;
        pwl_write(channel, PWL_REG_PERIOD, (uint16_t)value);
    }
}

// Stop-writes for slides whose deadline passed: disarm the sweep
// first (so it can't step past while we finish), then write the exact
// landing values.  One-shot events, so bursts here are fine.
static void pwl_env_stops(uint32_t now)
{
    for (int channel = 0; channel < PWL_NUM_CHANNELS; ++channel) {
        pwl_channel_state_t *ch = &pwl_ch[channel];

        if (ch->penv_pending && (int32_t)(now - ch->penv_stop_t) >= 0) {
            ch->penv_live = 0;
            ch->penv_pending = 0;
            pwl_write(channel, PWL_REG_SWEEP_PA,
                      pwl_pa_effective(ch, pwl_pa_amp_bits(ch)));
            pwl_write(channel, PWL_REG_PERIOD, ch->penv_target);
            envlog(7, ch->penv_target);
        }
        if ((ch->tenv_pending & 1) &&
            (int32_t)(now - ch->tenv_pwm_stop_t) >= 0) {
            ch->tenv_pending &= (uint8_t)~1u;
            pwl_write(channel, PWL_REG_SWEEP_WS, pwl_ws_effective(ch));
            pwl_write(channel, PWL_REG_PWM_OFFSET, ch->pwm_offset);
        }
        if ((ch->tenv_pending & 2) &&
            (int32_t)(now - ch->tenv_slope_stop_t) >= 0) {
            ch->tenv_pending &= (uint8_t)~2u;
            pwl_write(channel, PWL_REG_SWEEP_WS, pwl_ws_effective(ch));
            pwl_write(channel, PWL_REG_SLOPE_R, ch->slope_r);
            pwl_write(channel, PWL_REG_SLOPE_F, ch->slope_f);
        }
    }
}

// Feed-budget instrumentation: every service call is timed so the
// player can report what the software stack (ADSR staging, pitch/timbre
// stop-writes, vibrato/arp LFOs) actually costs - the headroom number
// any added display/decoder work has to live inside.
uint32_t pwl_svc_ticks, pwl_svc_max, pwl_svc_calls;

static void pwl_env_service_inner(void);

void pwl_env_service(void)
{
    uint32_t t0 = read_time();

    // NOTE: rate-gating this call is a measured NO-OP (A/B on demo 2:
    // 23.3% CPU ungated vs 23.6% gated at 1kHz) - the per-call cost
    // scales with the gap, i.e. the inner does fixed work per unit
    // TIME (catch-up stepping), so fewer calls just do more each.
    // Cutting the feed for real means deadline-skipping inside.

    pwl_env_service_inner();
    {
        uint32_t dt = read_time() - t0;

        pwl_svc_ticks += dt;
        pwl_svc_calls++;
        if (dt > pwl_svc_max)
            pwl_svc_max = dt;
    }
}

static void pwl_env_service_inner(void)
{
    static uint32_t last_write_us;
    uint32_t now = read_time();

    // Pitch/timbre slide stop-writes fire as soon as they are due --
    // lateness means audible overshoot (the hardware keeps sweeping
    // until the disarm lands)
    pwl_env_stops(now);

    // Held-note modulation.  Ahead of the amp gate below: period writes
    // don't need the isolation that amp writes do, and the LFO has to
    // keep its own 5ms-per-channel cadence to stay smooth.
    pwl_vibrato_service(now);

    // Arpeggiator cycles: same period-write class as the LFO
    pwl_arp_service(now);

    // Software-stepped A and D stages: linear amp trajectory, written
    // as isolated single amp writes (immune to the sweep stall).  At
    // most one register write per call; >=1ms between writes per
    // channel keeps every write isolated.
    if ((uint32_t)(now - last_write_us) < 1000u)
        return;

    for (int channel = 0; channel < PWL_NUM_CHANNELS; ++channel) {
        pwl_channel_state_t *ch = &pwl_ch[channel];
        uint8_t target, from;
        uint32_t el;

        if (!ch->env_enabled)
            continue;
        if (ch->env_state != PWL_ENV_ATTACK && ch->env_state != PWL_ENV_DECAY)
            continue;

        from = ch->env_from;
        target = (ch->env_state == PWL_ENV_ATTACK)
                     ? ch->env_peak : (uint8_t)(9 * ch->env_s);
        el = now - ch->env_t0;

        uint8_t a;
        if (el >= ch->env_total || ch->env_total == 0)
            a = target;
        else if (target >= from)
            a = (uint8_t)(from + (uint32_t)(target - from) * el / ch->env_total);
        else
            a = (uint8_t)(from - (uint32_t)(from - target) * el / ch->env_total);

        if (a != ch->env_last) {
            pwl_write(channel, PWL_REG_AMP, a);
            ch->env_last = a;
            last_write_us = now;
        }

        if (a == target) {
            if (ch->env_state == PWL_ENV_ATTACK && 9 * ch->env_s < ch->env_peak) {
                envlog(4, (uint16_t)channel);
                ch->env_from = ch->env_peak;
                ch->env_t0 = now;
                ch->env_total = (uint32_t)(ch->env_peak - 9 * ch->env_s)
                                * pwl_amp_step_us(ch->env_d);
                if (ch->env_d == 0)
                    ch->env_total = 0;
                ch->env_state = PWL_ENV_DECAY;
            } else {
                ch->env_state = PWL_ENV_HELD;
            }
        }
        return;             // one channel serviced per call
    }
}

void pwl_note_on(int channel, int midi_note, uint8_t amp)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];
    uint16_t mode = ch->mode;

    // Auto-detune: scale detune_exp with pitch so the beat rate tracks
    // the note (reference-driver formula: detune halves per octave down).
    // detune_5th gives the half steps on channels 0 and 2; freq_mults
    // modes leave sub-channel 0 undetuned, so skip the half step there.
    if (ch->relative_detune != PWL_DETUNE_OFF) {
        int det = (midi_note - 12 + ch->relative_detune) / 6;
        if (det < 0)
            det = 0;
        if (det > 15)
            det = 15;
        mode = (uint16_t)((mode & ~7u) | ((uint32_t)det >> 1));
        if ((pwl_cfg_shadow & PWL_CFG_STEREO_POS_EN) ||
            (mode & PWL_MODE_FREQ_MULTS(7)) == 0) {
            mode = (uint16_t)((mode & ~PWL_MODE_DETUNE_5TH) |
                              ((det & 1) ? PWL_MODE_DETUNE_5TH : 0));
        }
        ch->mode = mode;
    }

    if (ch->env_enabled) {
        // Envelope path: never zero amp (attack starts from wherever
        // the level is - retrigger and steal stay click-free); the amp
        // fields of sweep_pa belong to the engine.
        uint8_t peak_t = (uint8_t)((amp + 4) / 9);

        if (peak_t < 1)
            peak_t = 1;
        if (peak_t > 7)
            peak_t = 7;
        ch->env_peak = (uint8_t)(9 * peak_t);

        ch->penv_live = 0;                      // stale slide must not
        ch->penv_pending = 0;                   //   leak into the freeze
        pwl_env_aim(channel, 0, 0);             // freeze amp during setup
        pwl_write(channel, PWL_REG_SWEEP_WS, 0);
        pwl_apply_waveform(channel);

        uint32_t now0 = read_time();

        pwl_timbre_env_start(channel, now0);
        pwl_write(channel, PWL_REG_PERIOD,
                  pwl_pitch_env_start(channel, midi_note, now0));
        pwl_vibrato_start(channel, pwl_note_fperiod(midi_note), now0);
        pwl_arp_arm(channel, midi_note, now0);
        pwl_write(channel, PWL_REG_AMP, ch->env_last);
        pwl_write(channel, PWL_REG_SWEEP_WS, pwl_ws_effective(ch));

        envlog(1, (uint16_t)midi_note);
        if (ch->env_a == 0) {
            pwl_write(channel, PWL_REG_AMP, ch->env_peak);
            ch->env_last = ch->env_peak;
            if (9 * ch->env_s < ch->env_peak) {
                if (ch->env_d == 0) {
                    pwl_write(channel, PWL_REG_AMP, (uint8_t)(9 * ch->env_s));
                    ch->env_last = (uint8_t)(9 * ch->env_s);
                }
                pwl_env_aim(channel, ch->env_s, ch->env_d);
            } else if (ch->penv_live) {
                pwl_env_aim(channel, 0, 0);     // arm the pitch slide
            }
            ch->env_state = PWL_ENV_HELD;
        } else {
            // Software-stepped attack (isolated amp writes from the
            // service).  Read-free by design: the amp shadow tracks
            // the level, and a retrigger during the hardware release
            // estimates the current level from the release model --
            // no bus reads, and the trajectory is known ahead of time.
            uint8_t a0 = ch->env_last;

            if (ch->env_state == PWL_ENV_RELEASE && ch->env_r != 0) {
                uint32_t el = read_time() - ch->env_t0;
                uint32_t fallen = el / pwl_amp_step_us(ch->env_r);

                a0 = (fallen >= ch->env_from) ? 0
                     : (uint8_t)(ch->env_from - fallen);
            }
            envlog(2, a0);
            ch->env_from = a0;
            ch->env_last = a0;
            ch->env_t0 = read_time();
            ch->env_total = (uint32_t)((a0 < ch->env_peak)
                                ? (ch->env_peak - a0) : 1)
                            * pwl_amp_step_us(ch->env_a);
            envlog(3, (uint16_t)(ch->env_total / 1000));
            ch->env_state = PWL_ENV_ATTACK;
            if (ch->penv_live)
                pwl_env_aim(channel, 0, 0);     // arm the pitch slide
        }
        ch->on = 1;
        return;
    }

    // Silence first so the waveform change can't click at full volume
    pwl_write(channel, PWL_REG_SWEEP_PA, 0);
    pwl_write(channel, PWL_REG_SWEEP_WS, 0);
    pwl_write(channel, PWL_REG_AMP, 0);

    pwl_apply_waveform(channel);

    uint32_t now0 = read_time();

    pwl_timbre_env_start(channel, now0);
    pwl_write(channel, PWL_REG_PERIOD,
              pwl_pitch_env_start(channel, midi_note, now0));
    pwl_vibrato_start(channel, pwl_note_fperiod(midi_note), now0);
    pwl_arp_arm(channel, midi_note, now0);
    pwl_write(channel, PWL_REG_AMP, amp & 63);
    ch->env_last = amp & 63;

    pwl_write(channel, PWL_REG_SWEEP_PA,
              pwl_pa_effective(ch, (uint16_t)(ch->sweep_pa & 0x7Fu)));
    pwl_write(channel, PWL_REG_SWEEP_WS, pwl_ws_effective(ch));
    ch->on = 1;
}

void pwl_set_arp(int channel, uint8_t i2, uint8_t i3)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    ch->arp_i2 = i2;
    ch->arp_i3 = (uint8_t)(i2 ? i3 : 0);
    if (!i2)
        ch->arp_n = 0;                  // stop cycling; pitch stays put
}

void pwl_set_arp_rate(int channel, uint8_t step_ms)
{
    pwl_ch[channel].arp_step_ms = step_ms;
}

// Note-on tail hook: precompute this chord's cycle table
static void pwl_arp_arm(int channel, int midi_note, uint32_t now)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];
    uint8_t step = ch->arp_step_ms ? ch->arp_step_ms : 30;

    if (!ch->arp_i2) {
        ch->arp_n = 0;
        return;
    }
    ch->arp_fp[0] = pwl_note_fperiod(midi_note);
    ch->arp_fp[1] = pwl_note_fperiod(midi_note + ch->arp_i2);
    ch->arp_n = 2;
    if (ch->arp_i3) {
        ch->arp_fp[2] = pwl_note_fperiod(midi_note + ch->arp_i3);
        ch->arp_n = 3;
    }
    ch->arp_idx = 0;
    ch->arp_next = now + (uint32_t)step * 1000u;
}

// Chiptune chord shimmer: step every armed channel's cycle.  Period
// writes never click, so this needs no isolation from the amp writes.
static void pwl_arp_service(uint32_t now)
{
    for (int c = 0; c < PWL_NUM_CHANNELS; ++c) {
        pwl_channel_state_t *ch = &pwl_ch[c];
        uint8_t step;

        if (ch->arp_n < 2 || !ch->on)
            continue;
        if ((int32_t)(now - ch->arp_next) < 0)
            continue;
        step = ch->arp_step_ms ? ch->arp_step_ms : 30;
        ch->arp_idx = (uint8_t)((ch->arp_idx + 1) % ch->arp_n);
        pwl_write(c, PWL_REG_PERIOD, ch->arp_fp[ch->arp_idx]);
        ch->arp_next += (uint32_t)step * 1000u;
        if ((int32_t)(now - ch->arp_next) > 0)  // stalled (FS write):
            ch->arp_next = now + (uint32_t)step * 1000u;  // resync
    }
}

// Legato slur: move a SOUNDING note to a new pitch without
// re-articulating.  The amp envelope keeps breathing through the run,
// the pitch-envelope scoop does NOT re-fire, the timbre strike does
// not re-trigger, and the vibrato LFO re-centres on the new pitch with
// its onset delay restarted - so slurred runs stay straight and the
// bloom lands on the note finally held.  Detune_exp keeps the
// phrase-start value (recomputing would rewrite live waveform
// registers mid-envelope to nudge an inaudible beat rate).  Falls back
// to a full note-on when nothing is sounding.
void pwl_note_slur(int channel, int midi_note, uint8_t amp)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    if (!ch->on) {
        pwl_note_on(channel, midi_note, amp);
        return;
    }
    if (ch->penv_live || ch->penv_pending) {
        // a slide still flying toward the PREVIOUS pitch must not keep
        // sweeping the period register we are about to write
        ch->penv_live = 0;
        ch->penv_pending = 0;
        pwl_write(channel, PWL_REG_SWEEP_PA,
                  pwl_pa_effective(ch, pwl_pa_amp_bits(ch)));
    }
    pwl_write(channel, PWL_REG_PERIOD, pwl_note_fperiod(midi_note));
    pwl_vibrato_arm(channel, midi_note);
}

void pwl_note_off(int channel)
{
    pwl_channel_state_t *ch = &pwl_ch[channel];

    if (ch->env_enabled) {
        // Release: hardware sweeps to zero and stays there, no CPU.
        // Record start level/time for the retrigger model.
        envlog(5, (uint16_t)channel);
        ch->env_from = ch->env_last;
        ch->env_t0 = read_time();
        if (ch->env_r == 0) {
            pwl_write(channel, PWL_REG_AMP, 0);
            ch->env_last = 0;
        }
        if (ch->penv_rel_rate) {
            // Free-running release slide (composed into the aim below);
            // the period just rails silently if the tail is long, and
            // the next note-on rewrites it anyway
            ch->penv_live = (uint8_t)((ch->penv_rel_up ? 0x10u : 0) |
                                      ch->penv_rel_rate);
            ch->penv_pending = 0;
        }
        pwl_env_aim(channel, 0, ch->env_r);
        ch->env_state = PWL_ENV_RELEASE;
        ch->on = 0;
        return;
    }

    // Kill the amp sweep first so it can't ramp the level back up
    pwl_write(channel, PWL_REG_SWEEP_PA, 0);
    pwl_write(channel, PWL_REG_AMP, 0);
    ch->env_last = 0;
    ch->penv_live = 0;          // sweeps are dead; drop any slide state
    ch->penv_pending = 0;
    ch->tenv_pending = 0;
    ch->on = 0;
}

void pwl_all_off(void)
{
    for (int ch = 0; ch < PWL_NUM_CHANNELS; ++ch)
        pwl_note_off(ch);
}

void pwl_set_cfg(uint16_t cfg)
{
    pwl_cfg_shadow = cfg;
    pwl_write_raw(PWL_REG_CFG, cfg);
}

uint16_t pwl_get_cfg(void)
{
    return pwl_cfg_shadow;
}
