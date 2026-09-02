/************************************************************************************
 * tui/Mid2Pwl.cxx
 *
 * See Mid2Pwl.h.  Times are uint32 ms throughout (mid2pwl.py works in
 * float seconds; the comparisons port 1:1 with 0.01s -> 10ms etc).
 ************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Mid2Pwl.h"

extern "C" {
#include "../pwl_test.h"        // note_name() for the automap report
}

// g++'s freestanding <stdlib.h> shadows newlib's and omits qsort (same
// story as malloc in cxxrt.cpp); newlib's implementation links fine.
extern "C" void qsort(void *base, size_t nmemb, size_t size,
                      int (*compar)(const void *, const void *));


/*
==============================================================================
Instrument table.  KEEP IN SYNC with INSTRUMENTS/PRESET_NAMES in
tools/mid2pwl.py.  The plain presets carry no articulation and take the
role's default detune (melody 6, pad 12, bass off), exactly like the
Python tool; the named instruments are the hardware-verified recipes
from demos 14 and 15.
==============================================================================
*/

#define DET_ROLE 0xFF
#define DET_OFF  0x80

static const Mid2PwlInst_t s_Inst[] =
{
  //  name       pre det       adsr             penv          tslope    vib
  { "sine",      6, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "saw",       3, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "organ",     8, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "tri",       0, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "nes",       1, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "square",    2, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "pulse25",   4, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "pulse12",   5, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "clamp",     7, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "orion",    12, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "smooth",   13, DET_ROLE, 0,0,0,0,0,  0,0,0,0,  0,0,0,  0,0,0,0 },
  { "violin",   14, 6,        1,10,0,7,11, 1,-1,7,0, 1,64,9, 1,60,26,250 },
  { "panflute",  6, 6,        1,9,12,6,10, 1,-2,7,0, 0,0,0,  1,55,20,250 },
  { "bells",     0, 10,       1,0,14,0,13, 0,0,0,0,  1,40,9, 0,0,0,0 },
  // Struck string: hammer-instant attack, amp decays to SILENCE even
  // while held (s=0, ~2s), damper-felt release; the strike lands
  // bright and mellows in ~200ms (the fast spectral stage real pianos
  // have), on the PWL osc so the quiet tail stays clean; detune 3 =
  // the gentle unison-strings beat, tracking pitch like real unisons
  { "piano",    13, 3,        1,0,14,0,10, 0,0,0,0,  1,48,11, 0,0,0,0 },
  // Reed: the violin preset's pwm-skewed soft saw IS the reedy body,
  // but a sax is a solo voice (detune 2, not the violin's section 6)
  // that TONGUES notes (quick a7 speak, settles to s6 after the
  // accent), SCOOPS into pitch from 2 semis below (~90ms - the jazz
  // bend that makes it read as sax), and blooms a deep 5Hz vibrato
  // half a second into held notes only
  { "sax",      14, 2,        1,7,12,6,9,  1,-2,8,0, 1,32,11, 1,50,32,500,
                1 /* slur */ },
};
#define N_INST ((int)(sizeof(s_Inst) / sizeof(s_Inst[0])))

int mid2pwl_inst_count(void)                { return N_INST; }
const Mid2PwlInst_t *mid2pwl_inst(int idx)
{
  return (idx >= 0 && idx < N_INST) ? &s_Inst[idx] : &s_Inst[0];
}

int mid2pwl_inst_find(const char *name)
{
  for (int i = 0; i < N_INST; i++)
  {
    const char *a = s_Inst[i].name, *b = name;

    while (*a != 0 && *a == *b)         // byte-wise: newlib strcmp is
    {                                   //   unreliable here (see the
      a++;                              //   tcurses_vt100 note)
      b++;
    }
    if (*a == *b)
      return i;
  }
  return -1;
}

int mid2pwl_inst_default(int role)
{
  // mid2pwl.py --presets default "6,3,8" = sine, saw, organ
  return role == M2P_ROLE_BASS ? 1 : role == M2P_ROLE_PAD ? 2 : 0;
}

/*
==============================================================================
Reduction pipeline (ports of the Python functions of the same names)
==============================================================================
*/

typedef struct
{
  uint32_t on, off;
  uint8_t  note, vel;
  uint8_t  chan;                // source MIDI channel (chan_gain lookup)
  uint8_t  arp;                 // chord shape for the pad arpeggiator:
                                //   (i3 << 4) | i2 semitones above note
} Nt;

// GM percussion -> one-shot kind, or 0
static uint8_t drum_kind(int n)
{
  switch (n)
  {
    case 35: case 36: case 41: case 43: case 45: case 47: case 48: case 50:
      return EV_KICK;
    case 38: case 39: case 40: case 49: case 52: case 55: case 57:
      return EV_SNARE;
    case 37: case 42: case 44: case 46: case 51: case 53: case 54: case 56:
    case 58: case 59: case 60: case 61: case 62: case 63: case 64: case 69:
    case 70: case 75: case 76: case 80: case 81: case 82:
      return EV_HAT;
  }
  return 0;
}

// 'skip chN <bars>': is this onset inside a rested bar?  Bars are
// 1-based; negative indexes count from the song's end (-1 = last bar).
// The note is simply not collected - every other note keeps its
// absolute time, so the bar plays as rests instead of closing up.
int mid2pwl_skip_window(const CMidiFile *pMidi, int bar,
                        uint32_t *lo, uint32_t *hi);

// Resolve one 'skip' bar (negative = from the end) to its wall-clock
// window; returns the total bar count (0 = unresolvable)
int mid2pwl_skip_window(const CMidiFile *pMidi, int bar,
                        uint32_t *lo, uint32_t *hi)
{
  // A bar is num beats of 4/den quarters (6/8 = 3 quarters, NOT 6):
  // the denominator matters or every window is off by its ratio
  uint32_t num = pMidi->m_TimeSigNum ? pMidi->m_TimeSigNum : 4;
  uint32_t den = pMidi->m_TimeSigDen ? pMidi->m_TimeSigDen : 4;
  uint32_t barTicks = (uint32_t)pMidi->m_Division * num * 4u / den;

  if (barTicks == 0)
    barTicks = (uint32_t)pMidi->m_Division * 4u;

  // Negative bars count back from the LAST NOTE's bar, not the file's
  // padded tick span - MIDI files love a couple of silent bars at the
  // end, and "-1 = a bar of nothing" helps nobody
  uint32_t last_on = pMidi->m_NoteCount
      ? pMidi->m_Notes[pMidi->m_NoteCount - 1].on_ms : 0;
  int totalBars = 1;

  while (totalBars < 4096 &&
         pMidi->TickToMs((uint32_t)totalBars * barTicks) <= last_on)
    totalBars++;

  if (bar < 0)
    bar = totalBars + 1 + bar;
  if (bar < 1 || bar > totalBars)
  {
    *lo = *hi = 0;
    return totalBars;
  }
  *lo = pMidi->TickToMs((uint32_t)(bar - 1) * barTicks);
  *hi = pMidi->TickToMs((uint32_t)bar * barTicks);
  return totalBars;
}

// All of one MIDI channel's notes (already onset-sorted in m_Notes).
// Notes whose ONSET falls before the trim point are dropped and the
// survivors are rebased to it - the setup events sit at t=0, so without
// the rebase a trimmed song would open with the trim's length of dead
// air before the first note.
static int chan_notes(const CMidiFile *pMidi, int chan, Nt *out, int max)
{
  uint32_t trim = pMidi->BeatToMs(pMidi->m_Cvt.trim_beats);
  uint32_t sk_lo[8], sk_hi[8];
  int nSk = 0, n = 0;

  if (chan < 0)
    return 0;

  // Resolve the channel's rested-bar windows ONCE - resolving per note
  // walked the tempo map half a million times and read as a hang
  if (chan <= 15)
    for (int k = 0; k < pMidi->m_Cvt.skip_cnt[chan]; k++)
    {
      uint32_t lo, hi;

      mid2pwl_skip_window(pMidi, pMidi->m_Cvt.skip_bars[chan][k],
                          &lo, &hi);
      if (hi > lo)
      {
        sk_lo[nSk] = lo;
        sk_hi[nSk] = hi;
        nSk++;
      }
    }

  for (uint32_t i = 0; i < pMidi->m_NoteCount && n < max; i++)
  {
    const MidiNote_t *src = &pMidi->m_Notes[i];
    int rest = 0;

    for (int k = 0; k < nSk; k++)
      if (src->on_ms >= sk_lo[k] && src->on_ms < sk_hi[k])
      {
        rest = 1;
        break;
      }
    if (src->chan == chan && src->on_ms >= trim && !rest)
    {
      out[n].on = src->on_ms - trim;
      out[n].off = src->off_ms - trim;
      out[n].note = src->note;
      out[n].vel = src->vel;
      out[n].chan = (uint8_t)chan;
      out[n].arp = 0;
      n++;
    }
  }
  return n;
}

