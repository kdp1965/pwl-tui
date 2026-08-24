/************************************************************************************
 * pwl-test/tui/Mid2Pwl.h
 *
 * On-target port of tools/mid2pwl.py: reduce a parsed MIDI file (the
 * notes CMidiFile retains) onto the synth's 4 voices as a seq_ev_t
 * table in RAM, playable immediately and savable as .pwl (binary) or
 * .c (source, for compiling into firmware).
 *
 * The reduction follows the Python tool exactly - role mapping
 * (melody/bass/pad/drums) or the --satb 4-voice split, mono-reduce,
 * chord clustering, legato retriggers, velocity->amp - so a mix tuned
 * here and one generated on the host sound the same.  KEEP THE TWO IN
 * SYNC (instrument table included).
 ************************************************************************************/

#ifndef _PWL_TEST_TUI_MID2PWL_H
#define _PWL_TEST_TUI_MID2PWL_H

#include "MidiFile.h"

// Roles index cvt.inst[] / cvt.amp_*[]: 0 melody, 1 bass, 2 pad
#define M2P_ROLE_MEL   0
#define M2P_ROLE_BASS  1
#define M2P_ROLE_PAD   2

typedef struct
{
  const char *name;
  uint8_t  preset;
  uint8_t  detune;          // 0x80 = off, 0xFF = the role's default
  uint8_t  has_adsr, a, d, s, r;
  uint8_t  has_penv; int8_t p_off; uint8_t p_rate, p_rel;
  uint8_t  has_ts;   int8_t ts_delta; uint8_t ts_rate;
  uint8_t  has_vib;  uint8_t v_rate, v_cents; uint16_t v_delay;
  uint8_t  slur;            // legato chains slur (EV_SLUR) instead of
                            //   re-articulating - wind-instrument runs
} Mid2PwlInst_t;

int                  mid2pwl_inst_count(void);
const Mid2PwlInst_t *mid2pwl_inst(int idx);
int                  mid2pwl_inst_find(const char *name);
int                  mid2pwl_inst_default(int role);

// Build pMidi->m_Seq from pMidi->m_Notes + m_Cvt.  Returns the event
// count (>= 1, ends with EV_END) or -1 with err filled.
int  mid2pwl_convert(CMidiFile *pMidi, char *err, int errLen);

// Solo audition: one MIDI channel's line, rebased to its first note
// (leading rests skipped), on voice 2 with the given instrument -
// 'play ch2 [inst]', for deciding what a channel IS.  Ignores trim and
// the conversion's envelope overrides; returns the event count and a
// malloc'd table in *pSeq (caller frees), or -1 with err filled.
int  mid2pwl_audition(CMidiFile *pMidi, int chan, int instIdx,
                      seq_ev_t **pSeq, char *err, int errLen);

// Guess the role mapping from per-channel statistics (GM program,
// pitch, polyphony, density) and write it into m_Cvt.  The reasoning
// goes into report as one line per channel, '\n' separated, so the
// caller can show WHY.  Returns 0, or -1 when there is nothing to map.
int  mid2pwl_automap(CMidiFile *pMidi, char *report, int repLen);

// Write the conversion out; return bytes written or -1.
// .pwl: {'P','W','L','1', u32 count} + count seq_ev_t records.
long mid2pwl_save_pwl(const CMidiFile *pMidi, const char *path);
long mid2pwl_save_c(const CMidiFile *pMidi, const char *path);

#endif /* _PWL_TEST_TUI_MID2PWL_H */
