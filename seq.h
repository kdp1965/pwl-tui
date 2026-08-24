#pragma once

// pwl-test sequencer event format.
//
// A song is an array of seq_ev_t ending in EV_END, played by seq_play()
// in main.c: wait dt_ms, execute, repeat.  Hand-written demo tables and
// the tools/mid2pwl.py MIDI converter both target this format.
//
// Preset indexes used by EV_PRESET refer to the presets[] table in
// main.c, in this (stable) order:
//   0 tri      1 nes      2 square   3 saw      4 pulse25
//   5 pulse12  6 sine     7 clamp    8 organ    9 power
//  10 third   11 noise   12 orion   13 smooth  14 violin

#include <stdint.h>

typedef enum {
    EV_END = 0,     // end of song (dt ignored)
    EV_ON,          // ch, a = midi note, b = amp
    EV_OFF,         // ch
    EV_PRESET,      // ch, a = preset index
    EV_SWEEP_PA,    // ch, a|b<<8 = sweep value
    EV_SWEEP_WS,    // ch, a|b<<8 = sweep value
    EV_DETUNE,      // ch, a = relative_detune (int8; 0x80 = off)
    EV_KICK,        // ch
    EV_SNARE,       // ch
    EV_HAT,         // ch
    EV_PERIOD,      // ch, a|b<<8 = raw f_period (slide landings, microtones)
    EV_AMP,         // ch, a = amplitude (no retrigger)
    EV_VIB,         // ch, a = midi note, b = vibrato cycles (blocking,
                    //   ~10Hz, +-1/3 semitone via raw mantissa wobble)
    EV_NOP,         // nothing (holds a grid slot: rests, muted drums)
    EV_ADSR,        // ch, a = attack<<4|decay, b = sustain<<4|release
                    //   (a = 0xFF disables the envelope)
    EV_PENV,        // ch, a = slide-in start offset in semitones (int8),
                    //   b = rate | release_rate<<4 (release slide falls;
                    //   a=0 b=0 disables the pitch envelope)
    EV_TPWM,        // ch, a = pwm_offset start delta / 2 (int8),
                    //   b = rate (a=0 disables the pwm component)
    EV_TSLOPE,      // ch, a = slope start delta (int8), b = rate | dir<<4
                    //   (dir: 3 both, 1 r, 2 f, 0 opposite; a=0 disables)
    EV_VIBRATO,     // ch, a = LFO rate in 0.1Hz (0 = off), b = depth in
                    //   2-cent units [5:0] | onset delay [7:6] (0, 250,
                    //   500, 750ms).  Unlike EV_VIB this does not block:
                    //   the LFO runs from pwl_env_service() until the
                    //   note ends.  See VIB() in main.c.
    EV_SLUR,        // ch, a = midi note, b = amp: legato retune of the
                    //   sounding note - no re-articulation (envelope,
                    //   scoop and timbre strike all keep running);
                    //   full EV_ON behavior if the channel is silent
    EV_ARPIV,       // ch, a/b = semitone intervals above the root
                    //   (0 = off): the NEXT note-ons on this channel
                    //   arpeggiate root/+a/+b - one voice fakes a chord
    EV_ARPRATE,     // ch, a = ms per arp tone (0 = default 30)
} ev_cmd_t;

typedef struct {
    uint16_t dt_ms;     // delay after the previous event
    uint8_t  cmd, ch, a, b;
} seq_ev_t;