// Monophonic line: a new onset cuts the previous note; drop <= 10ms
static int mono_reduce(Nt *v, int n)
{
  int i, o = 0;

  for (i = 0; i < n; i++)
  {
    if (o > 0 && v[o - 1].off > v[i].on)
      v[o - 1].off = v[i].on;
    v[o++] = v[i];
  }
  n = o;
  o = 0;
  for (i = 0; i < n; i++)
    if (v[i].off - v[i].on > 10)
      v[o++] = v[i];
  return o;
}

static int nt_cmp(const void *a, const void *b)
{
  const Nt *x = (const Nt *)a, *y = (const Nt *)b;

  return x->on < y->on ? -1 : x->on > y->on ? 1 : 0;
}

// Later streams fill earlier ones' rests.  'have' is sorted and
// pairwise non-overlapping, so a binary search settles overlap.
static int merge_fill(Nt *have, int nHave, const Nt *extra, int nExtra,
                      int max)
{
  for (int i = 0; i < nExtra && nHave < max; i++)
  {
    const Nt *c = &extra[i];
    int lo = 0, hi = nHave;

    while (lo < hi)
    {
      int mid = (lo + hi) / 2;

      if (have[mid].on < c->on)
        lo = mid + 1;
      else
        hi = mid;
    }
    if ((lo > 0 && have[lo - 1].off > c->on) ||
        (lo < nHave && have[lo].on < c->off))
      continue;                         // overlaps something we keep
    memmove(&have[lo + 1], &have[lo], (nHave - lo) * sizeof(Nt));
    have[lo] = *c;
    nHave++;
  }
  return mono_reduce(have, nHave);
}

// Chord onsets within 60ms cluster to their root
static int cluster_chords(Nt *v, int n)
{
  int i = 0, o = 0;

  while (i < n)
  {
    int j = i + 1, nn = 0, k, m;
    Nt cl = v[i];
    uint8_t tone[8];

    tone[nn++] = v[i].note;
    while (j < n && v[j].on - v[i].on <= 60)
    {
      if (v[j].off > cl.off)
        cl.off = v[j].off;
      if (v[j].note < cl.note)
        cl.note = v[j].note;
      if (v[j].vel > cl.vel)
        cl.vel = v[j].vel;
      if (nn < 8)
        tone[nn++] = v[j].note;
      j++;
    }
    // chord shape for the arpeggiator: the two closest distinct tones
    // above the kept root, folded into 4-bit semitone intervals
    for (k = 0; k < nn; k++)
      for (m = k + 1; m < nn; m++)
        if (tone[m] < tone[k])
        {
          uint8_t t = tone[k];

          tone[k] = tone[m];
          tone[m] = t;
        }
    {
      int i2 = 0, i3 = 0;

      for (k = 0; k < nn; k++)
      {
        int iv = tone[k] - cl.note;

        if (iv > 15)
          iv -= 12;                     // wide voicing: octave-fold
        if (iv <= 0 || iv > 15 || iv == i2)
          continue;
        if (i2 == 0)
          i2 = iv;
        else if (i3 == 0)
        {
          i3 = iv;
          break;
        }
      }
      cl.arp = (uint8_t)((i3 << 4) | i2);
    }
    v[o++] = cl;
    i = j;
  }
  return mono_reduce(v, o);
}

static int fold_note(int note, int lo, int hi)
{
  while (note < lo)
    note += 12;
  while (note > hi)
    note -= 12;
  return note;
}

static int amp_of(int vel, int base, int span)
{
  int a = base + vel * span / 127;

  return a < 1 ? 1 : a > 63 ? 63 : a;
}

/*
==============================================================================
satb: split one polyphonic stream onto 4 mono voices (see the Python
docstring - skyline melody, bass floor, two inner voices, held-note
protection, loudest-first inners, the rest dropped)
==============================================================================
*/

static int satb_allocate(const Nt *notes, int n, const MidiCvt_t *cvt,
                         Nt *out[4], int nOut[4], int max, int *dropped)
{
  Nt held[4];
  uint8_t heldSet[4] = { 0, 0, 0, 0 };
  int i = 0;

  *dropped = 0;
  nOut[0] = nOut[1] = nOut[2] = nOut[3] = 0;

  while (i < n)
  {
    // one roll-window cluster
    int j = i + 1, k;
    uint8_t taken1 = 0, taken3 = 0;
    Nt cl[24];
    int nCl = 0;

    while (j < n && notes[j].on - notes[i].on <= cvt->roll_ms)
      j++;
    for (k = i; k < j && nCl < 24; k++)
      cl[nCl++] = notes[k];
    if (j - i > 24)
      *dropped += (j - i) - 24;

    uint32_t t0 = cl[0].on;

    // skyline -> melody (voice 2) unless a longer higher note holds
    int top = 0;

    for (k = 1; k < nCl; k++)
      if (cl[k].note > cl[top].note)
        top = k;
    if (cl[top].note >= cvt->mel_floor &&
        !(heldSet[2] && held[2].off > t0 + 50 && held[2].note > cl[top].note))
    {
      if (nOut[2] < max)
        out[2][nOut[2]++] = cl[top];
      held[2] = cl[top];
      heldSet[2] = 1;
      memmove(&cl[top], &cl[top + 1], (nCl - top - 1) * sizeof(Nt));
      nCl--;
    }

    // floor -> bass (voice 0)
    if (nCl > 0)
    {
      int bot = 0;

      for (k = 1; k < nCl; k++)
        if (cl[k].note < cl[bot].note)
          bot = k;
      if (cl[bot].note <= cvt->bass_ceil &&
          !(heldSet[0] && held[0].off > t0 + 50 &&
            held[0].note < cl[bot].note))
      {
        if (nOut[0] < max)
          out[0][nOut[0]++] = cl[bot];
        held[0] = cl[bot];
        heldSet[0] = 1;
        memmove(&cl[bot], &cl[bot + 1], (nCl - bot - 1) * sizeof(Nt));
        nCl--;
      }
    }

    // the rest, loudest first, into the free inner voices (1 and 3)
    for (k = 1; k < nCl; k++)           // insertion sort by -vel
    {
      Nt tmp = cl[k];
      int m = k - 1;

      while (m >= 0 && cl[m].vel < tmp.vel)
      {
        cl[m + 1] = cl[m];
        m--;
      }
      cl[m + 1] = tmp;
    }
    for (k = 0; k < nCl; k++)
    {
      int v = -1;

      if (!taken1 && !(heldSet[1] && held[1].off > cl[k].on))
        v = 1;
      else if (!taken3 && !(heldSet[3] && held[3].off > cl[k].on))
        v = 3;
      else if (!taken1 || !taken3)
      {
        // steal the older holder among the still-untaken voices
        if (!taken1 && !taken3)
          v = (!heldSet[1] || (heldSet[3] && held[1].on <= held[3].on))
              ? 1 : 3;
        else
          v = !taken1 ? 1 : 3;
      }
      if (v < 0)
      {
        (*dropped)++;
        continue;
      }
      if (nOut[v] < max)
        out[v][nOut[v]++] = cl[k];
      held[v] = cl[k];
      heldSet[v] = 1;
      if (v == 1)
        taken1 = 1;
      else
        taken3 = 1;
    }

    i = j;
  }

  for (i = 0; i < 4; i++)
  {
    qsort(out[i], nOut[i], sizeof(Nt), nt_cmp);
    nOut[i] = mono_reduce(out[i], nOut[i]);
  }
  return 0;
}

/*
==============================================================================
automap: guess which channel is which role.

No magic here - the same features a human reads off the track list,
reduced to integer scores:

  drums   channel 9 is drums (General MIDI says so)
  bass    bass-family program, low mean pitch, monophonic
  melody  lead/wind/solo-string program, high mean pitch, busy,
          monophonic, active across the whole piece
  pad     pad/ensemble/organ program, chords (overlapping notes),
          long notes, mid register

Each role scores every channel; a greedy matching takes the best
(role, channel) pair repeatedly so one channel cannot win two roles.
Leftover channels that still score like melodies join the melody
priority list (merge_fill plays them where the lead rests).

Special cases first: channels that duplicate another (same note count
and register - doubled parts) are dropped, and when what remains is a
single polyphonic performance (a solo piano), satb mode fits better
than the role mapping - exactly the call a human makes for gmlast.
==============================================================================
*/

typedef struct
{
  uint32_t notes, overlaps;
  uint32_t pitch_sum, dur_sum;
  uint32_t first_on, last_off;
  uint8_t  low, high;
  int      mean;            // pitch
  int      ovl_pct;         // % of notes overlapping the previous one
  int      prog;            // -1 unknown
  uint8_t  melodic;         // eligible for a role
  int8_t   dup_of;          // -1, or the channel this one doubles
} ChanStat_t;

static void chan_stats(const CMidiFile *pMidi, ChanStat_t *cs)
{
  uint32_t last_off[16] = { 0 };
  uint32_t i;
  int c;

  memset(cs, 0, sizeof(ChanStat_t) * 16);
  for (c = 0; c < 16; c++)
  {
    cs[c].prog = pMidi->m_ChanProg[c] == 0xFF ? -1 : pMidi->m_ChanProg[c];
    cs[c].dup_of = -1;
    cs[c].first_on = 0xFFFFFFFFu;
  }

  for (i = 0; i < pMidi->m_NoteCount; i++)
  {
    const MidiNote_t *n = &pMidi->m_Notes[i];
    ChanStat_t *s = &cs[n->chan];

    s->notes++;
    s->pitch_sum += n->note;
    s->dur_sum += n->off_ms - n->on_ms;
    if (n->on_ms < last_off[n->chan])
      s->overlaps++;                    // chordal / legato overlap
    if (n->off_ms > last_off[n->chan])
      last_off[n->chan] = n->off_ms;
    if (n->on_ms < s->first_on)
      s->first_on = n->on_ms;
    if (n->off_ms > s->last_off)
      s->last_off = n->off_ms;
    if (s->low == 0 || n->note < s->low)
      s->low = n->note;
    if (n->note > s->high)
      s->high = n->note;
  }

  for (c = 0; c < 16; c++)
  {
    if (cs[c].notes == 0)
      continue;
    cs[c].mean = (int)(cs[c].pitch_sum / cs[c].notes);
    cs[c].ovl_pct = (int)(cs[c].overlaps * 100 / cs[c].notes);
    cs[c].melodic = (c != 9 && cs[c].notes >= 8);
  }
}

static int prior_melody(int p)
{
  if (p < 0)
    return 0;
  if (p >= 80 && p <= 87)  return 80;   // synth leads
  if (p >= 72 && p <= 79)  return 70;   // pipes / flutes
  if (p >= 64 && p <= 71)  return 60;   // reeds
  if (p >= 40 && p <= 42)  return 60;   // violin / viola / cello
  if (p >= 56 && p <= 63)  return 50;   // brass
  if (p >= 8 && p <= 15)   return 30;   // chromatic percussion
  if (p >= 24 && p <= 31)  return 25;   // guitars
  if (p >= 0 && p <= 7)    return 25;   // pianos
  if (p >= 88 && p <= 95)  return -40;  // pads: not a lead
  if (p >= 32 && p <= 39)  return -60;  // basses: not a lead
  return 0;
}

static int prior_bass(int p)
{
  if (p < 0)
    return 0;
  if (p >= 32 && p <= 39)  return 100;  // bass family
  if (p == 43)             return 60;   // contrabass
  if (p == 58)             return 50;   // tuba
  return 0;
}

static int prior_pad(int p)
{
  if (p < 0)
    return 0;
  if (p >= 88 && p <= 95)  return 90;   // synth pads
  if (p >= 48 && p <= 55)  return 70;   // ensembles / choirs
  if (p >= 16 && p <= 23)  return 50;   // organs
  if (p == 44)             return 50;   // tremolo strings
  if (p >= 0 && p <= 7)    return 30;   // piano comping
  if (p >= 24 && p <= 31)  return 30;   // guitar comping
  return 0;
}

/*
==============================================================================
Instrument selection: GM program family -> the nearest thing this synth
does well, per role.  The reasoning behind the table:

  - plucky families (piano, guitar) sound right through the fake-decay
    amp sweep, so they map to plain presets ("smooth") whose notes decay
  - struck-metal families map to "bells" (instant attack, hardware ring)
  - pipes map to "panflute", solo/ensemble strings to "violin" - the
    articulated recipes built from those very sounds
  - reeds are pulse-like (odd harmonics), brass and synth leads saw-like
  - synth pads get "orion" (the shimmer), ensembles/choirs get "organ"
    (the 1x+2x stack reads as an ensemble)

NULL = no opinion, keep the role's current instrument.
==============================================================================
*/

static const char *gm_inst_melody(int p)
{
  if (p < 0)               return NULL;
  if (p == 14)             return "bells";      // tubular bells
  if (p >= 40 && p <= 55)  return "violin";     // strings, solo or section
  if (p >= 72 && p <= 79)  return "panflute";   // pipes
  if (p >= 64 && p <= 71)  return "pulse25";    // reeds
  if (p >= 56 && p <= 63)  return "saw";        // brass
  if (p == 80)             return "square";     // square lead
  if (p >= 81 && p <= 87)  return "saw";        // other synth leads
  if (p >= 8 && p <= 15)   return "bells";      // chromatic percussion
  if (p >= 16 && p <= 23)  return "organ";
  if (p >= 0 && p <= 7)    return "smooth";     // piano: decaying pluck
  if (p >= 24 && p <= 31)  return "smooth";     // guitar: likewise
  return NULL;
}

static const char *gm_inst_bass(int p)
{
  if (p == 38 || p == 39)  return "square";     // synth bass
  return "saw";                                 // every other bass: saw
}

static const char *gm_inst_pad(int p)
{
  if (p < 0)               return NULL;
  if (p == 14)             return "bells";
  if (p >= 8 && p <= 15)   return "bells";
  if (p >= 88 && p <= 95)  return "orion";      // synth pads
  if (p >= 48 && p <= 55)  return "organ";      // ensembles / choirs
  if (p >= 16 && p <= 23)  return "organ";
  return NULL;
}

static void pick_inst(MidiCvt_t *cvt, int role, const char *name)
{
  int idx;

  if (name != NULL && (idx = mid2pwl_inst_find(name)) >= 0)
    cvt->inst[role] = (uint8_t)idx;
}

/*
==============================================================================
Envelope and amp fitting.  The hand-tuned adsr/amp flags reduce to
per-channel statistics:

  attack   sustainer programs with long notes swell in; struck ones hit
  decay    for a decayer, the rate whose full 63-step sweep matches the
           median hold time - the note fades over how long it is held
  sustain  sustainers hold (organ physics), decayers fall away
  release  the median SILENCE between notes: real gaps earn a tail that
           fills them, touching notes need a short one or pitches smear
  amp      a linear fit of velocity -> amplitude so this channel's loud
           notes (p90 velocity) land on the role's mix budget and its
           soft notes (p10) on the role's floor - a file with flat
           velocities automatically gets a narrow span
==============================================================================
*/

typedef struct
{
  int      vel_p10, vel_p90;
  uint32_t dur_med, gap_med;    // ms
} RoleProf_t;

static uint32_t hist_median(const uint16_t *h, int bins, uint32_t total,
                            uint32_t bucket_ms)
{
  uint32_t cum = 0;
  int i;

  for (i = 0; i < bins; i++)
  {
    cum += h[i];
    if (cum * 2 >= total)
      break;
  }
  return (uint32_t)i * bucket_ms + bucket_ms / 2;
}

static void role_profile(const CMidiFile *pMidi, int chan, RoleProf_t *pf)
{
  uint16_t vh[128], dh[64], gh[64];
  uint32_t n = 0, ng = 0, cum, i;
  uint32_t prev_on = 0, prev_dur = 0;
  bool have_prev = false;

  memset(vh, 0, sizeof(vh));
  memset(dh, 0, sizeof(dh));
  memset(gh, 0, sizeof(gh));
  pf->vel_p10 = 40;
  pf->vel_p90 = 100;
  pf->dur_med = 300;
  pf->gap_med = 100;

  for (i = 0; i < pMidi->m_NoteCount; i++)
  {
    const MidiNote_t *nt = &pMidi->m_Notes[i];
    uint32_t dur;

    if (nt->chan != chan)
      continue;
    dur = nt->off_ms - nt->on_ms;
    vh[nt->vel & 127]++;
    dh[dur / 50 > 63 ? 63 : dur / 50]++;
    if (have_prev)
    {
      // silence between the previous note's end and this onset
      uint32_t ioi = nt->on_ms - prev_on;
      uint32_t gap = ioi > prev_dur ? ioi - prev_dur : 0;

      gh[gap / 50 > 63 ? 63 : gap / 50]++;
      ng++;
    }
    prev_on = nt->on_ms;
    prev_dur = dur;
    have_prev = true;
    n++;
  }
  if (n == 0)
    return;

  for (i = 0, cum = 0; i < 128; i++)
  {
    cum += vh[i];
    if (cum * 10 >= n)
    {
      pf->vel_p10 = (int)i;
      break;
    }
  }
  for (i = 0, cum = 0; i < 128; i++)
  {
    cum += vh[i];
    if (cum * 10 >= n * 9)
    {
      pf->vel_p90 = (int)i;
      break;
    }
  }
  pf->dur_med = hist_median(dh, 64, n, 50);
  if (ng > 0)
    pf->gap_med = hist_median(gh, 64, ng, 50);
}

// Sustainers keep sounding while held (bowed/blown/driven); the rest
// are struck or plucked and decay on their own
static bool gm_sustainer(int p)
{
  return p >= 16 && ((p >= 16 && p <= 23) || (p >= 40 && p <= 95));
}

// Smallest sweep rate whose full 63-step swing lasts at least ms
static int fit_decay_rate(uint32_t ms)
{
  for (int r = 5; r <= 15; r++)
    if (63u * (2u << r) / 1000u >= ms)
      return r;
  return 15;
}

static void derive_role(MidiCvt_t *cvt, int role, int prog,
                        const RoleProf_t *pf)
{
  static const int peak[3] = { 58, 44, 28 };    // MEL, BASS, PAD budgets
  static const int flor[3] = { 30, 26, 14 };
  int8_t *ov = cvt->adsr_ovr[role];
  int vspan = pf->vel_p90 - pf->vel_p10;
  int span, base;
  bool sus = gm_sustainer(prog);

  // ---- amp: velocity p90 -> the role's peak, p10 -> its floor
  if (vspan >= 8)
  {
    span = (peak[role] - flor[role]) * 127 / vspan;
    if (span > 34)
      span = 34;
  }
  else
    span = 10;                          // flat velocities: nearly constant
  base = peak[role] - pf->vel_p90 * span / 127;
  if (base < 2)
    base = 2;
  cvt->amp_base[role] = (uint8_t)base;
  cvt->amp_span[role] = (uint8_t)(span < 6 ? 6 : span);

  // ---- envelope: only where the instrument brings none of its own
  if (mid2pwl_inst(cvt->inst[role])->has_adsr)
  {
    ov[0] = -1;
    return;
  }
  switch (role)
  {
    case M2P_ROLE_MEL:
      if (sus)
      {
        ov[0] = 7;
        ov[1] = 12;
        ov[2] = 4;
        ov[3] = (int8_t)(pf->gap_med > 150 ? 11 : 8);
      }
      else
      {
        ov[0] = 0;
        ov[1] = (int8_t)fit_decay_rate(pf->dur_med ? pf->dur_med : 400);
        ov[2] = 1;
        ov[3] = 6;
      }
      break;
    case M2P_ROLE_BASS:
      ov[0] = 1;
      if (sus)
      {
        ov[1] = 0;
        ov[2] = 6;
        ov[3] = 4;
      }
      else
      {
        ov[1] = 11;
        ov[2] = 4;
        ov[3] = 1;                      // tight: bass tails turn to mud
      }
      break;
    default:                            // PAD
      ov[0] = (int8_t)(pf->dur_med > 800 ? 11 : pf->dur_med > 400 ? 9 : 7);
      ov[1] = 0;
      ov[2] = 7;
      ov[3] = 12;
      break;
  }
}

int mid2pwl_automap(CMidiFile *pMidi, char *report, int repLen)
{
  ChanStat_t cs[16];
  MidiCvt_t *cvt = &pMidi->m_Cvt;
  int score[3][16];                     // [role][chan]: MEL / BASS / PAD
  int8_t role_of[16];                   // -1 free, else M2P_ROLE_*
  uint32_t span, total_notes = 0;
  int rp = 0, c, r, i, n_melodic = 0, busiest = -1;

  if (pMidi->m_NoteCount == 0)
  {
    snprintf(report, repLen, "no notes to map");
    return -1;
  }

  chan_stats(pMidi, cs);
  span = pMidi->TickToMs(pMidi->m_MaxTicks);
  if (span == 0)
    span = 1;

  // Duplicate detection: doubled parts (same count, same register)
  for (c = 0; c < 16; c++)
  {
    if (!cs[c].melodic)
      continue;
    total_notes += cs[c].notes;
    n_melodic++;
    if (busiest < 0 || cs[c].notes > cs[busiest].notes)
      busiest = c;
    for (i = 0; i < c; i++)
    {
      if (!cs[i].melodic || cs[i].dup_of >= 0)
        continue;
      uint32_t d = cs[c].notes > cs[i].notes ? cs[c].notes - cs[i].notes
                                             : cs[i].notes - cs[c].notes;

      if (d * 20 <= cs[i].notes &&
          (cs[c].mean > cs[i].mean ? cs[c].mean - cs[i].mean
                                   : cs[i].mean - cs[c].mean) <= 1)
      {
        cs[c].dup_of = (int8_t)i;
        cs[c].melodic = 0;
        n_melodic--;
        total_notes -= cs[c].notes;
        break;
      }
    }
  }

  // Reset the mapping (mix gains included); drums = ch 9 when it plays
  for (i = 0; i < MIDI_MEL_SRCS; i++)
    cvt->melody[i] = -1;
  for (c = 0; c < 16; c++)
  {
    cvt->chan_gain[c] = 100;
    cvt->chan_inst[c] = -1;
  }
  cvt->bass = cvt->pad = cvt->satb = -1;
  cvt->drums = (int8_t)(cs[9].notes > 0 ? 9 : -1);
  for (c = 0; c < 16; c++)
    role_of[c] = -1;

  // A single polyphonic performance is a satb split, not a role map
  if (n_melodic == 1 ||
      (busiest >= 0 && cs[busiest].notes * 100 >= total_notes * 60 &&
       cs[busiest].ovl_pct >= 35))
  {
    for (c = 0; c < 16; c++)
      if (cs[c].melodic && (n_melodic == 1 || c == busiest))
      {
        cvt->satb = (int8_t)c;
        break;
      }
  }

  if (cvt->satb < 0)
  {
    // Score every melodic channel for every role
    for (c = 0; c < 16; c++)
    {
      score[0][c] = score[1][c] = score[2][c] = -1000;
      if (!cs[c].melodic)
        continue;

      int mono = 100 - cs[c].ovl_pct;
      int cover = (int)((uint64_t)(cs[c].last_off - cs[c].first_on) * 100
                        / span);
      int density = (int)(cs[c].notes * 60 / (total_notes ? total_notes : 1));
      int mean_dur = (int)(cs[c].dur_sum / cs[c].notes);

      score[M2P_ROLE_MEL][c] = prior_melody(cs[c].prog)
          + (cs[c].mean > 55 ? (cs[c].mean - 55 > 25 ? 25 : cs[c].mean - 55)
                             : (cs[c].mean - 55)) * 2
          + mono / 2
          + (density > 50 ? 50 : density)
          + cover / 4;
      score[M2P_ROLE_BASS][c] = prior_bass(cs[c].prog)
          + (cs[c].mean < 60 ? (60 - cs[c].mean > 30 ? 30 : 60 - cs[c].mean)
                             : (60 - cs[c].mean)) * 3
          + mono / 2;
      score[M2P_ROLE_PAD][c] = prior_pad(cs[c].prog)
          + (cs[c].ovl_pct > 60 ? 60 : cs[c].ovl_pct)
          + (mean_dur / 40 > 40 ? 40 : mean_dur / 40)
          + (cs[c].mean >= 48 && cs[c].mean <= 72 ? 20 : 0);
    }

    // Greedy matching: repeatedly take the best (role, channel) pair
    for (r = 0; r < 3; r++)
    {
      int bestRole = -1, bestChan = -1, best = 49;    // floor: score >= 50

      for (i = 0; i < 3; i++)
      {
        bool taken = (i == M2P_ROLE_MEL && cvt->melody[0] >= 0) ||
                     (i == M2P_ROLE_BASS && cvt->bass >= 0) ||
                     (i == M2P_ROLE_PAD && cvt->pad >= 0);

        if (taken)
          continue;
        for (c = 0; c < 16; c++)
          if (role_of[c] < 0 && score[i][c] > best)
          {
            best = score[i][c];
            bestRole = i;
            bestChan = c;
          }
      }
      if (bestRole < 0)
        break;
      role_of[bestChan] = (int8_t)bestRole;
      if (bestRole == M2P_ROLE_MEL)
        cvt->melody[0] = (int8_t)bestChan;
      else if (bestRole == M2P_ROLE_BASS)
        cvt->bass = (int8_t)bestChan;
      else
        cvt->pad = (int8_t)bestChan;
    }

    // Leftovers that still read as melodies fill the lead's rests
    if (cvt->melody[0] >= 0)
    {
      int lead = score[M2P_ROLE_MEL][(int)cvt->melody[0]];
      int slot = 1;

      for (c = 0; c < 16 && slot < MIDI_MEL_SRCS; c++)
        if (cs[c].melodic && role_of[c] < 0 &&
            score[M2P_ROLE_MEL][c] * 10 >= lead * 7)
        {
          cvt->melody[slot++] = (int8_t)c;
          role_of[c] = M2P_ROLE_MEL;
        }
    }
  }

  // Instruments from the winners' GM programs, then envelopes and amp
  // ranges fitted to each winner's note statistics.  An explicit guess
  // command replaces what is there; 'inst'/cfg edits overrule after.
  if (cvt->satb >= 0)
  {
    int p = cs[(int)cvt->satb].prog;
    RoleProf_t pf;

    pick_inst(cvt, M2P_ROLE_MEL, gm_inst_melody(p));
    pick_inst(cvt, M2P_ROLE_BASS, "saw");
    pick_inst(cvt, M2P_ROLE_PAD, gm_inst_pad(p) != NULL ? gm_inst_pad(p)
                                                        : "organ");
    role_profile(pMidi, cvt->satb, &pf);
    derive_role(cvt, M2P_ROLE_MEL, p, &pf);
    derive_role(cvt, M2P_ROLE_BASS, p, &pf);
    derive_role(cvt, M2P_ROLE_PAD, p, &pf);
  }
  else
  {
    static const int8_t *chan_of[3] = { NULL, NULL, NULL };
    RoleProf_t pf;

    (void)chan_of;
    if (cvt->melody[0] >= 0)
    {
      pick_inst(cvt, M2P_ROLE_MEL, gm_inst_melody(cs[(int)cvt->melody[0]].prog));
      role_profile(pMidi, cvt->melody[0], &pf);
      derive_role(cvt, M2P_ROLE_MEL, cs[(int)cvt->melody[0]].prog, &pf);
    }
    if (cvt->bass >= 0)
    {
      pick_inst(cvt, M2P_ROLE_BASS, gm_inst_bass(cs[(int)cvt->bass].prog));
      role_profile(pMidi, cvt->bass, &pf);
      derive_role(cvt, M2P_ROLE_BASS, cs[(int)cvt->bass].prog, &pf);
    }
    if (cvt->pad >= 0)
    {
      pick_inst(cvt, M2P_ROLE_PAD, gm_inst_pad(cs[(int)cvt->pad].prog));
      role_profile(pMidi, cvt->pad, &pf);
      derive_role(cvt, M2P_ROLE_PAD, cs[(int)cvt->pad].prog, &pf);
    }
  }

  // ---- the reasoning, one line per sounding channel
  for (c = 0; c < 16 && rp < repLen - 8; c++)
  {
    const char *verdict;
    char extra[28];

    if (cs[c].notes == 0)
      continue;

    extra[0] = 0;
    if (c == 9)
      verdict = cvt->drums == 9 ? "drums" : "drums (unused)";
    else if (cs[c].dup_of >= 0)
    {
      snprintf(extra, sizeof(extra), "doubles ch%d", cs[c].dup_of);
      verdict = extra;
    }
    else if (cvt->satb == c)
    {
      snprintf(extra, sizeof(extra), "satb, %s lead",
               mid2pwl_inst(cvt->inst[M2P_ROLE_MEL])->name);
      verdict = extra;
    }
    else if (role_of[c] == M2P_ROLE_MEL)
    {
      if (cvt->melody[0] != c)
        verdict = "melody (fills)";
      else
      {
        snprintf(extra, sizeof(extra), "melody = %s",
                 mid2pwl_inst(cvt->inst[M2P_ROLE_MEL])->name);
        verdict = extra;
      }
    }
    else if (role_of[c] == M2P_ROLE_BASS)
    {
      snprintf(extra, sizeof(extra), "bass = %s",
               mid2pwl_inst(cvt->inst[M2P_ROLE_BASS])->name);
      verdict = extra;
    }
    else if (role_of[c] == M2P_ROLE_PAD)
    {
      snprintf(extra, sizeof(extra), "pad = %s",
               mid2pwl_inst(cvt->inst[M2P_ROLE_PAD])->name);
      verdict = extra;
    }
    else if (!cs[c].melodic)
      verdict = "(too sparse)";
    else
      verdict = "(unused)";

    rp += snprintf(&report[rp], repLen - rp,
                   "ch%-2d %5lun %-3s poly%-3d %-18.18s -> %s\n",
                   c, (unsigned long)cs[c].notes,
                   c == 9 ? "-" : note_name(cs[c].mean), cs[c].ovl_pct,
                   c == 9 ? "GM percussion"
                          : (cs[c].prog >= 0 ? MidiGmName(cs[c].prog)
                                             : "(no program)"),
                   verdict);
  }
  // Fitted mix and envelopes, one line each
  if (rp < repLen - 8)
    rp += snprintf(&report[rp], repLen - rp,
                   "amp: mel=%u+%u bass=%u+%u pad=%u+%u\n",
                   cvt->amp_base[0], cvt->amp_span[0], cvt->amp_base[1],
                   cvt->amp_span[1], cvt->amp_base[2], cvt->amp_span[2]);
  if (rp < repLen - 8)
  {
    static const char *const rn[3] = { "mel", "bass", "pad" };
    int r2;

    rp += snprintf(&report[rp], repLen - rp, "adsr:");
    for (r2 = 0; r2 < 3; r2++)
    {
      if (cvt->adsr_ovr[r2][0] >= 0)
        rp += snprintf(&report[rp], repLen - rp, " %s=%d/%d/%d/%d", rn[r2],
                       cvt->adsr_ovr[r2][0], cvt->adsr_ovr[r2][1],
                       cvt->adsr_ovr[r2][2], cvt->adsr_ovr[r2][3]);
      else
        rp += snprintf(&report[rp], repLen - rp, " %s=(%s's own)", rn[r2],
                       mid2pwl_inst(cvt->inst[r2])->name);
    }
    rp += snprintf(&report[rp], repLen - rp, "\n");
  }

  if (rp > 0 && report[rp - 1] == '\n')
    report[rp - 1] = 0;

  return (cvt->satb >= 0 || cvt->melody[0] >= 0 || cvt->bass >= 0 ||
          cvt->pad >= 0 || cvt->drums >= 0) ? 0 : -1;
}

/*
==============================================================================
Event assembly
==============================================================================
*/

typedef struct
{
  uint32_t t;               // ms
  uint16_t order;           // same-tick determinism (control < notes)
  uint32_t idx;             // insertion index: stable sort tie-break
  uint8_t  cmd, ch, a, b;
} Ev_t;

typedef struct
{
  Ev_t    *v;
  int      n, max;
  uint32_t idx;
} EvBuf_t;

static void ev_push(EvBuf_t *eb, uint32_t t, int order, int cmd, int ch,
                    int a, int b)
{
  if (eb->n >= eb->max)
  {
    int nmax = eb->max * 2;
    Ev_t *nv = (Ev_t *)realloc(eb->v, nmax * sizeof(Ev_t));

    if (nv == NULL)
      return;                           // full: silently drops (checked later)
    eb->v = nv;
    eb->max = nmax;
  }
  eb->v[eb->n].t = t;
  eb->v[eb->n].order = (uint16_t)order;
  eb->v[eb->n].idx = eb->idx++;
  eb->v[eb->n].cmd = (uint8_t)cmd;
  eb->v[eb->n].ch = (uint8_t)ch;
  eb->v[eb->n].a = (uint8_t)a;
  eb->v[eb->n].b = (uint8_t)b;
  eb->n++;
}

static int ev_cmp(const void *a, const void *b)
{
  const Ev_t *x = (const Ev_t *)a, *y = (const Ev_t *)b;

  if (x->t != y->t)
    return x->t < y->t ? -1 : 1;
  if (x->order != y->order)
    return (int)x->order - (int)y->order;
  return x->idx < y->idx ? -1 : 1;
}

// Sorted events -> a seq table: dt deltas from a running accumulator,
// 65535ms NOP splits for long gaps, closing EV_END.  Rebases to the
// first event's time.  Returns the count, or -1 (out of memory).
static int evbuf_to_seq(const EvBuf_t *eb, seq_ev_t **out)
{
  seq_ev_t *seq = (seq_ev_t *)malloc((eb->n + 64) * sizeof(seq_ev_t));
  uint32_t t0 = eb->n > 0 ? eb->v[0].t : 0, emitted = 0;
  int i, o = 0, cap = eb->n + 64;

  *out = NULL;
  if (seq == NULL)
    return -1;
  for (i = 0; i < eb->n; i++)
  {
    uint32_t target = eb->v[i].t - t0;
    uint32_t dt = target - emitted;

    while (dt > 65535 && o < cap - 2)
    {
      seq[o].dt_ms = 65535;
      seq[o].cmd = EV_NOP;
      seq[o].ch = seq[o].a = seq[o].b = 0;
      o++;
      dt -= 65535;
      emitted += 65535;
    }
    if (o >= cap - 2)
    {
      cap += 256;
      seq_ev_t *ns = (seq_ev_t *)realloc(seq, cap * sizeof(seq_ev_t));

      if (ns == NULL)
        break;
      seq = ns;
    }
    emitted = target;
    seq[o].dt_ms = (uint16_t)dt;
    seq[o].cmd = eb->v[i].cmd;
    seq[o].ch = eb->v[i].ch;
    seq[o].a = eb->v[i].a;
    seq[o].b = eb->v[i].b;
    o++;
  }
  seq[o].dt_ms = 1500;
  seq[o].cmd = EV_END;
  seq[o].ch = seq[o].a = seq[o].b = 0;
  o++;
  *out = seq;
  return o;
}

// Mid-line instrument change ('inst ch5 violin'): fully reprogram the
// voice right before the note lands, clearing whatever the previous
// instrument had armed - each subsystem is set or explicitly off.
// useOvr keeps the role's ADSR override when falling BACK to the role
// instrument; a per-channel assignment plays its own envelope.
static void emit_inst_sw(EvBuf_t *eb, uint32_t t, int order, int ch,
                         int instIdx, int role, const MidiCvt_t *cvt,
                         int useOvr)
{
  const Mid2PwlInst_t *in = mid2pwl_inst(instIdx);
  static const uint8_t roleDet[3] = { 6, DET_OFF, 12 };
  int det = (in->detune == DET_ROLE) ? roleDet[role] : in->detune;
  const int8_t *ovr = cvt->adsr_ovr[role];
  int ovrOn = useOvr && ovr[0] >= 0;

  ev_push(eb, t, order, EV_PRESET, ch, in->preset, 0);
  ev_push(eb, t, order, EV_DETUNE, ch, det, 0);
  if (ovrOn)
    ev_push(eb, t, order, EV_ADSR, ch, (ovr[0] << 4) | ovr[1],
            (ovr[2] << 4) | ovr[3]);
  else if (in->has_adsr)
    ev_push(eb, t, order, EV_ADSR, ch, (in->a << 4) | in->d,
            (in->s << 4) | in->r);
  else
    ev_push(eb, t, order, EV_ADSR, ch, 0xFF, 0);
  ev_push(eb, t, order, EV_PENV, ch,
          in->has_penv ? (in->p_off & 0xFF) : 0,
          in->has_penv ? ((in->p_rel << 4) | in->p_rate) : 0);
  ev_push(eb, t, order, EV_TSLOPE, ch,
          in->has_ts ? (in->ts_delta & 0xFF) : 0,
          in->has_ts ? ((3 << 4) | in->ts_rate) : 0);
  ev_push(eb, t, order, EV_VIBRATO, ch,
          in->has_vib ? in->v_rate : 0,
          in->has_vib ? (((in->v_cents / 2) & 63) |
                         ((in->v_delay / 250 > 3 ? 3
                                                 : in->v_delay / 250) << 6))
                      : 0);
  if (!in->has_adsr && !ovrOn &&
      (role == M2P_ROLE_MEL || role == M2P_ROLE_BASS))
    // fake decay via amp sweep for envelope-less voices
    ev_push(eb, t, order, EV_SWEEP_PA, ch,
            role == M2P_ROLE_MEL ? 14 : 13, 0);
  else
    ev_push(eb, t, order, EV_SWEEP_PA, ch, 0, 0);
}

// One melodic line -> EV_ON / EV_OFF with legato retriggers.  ci is
// the per-MIDI-channel instrument map (cvt->chan_inst), or NULL when
// the caller has already fixed the voice's instrument (audition).
static void emit_line(EvBuf_t *eb, const Nt *line, int n, int ch, int order,
                      const MidiCvt_t *cvt, int role, int lo, int hi,
                      const int8_t *ci, int baseInst)
{
  int cur = -1;                         // -1 = the role's instrument
  int linked = 0, prevNote = -1;        // legato chain state for slurs
  int prevArp = -1;                     // programmed arp intervals

  for (int i = 0; i < n; i++)
  {
    int switched = 0;

    // a note from an instrument-assigned MIDI channel reprograms the
    // voice; leaving that channel restores the role instrument
    if (ci != NULL)
    {
      int want = ci[line[i].chan & 15];

      if (want != cur)
      {
        emit_inst_sw(eb, line[i].on, order, ch,
                     want >= 0 ? want : cvt->inst[role], role, cvt,
                     want < 0);
        cur = want;
        switched = 1;                   // new sound: re-articulate
      }
    }
    int note = line[i].note + cvt->transpose;

    if (lo > 0)
      note = fold_note(note, lo, hi);
    note = note < 11 ? 11 : note > 106 ? 106 : note;

    // per-source-channel mix ('amp ch7 50'): notes keep their MIDI
    // channel through the reduction precisely for this
    int amp = amp_of(line[i].vel, cvt->amp_base[role], cvt->amp_span[role])
              * cvt->chan_gain[line[i].chan & 15] / 100;

    // Pad arpeggiator ('cset arp <ms>'): program this chord's shape
    // before the gate; one voice then cycles the whole chord
    if (cvt->arp_ms && role == M2P_ROLE_PAD && line[i].arp != prevArp)
    {
      ev_push(eb, line[i].on, order, EV_ARPIV, ch,
              line[i].arp & 15, (line[i].arp >> 4) & 15);
      prevArp = line[i].arp;
    }

    // Within a legato chain a slurring instrument (sax) retunes the
    // breathing note instead of tonguing it again - the phrase's
    // scoop, strike and envelope all ride through the run.  Repeated
    // pitches and instrument switches always re-articulate.
    {
      int active = (ci != NULL && cur >= 0) ? cur : baseInst;

      if (linked && !switched && note != prevNote &&
          mid2pwl_inst(active)->slur)
        ev_push(eb, line[i].on, order, EV_SLUR, ch, note,
                amp < 1 ? 1 : amp > 63 ? 63 : amp);
      else
        ev_push(eb, line[i].on, order, EV_ON, ch, note,
                amp < 1 ? 1 : amp > 63 ? 63 : amp);
    }
    prevNote = note;

    uint32_t gap = (i + 1 < n) ? line[i + 1].on - line[i].off : 0xFFFFFFFFu;

    if (line[i + 1 < n ? i + 1 : i].on < line[i].off && i + 1 < n)
      gap = 0;                          // overlap: retrigger, no off
    if (gap > cvt->legato_ms)
    {
      ev_push(eb, line[i].off, order, EV_OFF, ch, 0, 0);
      linked = 0;
    }
    else
      linked = 1;
  }
}

// The instrument setup block for one voice (order 0: before everything)
static void emit_inst(EvBuf_t *eb, int ch, int instIdx, int role,
                      const MidiCvt_t *cvt)
{
  const Mid2PwlInst_t *in = mid2pwl_inst(instIdx);
  static const uint8_t roleDet[3] = { 6, DET_OFF, 12 };
  int det = (in->detune == DET_ROLE) ? roleDet[role] : in->detune;

  const int8_t *ovr = cvt->adsr_ovr[role];

  ev_push(eb, 0, 0, EV_PRESET, ch, in->preset, 0);
  ev_push(eb, 0, 0, EV_DETUNE, ch, det, 0);
  if (ovr[0] >= 0)
    // per-role envelope override (automap's data-fitted ADSR, or a
    // hand-edited cfg) - wins over the instrument's own
    ev_push(eb, 0, 0, EV_ADSR, ch, (ovr[0] << 4) | ovr[1],
            (ovr[2] << 4) | ovr[3]);
  else if (in->has_adsr)
    ev_push(eb, 0, 0, EV_ADSR, ch, (in->a << 4) | in->d,
            (in->s << 4) | in->r);
  if (in->has_penv)
    ev_push(eb, 0, 0, EV_PENV, ch, in->p_off & 0xFF,
            (in->p_rel << 4) | in->p_rate);
  if (in->has_ts)
    ev_push(eb, 0, 0, EV_TSLOPE, ch, in->ts_delta & 0xFF,
            (3 << 4) | in->ts_rate);
  if (in->has_vib)
    ev_push(eb, 0, 0, EV_VIBRATO, ch, in->v_rate,
            ((in->v_cents / 2) & 63) |
            ((in->v_delay / 250 > 3 ? 3 : in->v_delay / 250) << 6));
  if (!in->has_adsr && ovr[0] < 0)
  {
    // fake decay via amp sweep for envelope-less melody/bass (python
    // emits r14 melody, r13 bass; pad stays flat)
    if (role == M2P_ROLE_MEL)
      ev_push(eb, 0, 0, EV_SWEEP_PA, ch, 14, 0);
    else if (role == M2P_ROLE_BASS)
      ev_push(eb, 0, 0, EV_SWEEP_PA, ch, 13, 0);
  }
}

int mid2pwl_convert(CMidiFile *pMidi, char *err, int errLen)
{
  MidiCvt_t *cvt = &pMidi->m_Cvt;
  Nt *pool = NULL, *mel, *tmp;
  EvBuf_t eb = { NULL, 0, 0, 0 };
  int max, i, nMel = 0, satb = cvt->satb >= 0;

  free(pMidi->m_Seq);
  pMidi->m_Seq = NULL;
  pMidi->m_SeqCount = 0;
  pMidi->m_Saved = 0;

  if (pMidi->m_NoteCount == 0)
  {
    snprintf(err, errLen, "no notes retained (reopen the file?)");
    return -1;
  }
  if (!satb && cvt->melody[0] < 0 && cvt->bass < 0 && cvt->pad < 0)
  {
    snprintf(err, errLen, "nothing mapped: 'map melody <ch>' first"
                          " (or 'map satb <ch>')");
    return -1;
  }

  max = (int)pMidi->m_NoteCount + 8;
  pool = (Nt *)malloc((size_t)max * 3 * sizeof(Nt));
  eb.max = 2 * (int)pMidi->m_NoteCount + 96;
  eb.v = (Ev_t *)malloc(eb.max * sizeof(Ev_t));
  if (pool == NULL || eb.v == NULL)
  {
    free(pool);
    free(eb.v);
    snprintf(err, errLen, "out of memory");
    return -1;
  }
  mel = pool;
  tmp = pool + max;

  if (satb)
  {
    // one polyphonic channel onto all four voices
    Nt *voice[4];
    int nV[4], dropped = 0, n;

    for (i = 0; i < 4; i++)
      voice[i] = (Nt *)malloc((size_t)max * sizeof(Nt));
    if (voice[0] == NULL || voice[1] == NULL || voice[2] == NULL ||
        voice[3] == NULL)
    {
      for (i = 0; i < 4; i++)
        free(voice[i]);
      free(pool);
      free(eb.v);
      snprintf(err, errLen, "out of memory");
      return -1;
    }

    n = chan_notes(pMidi, cvt->satb, mel, max);
    satb_allocate(mel, n, cvt, voice, nV, max, &dropped);

    emit_inst(&eb, 2, cvt->inst[M2P_ROLE_MEL], M2P_ROLE_MEL, cvt);
    emit_inst(&eb, 0, cvt->inst[M2P_ROLE_BASS], M2P_ROLE_BASS, cvt);
    emit_inst(&eb, 1, cvt->inst[M2P_ROLE_PAD], M2P_ROLE_PAD, cvt);
    emit_inst(&eb, 3, cvt->inst[M2P_ROLE_PAD], M2P_ROLE_PAD, cvt);
    if (cvt->arp_ms)
    {
      ev_push(&eb, 0, 0, EV_ARPRATE, 1, cvt->arp_ms, 0);
      ev_push(&eb, 0, 0, EV_ARPRATE, 3, cvt->arp_ms, 0);
    }

    emit_line(&eb, voice[0], nV[0], 0, 2, cvt, M2P_ROLE_BASS, 0, 0,
              cvt->chan_inst, cvt->inst[M2P_ROLE_BASS]);
    emit_line(&eb, voice[1], nV[1], 1, 3, cvt, M2P_ROLE_PAD, 0, 0,
              cvt->chan_inst, cvt->inst[M2P_ROLE_PAD]);
    emit_line(&eb, voice[3], nV[3], 3, 3, cvt, M2P_ROLE_PAD, 0, 0,
              cvt->chan_inst, cvt->inst[M2P_ROLE_PAD]);
    emit_line(&eb, voice[2], nV[2], 2, 4, cvt, M2P_ROLE_MEL, 0, 0,
              cvt->chan_inst, cvt->inst[M2P_ROLE_MEL]);

    for (i = 0; i < 4; i++)
      free(voice[i]);
  }
  else
  {
    int n;

    // melody: priority-merged sources
    for (i = 0; i < MIDI_MEL_SRCS && cvt->melody[i] >= 0; i++)
    {
      n = chan_notes(pMidi, cvt->melody[i], tmp, max);
      n = mono_reduce(tmp, n);
      if (i == 0)
      {
        memcpy(mel, tmp, n * sizeof(Nt));
        nMel = n;
      }
      else
        nMel = merge_fill(mel, nMel, tmp, n, max);
    }

    emit_inst(&eb, 2, cvt->inst[M2P_ROLE_MEL], M2P_ROLE_MEL, cvt);
    emit_inst(&eb, 0, cvt->inst[M2P_ROLE_BASS], M2P_ROLE_BASS, cvt);
    emit_inst(&eb, 1, cvt->inst[M2P_ROLE_PAD], M2P_ROLE_PAD, cvt);
    if (cvt->arp_ms)
      ev_push(&eb, 0, 0, EV_ARPRATE, 1, cvt->arp_ms, 0);

    if (cvt->bass >= 0)
    {
      n = mono_reduce(tmp, chan_notes(pMidi, cvt->bass, tmp, max));
      emit_line(&eb, tmp, n, 0, 2, cvt, M2P_ROLE_BASS, 0, 0,
                cvt->chan_inst, cvt->inst[M2P_ROLE_BASS]);
    }
    if (cvt->pad >= 0)
    {
      n = cluster_chords(tmp, chan_notes(pMidi, cvt->pad, tmp, max));
      emit_line(&eb, tmp, n, 1, 3, cvt, M2P_ROLE_PAD,
                cvt->pad_lo, cvt->pad_hi, cvt->chan_inst,
                cvt->inst[M2P_ROLE_PAD]);
    }
    emit_line(&eb, mel, nMel, 2, 4, cvt, M2P_ROLE_MEL, 0, 0,
              cvt->chan_inst, cvt->inst[M2P_ROLE_MEL]);

    if (cvt->drums >= 0)
    {
      n = chan_notes(pMidi, cvt->drums, tmp, max);
      for (i = 0; i < n; i++)
      {
        uint8_t kind = drum_kind(tmp[i].note);

        if (kind != 0)
          ev_push(&eb, tmp[i].on, 1, kind, 3, 0, 0);
      }
    }
  }

  free(pool);

  if (eb.n < 1)
  {
    free(eb.v);
    snprintf(err, errLen, "the mapping produced no events");
    return -1;
  }

  qsort(eb.v, eb.n, sizeof(Ev_t), ev_cmp);

  {
    seq_ev_t *seq;
    int o = evbuf_to_seq(&eb, &seq);

    free(eb.v);
    if (o < 0)
    {
      snprintf(err, errLen, "out of memory");
      return -1;
    }
    pMidi->m_Seq = seq;
    pMidi->m_SeqCount = (uint32_t)o;
  }
  return (int)pMidi->m_SeqCount;
}

/*
==============================================================================
Solo audition: one channel, rebased to its first onset, on voice 2
==============================================================================
*/

int mid2pwl_audition(CMidiFile *pMidi, int chan, int instIdx,
                     seq_ev_t **pSeq, char *err, int errLen)
{
  MidiCvt_t *cvt = &pMidi->m_Cvt;
  EvBuf_t eb = { NULL, 0, 0, 0 };
  Nt *notes;
  int i, n, o, max = (int)pMidi->m_NoteCount + 8;
  uint16_t savedTrim = cvt->trim_beats;
  int8_t   savedOvr = cvt->adsr_ovr[M2P_ROLE_MEL][0];
  seq_ev_t *seq;

  *pSeq = NULL;
  if (chan < 0 || chan > 15)
  {
    snprintf(err, errLen, "channels are 0-15");
    return -1;
  }

  notes = (Nt *)malloc((size_t)max * sizeof(Nt));
  eb.max = 2 * max + 32;
  eb.v = (Ev_t *)malloc(eb.max * sizeof(Ev_t));
  if (notes == NULL || eb.v == NULL)
  {
    free(notes);
    free(eb.v);
    snprintf(err, errLen, "out of memory");
    return -1;
  }

  // The whole channel, trim ignored: the point is to hear what it IS
  cvt->trim_beats = 0;
  n = chan_notes(pMidi, chan, notes, max);
  cvt->trim_beats = savedTrim;
  if (n == 0)
  {
    free(notes);
    free(eb.v);
    snprintf(err, errLen, "channel %d has no notes", chan);
    return -1;
  }
  n = mono_reduce(notes, n);

  // Start at the first note: strip the rest it keeps relative to the
  // other channels
  {
    uint32_t t0 = notes[0].on;

    for (i = 0; i < n; i++)
    {
      notes[i].on -= t0;
      notes[i].off -= t0;
    }
  }

  // The chosen instrument's own sound: no per-role envelope override
  cvt->adsr_ovr[M2P_ROLE_MEL][0] = -1;
  emit_inst(&eb, 2, instIdx, M2P_ROLE_MEL, cvt);
  cvt->adsr_ovr[M2P_ROLE_MEL][0] = savedOvr;
  emit_line(&eb, notes, n, 2, 2, cvt, M2P_ROLE_MEL, 0, 0, NULL, instIdx);
  free(notes);

  o = evbuf_to_seq(&eb, &seq);
  free(eb.v);
  if (o < 0)
  {
    snprintf(err, errLen, "out of memory");
    return -1;
  }
  *pSeq = seq;
  return o;
}

/*
==============================================================================
Savers
==============================================================================
*/

long mid2pwl_save_pwl(const CMidiFile *pMidi, const char *path)
{
  FILE *f;
  uint32_t count = pMidi->m_SeqCount;
  long total;

  if (pMidi->m_Seq == NULL || count == 0)
    return -1;
  if ((f = fopen(path, "w")) == NULL)
    return -1;

  fwrite("PWL1", 1, 4, f);
  fwrite(&count, 4, 1, f);
  fwrite(pMidi->m_Seq, sizeof(seq_ev_t), count, f);
  total = 8 + (long)count * (long)sizeof(seq_ev_t);
  fclose(f);
  return total;
}

// seq.h's ev_cmd_t, for readable .c output
static const char *const s_CmdNames[] =
{
  "EV_END", "EV_ON", "EV_OFF", "EV_PRESET", "EV_SWEEP_PA", "EV_SWEEP_WS",
  "EV_DETUNE", "EV_KICK", "EV_SNARE", "EV_HAT", "EV_PERIOD", "EV_AMP",
  "EV_VIB", "EV_NOP", "EV_ADSR", "EV_PENV", "EV_TPWM", "EV_TSLOPE",
  "EV_VIBRATO", "EV_SLUR", "EV_ARPIV", "EV_ARPRATE"
};
#define N_CMD_NAMES ((int)(sizeof(s_CmdNames) / sizeof(s_CmdNames[0])))

long mid2pwl_save_c(const CMidiFile *pMidi, const char *path)
{
  const MidiCvt_t *cvt = &pMidi->m_Cvt;
  char name[40];
  FILE *f;
  uint32_t i;
  int n;

  if (pMidi->m_Seq == NULL || pMidi->m_SeqCount == 0)
    return -1;
  if ((f = fopen(path, "w")) == NULL)
    return -1;

  // table name: song_<basename> with unfriendly chars mapped to '_'
  n = snprintf(name, sizeof(name), "song_%s", pMidi->m_Title);
  if (n > 4 && name[n - 4] == '.' && name[n - 3] == 'm' &&
      name[n - 2] == 'i' && name[n - 1] == 'd')
    name[n - 4] = 0;
  for (char *p = name; *p != 0; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9')))
      *p = '_';

  fprintf(f, "// '%s' - converted on TinyQV by pwl-test (tui 'save')\n",
          name);
  fprintf(f, "//   from %s\n", pMidi->m_Path);
  if (cvt->satb >= 0)
    fprintf(f, "// satb split of MIDI ch %d, roll %ums\n", cvt->satb,
            cvt->roll_ms);
  else
  {
    fprintf(f, "// melody ch %d  bass ch %d  pad ch %d  drums ch %d\n",
            cvt->melody[0], cvt->bass, cvt->pad, cvt->drums);
  }
  fprintf(f, "// inst: mel=%s bass=%s pad=%s  transpose %d  legato %ums\n",
          mid2pwl_inst(cvt->inst[0])->name, mid2pwl_inst(cvt->inst[1])->name,
          mid2pwl_inst(cvt->inst[2])->name, cvt->transpose, cvt->legato_ms);
  if (cvt->trim_beats > 0)
    fprintf(f, "// trimmed: first %u beats\n", cvt->trim_beats);
  fprintf(f, "#include \"seq.h\"\n\n");
  fprintf(f, "const seq_ev_t %s[] = {\n", name);
  for (i = 0; i < pMidi->m_SeqCount; i++)
  {
    const seq_ev_t *e = &pMidi->m_Seq[i];
    const char *cmd = e->cmd < N_CMD_NAMES ? s_CmdNames[e->cmd] : "0";

    fprintf(f, "    { %u, %s, %u, %u, %u },\n", e->dt_ms, cmd, e->ch,
            e->a, e->b);
  }
  fprintf(f, "};\n");

  long total = ftell(f);

  fclose(f);
  return total;
}
