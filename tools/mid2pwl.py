#!/usr/bin/env python3
"""Convert a MIDI file to a pwl-test sequencer table (seq.h format).

Reduces a multi-track General MIDI file onto the PWL synth's 4 hardware
channels:

    pwl ch0  bass    one MIDI channel, monophonic (last-on wins)
    pwl ch1  pad     one MIDI channel; chords cluster to their ROOT note,
                     octave-folded into --pad-lo..--pad-hi; the 'organ'
                     preset's 1x+2x frequency multipliers add the octave
                     back, so one voice carries the chord bed
    pwl ch2  melody  one or more MIDI channels in priority order
                     (later channels fill only where earlier ones rest)
    pwl ch3  drums   GM percussion mapped to EV_KICK / EV_SNARE / EV_HAT

Note-offs become EV_OFF only when the gap to the next note exceeds
--legato ms; shorter gaps retrigger directly (period changes are
click-free on this synth).  Velocities map linearly onto per-voice
amp ranges.  dt values use a running-time accumulator (no drift).

--adsr-melody/--adsr-bass/--adsr-pad A,D,S,R put a real amplitude
envelope on that voice (EV_ADSR; rates use the sweep scale, sustain
0-7).  Velocity then sets the attack peak, EV_OFF starts the release
tail, and the legacy amp-sweep "fake decay" is skipped for that
channel.  Voices without a spec keep the old flat behavior.

--satb CH replaces the role mapping entirely: it splits ONE polyphonic
channel (e.g. a solo piano performance) onto all four synth voices --
skyline melody (pwl ch2), bass floor (ch0), and TWO inner voices
(ch1 + ch3, normally the drum channel).  Rolled chords are clustered
(--roll ms); a held melody/bass note is protected from being stolen
by inner notes crossing it; leftover notes beyond 4 voices are
dropped loudest-last (counts reported).  --adsr-pad / --amp-pad /
the PAD preset apply to both inner voices in this mode.

Usage:
  mid2pwl.py <file.mid> <table_name> --bass CH --pad CH --melody CH[,CH..]
             [--drums CH] [--legato MS] [--pad-lo N --pad-hi N]
             [--presets MEL,BASS,PAD] [--adsr-melody A,D,S,R]
             [--adsr-bass A,D,S,R] [--adsr-pad A,D,S,R]
             [--transpose N] [-o out.c]
  mid2pwl.py <file.mid> <table_name> --satb CH [--roll MS]
             [--mel-floor N] [--bass-ceil N] [common flags as above]

Needs mido (the faceswap conda python has it).
"""
import argparse
import shlex
import sys
import textwrap

import mido

PRESET_NAMES = ["tri", "nes", "square", "saw", "pulse25", "pulse12",
                "sine", "clamp", "organ", "power", "third", "noise",
                "orion", "smooth", "violin"]

# Named instruments: a preset plus the articulation that makes it read
# as the real thing - ADSR, pitch slide-in, timbre envelope (bow bite /
# bell strike) and vibrato LFO.  These are the hardware-verified recipes
# from the pwl-test demos (14 flute+bells, 15 violin).
#
# KEEP IN SYNC with s_Instruments in pwl-test/tui/Mid2Pwl.cxx - the
# on-target converter uses the same table.
#
#   adsr   (a, d, s, r)         rates 0-15, sustain 0-7
#   penv   (off, rate, rel)     slide in from off semitones at rate
#   tslope (delta, rate)        slopes start +delta, sweep home (dir=both)
#   vib    (rate_dhz, cents, delay_ms)
INSTRUMENTS = {
    "violin":   dict(preset=14, detune=6,  adsr=(10, 0, 7, 11),
                     penv=(-1, 7, 0), tslope=(64, 9), vib=(60, 26, 250)),
    "panflute": dict(preset=6,  detune=6,  adsr=(9, 12, 6, 10),
                     penv=(-2, 7, 0), tslope=None,    vib=(55, 20, 250)),
    "bells":    dict(preset=0,  detune=10, adsr=(0, 14, 0, 13),
                     penv=None,      tslope=(40, 9),  vib=None),
    # struck string: instant attack, decay-to-silence while held,
    # damper release, bright strike mellowing fast, unison-beat detune
    "piano":    dict(preset=13, detune=3,  adsr=(0, 14, 0, 10),
                     penv=None,      tslope=(48, 11), vib=None),
    # reed: tongued attack settling after the accent, 2-semi jazz scoop
    # into every note, deep 5Hz vibrato blooming 500ms into held notes
    "sax":      dict(preset=14, detune=2,  adsr=(7, 12, 6, 9),
                     penv=(-2, 8, 0), tslope=(32, 11), vib=(50, 32, 500),
                     slur=True),
}


def vib_b(cents, delay_ms):
    """EV_VIBRATO b field: depth in 2-cent units + 2-bit delay code."""
    return ((cents // 2) & 63) | (min(delay_ms // 250, 3) << 6)

# GM percussion -> one-shot kind
DRUM_MAP = {}
for n in (35, 36, 41, 43, 45, 47, 48, 50):          # kicks + toms
    DRUM_MAP[n] = "EV_KICK"
for n in (38, 39, 40, 49, 52, 55, 57):              # snares, claps, crashes
    DRUM_MAP[n] = "EV_SNARE"
for n in (37, 42, 44, 46, 51, 53, 54, 56, 58, 59,   # hats, rides, perc
          60, 61, 62, 63, 64, 69, 70, 75, 76, 80, 81, 82):
    DRUM_MAP[n] = "EV_HAT"


def collect_notes(mid):
    """{channel: [(t_on, t_off, note, vel, chan)]} in absolute seconds.

    Each note keeps its source channel so per-channel gain (--gain)
    still knows where a note came from after melody sources merge."""
    open_notes = {}
    notes = {}
    t = 0.0
    for msg in mid:                    # merged tracks, delta in seconds
        t += msg.time
        if msg.type == 'note_on' and msg.velocity > 0:
            open_notes.setdefault((msg.channel, msg.note), []).append(
                (t, msg.velocity))
        elif msg.type in ('note_off', 'note_on'):
            key = (msg.channel, msg.note)
            if open_notes.get(key):
                t_on, vel = open_notes[key].pop(0)
                notes.setdefault(msg.channel, []).append(
                    (t_on, t, msg.note, vel, msg.channel))
    for (ch, note), pend in open_notes.items():     # never released
        for t_on, vel in pend:
            notes.setdefault(ch, []).append((t_on, t, note, vel, ch))
    for ch in notes:
        notes[ch].sort()
    return notes


def mono_reduce(notes):
    """Monophonic line: a new onset cuts the previous note (last wins)."""
    out = []
    for n in notes:
        if out and out[-1][1] > n[0]:
            prev = out[-1]
            out[-1] = prev[:1] + (n[0],) + prev[2:]
        out.append(tuple(n))
    return [n for n in out if n[1] - n[0] > 0.01]


def merge_priority(streams):
    """Combine melody sources; later streams fill earlier ones' rests."""
    result = list(streams[0])
    for extra in streams[1:]:
        for cand in extra:
            if not any(c[0] < cand[1] and cand[0] < c[1] for c in result):
                result.append(cand)
    result.sort()
    return mono_reduce(result)


def chord_shape(group, root):
    """Arp intervals: the two closest distinct tones above the root,
    octave-folded into 4 bits each -> (i3 << 4) | i2 (0 = no chord)."""
    i2 = i3 = 0
    for tone in sorted(g[2] for g in group):
        iv = tone - root
        if iv > 15:
            iv -= 12
        if iv <= 0 or iv > 15 or iv == i2:
            continue
        if i2 == 0:
            i2 = iv
        elif i3 == 0:
            i3 = iv
            break
    return (i3 << 4) | i2


def cluster_chords(notes, window=0.06):
    """Group simultaneous onsets; returns [(t_on, t_off, root, vel,
    src, arp_shape)]."""
    out = []
    group = []
    for n in notes:
        if group and n[0] - group[0][0] > window:
            t_on = group[0][0]
            t_off = max(g[1] for g in group)
            root = min(g[2] for g in group)
            vel = max(g[3] for g in group)
            out.append((t_on, t_off, root, vel, group[0][4],
                        chord_shape(group, root)))
            group = []
        group.append(n)
    if group:
        root = min(g[2] for g in group)
        out.append((group[0][0], max(g[1] for g in group),
                    root, max(g[3] for g in group), group[0][4],
                    chord_shape(group, root)))
    return mono_reduce(out)


def fold(note, lo, hi):
    while note < lo:
        note += 12
    while note > hi:
        note -= 12
    return note


def amp_of(vel, base, span):
    return max(1, min(63, base + vel * span // 127))


def satb_allocate(notes, roll, mel_floor, bass_ceil):
    """Split one polyphonic stream onto 4 mono voices.

    Returns {pwl_ch: [(t_on, t_off, note, vel, chan)]} for ch2 melody
    (skyline), ch0 bass (floor), ch1+ch3 inner.  Onsets within `roll`
    seconds form one chord cluster; each cluster's top note goes to
    the melody and its bottom to the bass UNLESS a longer note from an
    earlier cluster still sounds beyond it (held-note protection --
    an arpeggio may not steal a sustained melody or bass note).
    Remaining notes fill the two inner voices loudest-first, one note
    per voice per cluster; the rest are dropped and counted.
    """
    clusters, cur = [], []
    for n in notes:
        if cur and n[0] - cur[0][0] > roll:
            clusters.append(cur)
            cur = []
        cur.append(n)
    if cur:
        clusters.append(cur)

    out = {0: [], 1: [], 2: [], 3: []}
    held = {v: None for v in out}
    dropped = 0
    for cl in clusters:
        t0 = cl[0][0]
        rest = sorted(cl, key=lambda n: -n[2])
        top = rest[0]
        hm = held[2]
        if top[2] >= mel_floor and not (hm and hm[1] > t0 + 0.05
                                        and hm[2] > top[2]):
            out[2].append(top)
            held[2] = top
            rest.remove(top)
        if rest:
            bot = min(rest, key=lambda n: n[2])
            hb = held[0]
            if bot[2] <= bass_ceil and not (hb and hb[1] > t0 + 0.05
                                            and hb[2] < bot[2]):
                out[0].append(bot)
                held[0] = bot
                rest.remove(bot)
        rest.sort(key=lambda n: -n[3])
        taken = set()
        for n in rest:
            free = [v for v in (1, 3) if v not in taken
                    and not (held[v] and held[v][1] > n[0])]
            if free:
                v = free[0]
            else:
                cand = [v for v in (1, 3) if v not in taken]
                if not cand:
                    dropped += 1
                    continue
                v = min(cand, key=lambda v: held[v][0])
            out[v].append(n)
            held[v] = n
            taken.add(v)
    for v in out:
        out[v] = mono_reduce(sorted(out[v]))
    return out, dropped


def main():
    p = argparse.ArgumentParser()
    p.add_argument("input")
    p.add_argument("name")
    p.add_argument("--bass", type=int)
    p.add_argument("--pad", type=int)
    p.add_argument("--melody",
                   help="channel list, priority order (e.g. 0 or 0,12,13)")
    p.add_argument("--satb", type=int, default=None,
                   help="split this one polyphonic channel onto all 4 "
                        "voices (melody/bass/2 inners) instead of the "
                        "role mapping")
    p.add_argument("--roll", type=int, default=280,
                   help="satb: chord-roll cluster window ms")
    p.add_argument("--mel-floor", type=int, default=60,
                   help="satb: lowest pitch the melody voice may take")
    p.add_argument("--bass-ceil", type=int, default=55,
                   help="satb: highest pitch the bass voice may take")
    p.add_argument("--drums", type=int, default=9)
    p.add_argument("--perc", default="",
                   help="extra percussion channels CH:kick|snare|hat[,..] "
                        "(pitched percolator tracks -> one-shots)")
    p.add_argument("--legato", type=int, default=120)
    p.add_argument("--arp", type=int, default=0, metavar="MS",
                   help="pad arpeggiator: cycle chord tones every MS ms "
                        "on the one pad voice (0 = off; the TUI's "
                        "'cset arp')")
    p.add_argument("--pad-lo", type=int, default=45)
    p.add_argument("--pad-hi", type=int, default=59)
    p.add_argument("--presets", default="6,3,8",
                   help="melody,bass,pad preset indexes (default sine,saw,organ)")
    p.add_argument("--transpose", type=int, default=0)
    p.add_argument("--amp-melody", default="30,22", help="base,span (vel->amp)")
    p.add_argument("--gain", default="",
                   help="per-MIDI-channel volume CH:PCT[,CH:PCT..] "
                        "(7:50 = ch7 at half volume wherever its notes "
                        "land; the TUI's 'amp ch7 50')")
    p.add_argument("--amp-bass", default="24,18")
    p.add_argument("--amp-pad", default="18,14")
    p.add_argument("--adsr-melody", default="", help="A,D,S,R envelope "
                   "(rates 0-15 on the sweep scale, sustain 0-7)")
    p.add_argument("--adsr-bass", default="")
    p.add_argument("--adsr-pad", default="")
    p.add_argument("--penv-melody", default="", help="pitch envelope "
                   "OFFSET,RATE[,RELRATE]: notes slide in from OFFSET "
                   "semitones away; RELRATE adds a falling release slide")
    p.add_argument("--penv-bass", default="")
    p.add_argument("--penv-pad", default="")
    p.add_argument("--inst-melody", default="", help="named instrument "
                   f"bundle ({'/'.join(INSTRUMENTS)}): sets the preset, "
                   "detune, ADSR, pitch/timbre envelopes and vibrato in "
                   "one flag; explicit --adsr-*/--penv-* still override")
    p.add_argument("--inst-bass", default="")
    p.add_argument("--inst-pad", default="")
    p.add_argument("-o", "--output")
    args = p.parse_args()

    def parse_inst(name, who):
        if not name:
            return None
        if name not in INSTRUMENTS:
            sys.exit(f"--inst-{who}: unknown instrument '{name}' "
                     f"(have {', '.join(INSTRUMENTS)})")
        return INSTRUMENTS[name]

    # pwl channel -> instrument bundle (pad's covers inner2 in satb mode)
    inst = {2: parse_inst(args.inst_melody, "melody"),
            0: parse_inst(args.inst_bass, "bass"),
            1: parse_inst(args.inst_pad, "pad")}
    inst[3] = inst[1] if args.satb is not None else None
    inst_name = {2: args.inst_melody, 0: args.inst_bass,
                 1: args.inst_pad, 3: args.inst_pad}

    gain = {}
    for spec in (args.gain.split(",") if args.gain else []):
        ch, pct = spec.split(":")
        gain[int(ch)] = int(pct)

    amp_m = tuple(int(x) for x in args.amp_melody.split(","))
    amp_b = tuple(int(x) for x in args.amp_bass.split(","))
    amp_p = tuple(int(x) for x in args.amp_pad.split(","))

    def parse_adsr(spec, who):
        if not spec:
            return None
        a, d, s, r = (int(x) for x in spec.split(","))
        if not (0 <= a <= 15 and 0 <= d <= 15 and 0 <= s <= 7
                and 0 <= r <= 15):
            sys.exit(f"--adsr-{who}: rates 0-15, sustain 0-7")
        return (a, d, s, r)

    # pwl channel -> (a,d,s,r); melody=ch2, bass=ch0, pad/inner=ch1(+ch3)
    adsr = {2: parse_adsr(args.adsr_melody, "melody"),
            0: parse_adsr(args.adsr_bass, "bass"),
            1: parse_adsr(args.adsr_pad, "pad")}
    adsr[3] = adsr[1] if args.satb is not None else None
    for ch in adsr:
        if adsr[ch] is None and inst[ch] is not None:
            adsr[ch] = inst[ch]["adsr"]

    def parse_penv(spec, who):
        if not spec:
            return None
        parts = [int(x) for x in spec.split(",")]
        if len(parts) == 2:
            parts.append(0)
        off, rate, rel = parts
        if not (-64 <= off <= 63 and 0 <= rate <= 15 and 0 <= rel <= 15):
            sys.exit(f"--penv-{who}: OFFSET,RATE[,RELRATE] "
                     "(offset +-semitones, rates 0-15)")
        return (off, rate, rel)

    penv = {2: parse_penv(args.penv_melody, "melody"),
            0: parse_penv(args.penv_bass, "bass"),
            1: parse_penv(args.penv_pad, "pad")}
    penv[3] = penv[1] if args.satb is not None else None
    for ch in penv:
        if penv[ch] is None and inst[ch] is not None:
            penv[ch] = inst[ch]["penv"]

    if args.satb is None and (args.bass is None or args.pad is None
                              or args.melody is None):
        sys.exit("--bass/--pad/--melody are required unless --satb is used")

    mid = mido.MidiFile(args.input)
    notes = collect_notes(mid)
    mel_chans = ([int(c) for c in args.melody.split(",")]
                 if args.melody else [])
    pm, pb, pp = (int(x) for x in args.presets.split(","))
    if inst[2] is not None:
        pm = inst[2]["preset"]
    if inst[0] is not None:
        pb = inst[0]["preset"]
    if inst[1] is not None:
        pp = inst[1]["preset"]
    perc = {}
    if args.perc:
        for spec in args.perc.split(","):
            ch, kind = spec.split(":")
            perc[int(ch)] = "EV_" + kind.upper()

    if args.satb is not None:
        used = {args.satb}
    else:
        used = set(mel_chans) | {args.bass, args.pad, args.drums} | set(perc)
    for ch in sorted(notes):
        if ch not in used:
            rng = (min(n[2] for n in notes[ch]), max(n[2] for n in notes[ch]))
            print(f"  (dropped MIDI ch{ch}: {len(notes[ch])} notes, "
                  f"range {rng[0]}-{rng[1]})", file=sys.stderr)

    if args.satb is not None:
        voices, n_drop = satb_allocate(notes.get(args.satb, []),
                                       args.roll / 1000.0,
                                       args.mel_floor, args.bass_ceil)
        melody, bass = voices[2], voices[0]
        inner1, inner2 = voices[1], voices[3]
        pad, drums = [], []
        if n_drop:
            print(f"  (satb: {n_drop} notes beyond 4 voices dropped)",
                  file=sys.stderr)
    else:
        melody = merge_priority([mono_reduce(notes.get(c, []))
                                 for c in mel_chans])
        bass = mono_reduce(notes.get(args.bass, []))
        pad = cluster_chords(notes.get(args.pad, []))
        drums = notes.get(args.drums, [])
        inner1 = inner2 = None

    # (time, order, "line") -- order keeps same-tick events deterministic:
    # control first, then drums / bass / pad / melody
    evs = []

    def emit(t, order, cmd, ch, a=0, b=0):
        evs.append((t, order, cmd, ch, a, b))

    def emit_line(line, ch, order, base, span, lo=None, hi=None,
                  transpose=0, slur=False, arp=False):
        linked, prev_note = False, None
        prev_arp = None
        for i, ev5 in enumerate(line):
            t_on, t_off, note, vel, src = ev5[:5]
            if arp and args.arp:
                shape = ev5[5] if len(ev5) > 5 else 0
                if shape != prev_arp:
                    emit(t_on, order, "EV_ARPIV", ch, shape & 15,
                         (shape >> 4) & 15)
                    prev_arp = shape
            note += transpose
            if lo is not None:
                note = fold(note, lo, hi)
            note = max(11, min(106, note))
            amp = amp_of(vel, base, span) * gain.get(src, 100) // 100
            # a slurring instrument (sax) retunes through legato runs
            # instead of re-articulating every note
            op = "EV_SLUR" if (slur and linked and note != prev_note)                  else "EV_ON"
            emit(t_on, order, op, ch, note, max(1, min(63, amp)))
            prev_note = note
            gap = (line[i + 1][0] - t_off) if i + 1 < len(line) else 1e9
            linked = gap * 1000 <= args.legato
            if not linked:
                emit(t_off, order, "EV_OFF", ch)

    emit_line(bass, 0, 2, amp_b[0], amp_b[1], transpose=args.transpose)
    if args.satb is not None:
        emit_line(inner1, 1, 3, amp_p[0], amp_p[1], transpose=args.transpose)
        emit_line(inner2, 3, 3, amp_p[0], amp_p[1], transpose=args.transpose)
    else:
        if args.arp:
            emit(0.0, 0, "EV_ARPRATE", 1, args.arp)
        emit_line(pad, 1, 3, amp_p[0], amp_p[1], lo=args.pad_lo,
                  hi=args.pad_hi, transpose=args.transpose, arp=True)
    emit_line(melody, 2, 4, amp_m[0], amp_m[1], transpose=args.transpose,
              slur=INSTRUMENTS.get(args.inst_melody, {}).get("slur", False))

    unmapped = {}
    for t_on, _, note, vel, _ in drums:
        kind = DRUM_MAP.get(note)
        if kind:
            emit(t_on, 1, kind, 3)
        else:
            unmapped[note] = unmapped.get(note, 0) + 1
    if unmapped:
        print(f"  (unmapped drum notes: {unmapped})", file=sys.stderr)
    for ch, kind in perc.items():
        for t_on, _, note, vel, _ in notes.get(ch, []):
            emit(t_on, 1, kind, 3)

    evs.sort(key=lambda e: (e[0], e[1]))
    t0 = evs[0][0] if evs else 0.0

    # ---- emit the C table ----
    out = args.output or (args.name + ".c")
    lines = []
    lines.append(f"// '{args.name}' - generated by tools/mid2pwl.py from")
    lines.append(f"//   {args.input}")
    cmd = " ".join([sys.argv[0]] + [shlex.quote(a) for a in sys.argv[1:]])
    lines.append("// command:")
    lines.extend(textwrap.wrap(cmd, width=74, initial_indent="//   ",
                               subsequent_indent="//     "))
    if args.satb is not None:
        lines.append(f"// satb split of MIDI ch {args.satb}: melody=pwl2 "
                     f"(skyline >={args.mel_floor}), bass=pwl0 "
                     f"(<={args.bass_ceil}), inner=pwl1+pwl3, "
                     f"roll {args.roll}ms")
        lines.append(f"// {len(melody)} melody / {len(bass)} bass / "
                     f"{len(inner1)}+{len(inner2)} inner")
    else:
        lines.append(f"// melody: MIDI ch {mel_chans}  bass: ch {args.bass}  "
                     f"pad: ch {args.pad} (root-folded "
                     f"{args.pad_lo}-{args.pad_hi})  drums: ch {args.drums}")
        lines.append(f"// {len(melody)} melody / {len(bass)} bass / "
                     f"{len(pad)} pad chords / {len(drums)} drum hits")
    in_name = "inner" if args.satb is not None else "pad"
    specs = [f"{name} {'/'.join(str(v) for v in adsr[ch])}"
             for ch, name in ((2, "melody"), (0, "bass"), (1, in_name))
             if adsr[ch]]
    if specs:
        lines.append(f"// ADSR (A/D/S/R): {', '.join(specs)}")
    pspecs = [f"{name} {penv[ch][0]:+d}@r{penv[ch][1]}"
              + (f" rel r{penv[ch][2]}" if penv[ch][2] else "")
              for ch, name in ((2, "melody"), (0, "bass"), (1, in_name))
              if penv[ch]]
    if pspecs:
        lines.append(f"// pitch env (slide-in): {', '.join(pspecs)}")
    lines.append('#include "seq.h"')
    lines.append("")
    lines.append(f"const seq_ev_t {args.name}[] = {{")
    lines.append("    // voice setup")
    lines.append(f"    {{ 0, EV_PRESET, 2, {pm} }},   // melody: {PRESET_NAMES[pm]}")
    lines.append(f"    {{ 0, EV_PRESET, 0, {pb} }},   // bass:   {PRESET_NAMES[pb]}")
    lines.append(f"    {{ 0, EV_PRESET, 1, {pp} }},   // {in_name}: "
                 f"{PRESET_NAMES[pp]}")
    det_default = {2: 6, 1: 12, 0: 0x80, 3: 12}
    det = {ch: (inst[ch]["detune"] if inst[ch] is not None
                else det_default[ch]) for ch in det_default}
    lines.append(f"    {{ 0, EV_DETUNE, 2, {det[2]} }},")
    lines.append(f"    {{ 0, EV_DETUNE, 1, {det[1]} }},")
    lines.append(f"    {{ 0, EV_DETUNE, 0, 0x{det[0]:02x} }},"
                 + ("  // bass: detune off" if det[0] == 0x80 else ""))
    if args.satb is not None:
        lines.append(f"    {{ 0, EV_PRESET, 3, {pp} }},   // inner2: "
                     f"{PRESET_NAMES[pp]}")
        lines.append(f"    {{ 0, EV_DETUNE, 3, {det[3]} }},")
    voice_of = {2: "melody", 0: "bass", 1: in_name, 3: "inner2"}
    for ch in (2, 0, 1, 3):
        if adsr[ch]:
            a, d, s, r = adsr[ch]
            lines.append(f"    {{ 0, EV_ADSR, {ch}, 0x{(a << 4) | d:02x}, "
                         f"0x{(s << 4) | r:02x} }},   // {voice_of[ch]}: "
                         f"A{a} D{d} S{s} R{r}")
    for ch in (2, 0, 1, 3):
        if penv.get(ch):
            off, rate, rel = penv[ch]
            tail = f", rel r{rel}" if rel else ""
            lines.append(f"    {{ 0, EV_PENV, {ch}, 0x{off & 0xFF:02x}, "
                         f"0x{(rel << 4) | rate:02x} }},   // {voice_of[ch]}: "
                         f"slide {off:+d} @ r{rate}{tail}")
    for ch in (2, 0, 1, 3):
        if inst.get(ch) and inst[ch]["tslope"]:
            delta, rate = inst[ch]["tslope"]
            lines.append(f"    {{ 0, EV_TSLOPE, {ch}, 0x{delta & 0xFF:02x}, "
                         f"0x{(3 << 4) | rate:02x} }},   // {voice_of[ch]} "
                         f"({inst_name[ch]}): slopes {delta:+d} @ r{rate}")
        if inst.get(ch) and inst[ch]["vib"]:
            rate, cents, delay = inst[ch]["vib"]
            lines.append(f"    {{ 0, EV_VIBRATO, {ch}, {rate}, "
                         f"0x{vib_b(cents, delay):02x} }},   // {voice_of[ch]} "
                         f"({inst_name[ch]}): {rate/10:.1f}Hz +-{cents}c "
                         f"{delay}ms")
    if not adsr[0] or not adsr[2]:
        lines.append("    // fake decay via amp sweeps for voices without an")
        lines.append("    // envelope (they re-arm at every note-on)")
    if not adsr[0]:
        lines.append("    { 0, EV_SWEEP_PA, 0, "
                     "(uint8_t)((13) | (0<<4)), 0 },   // bass amp down r13")
    if not adsr[2]:
        lines.append("    { 0, EV_SWEEP_PA, 2, "
                     "(uint8_t)((14) | (0<<4)), 0 },   // melody amp down r14")

    emitted_ms = 0
    n_ev = 0
    for t, order, cmd, ch, a, b in evs:
        target_ms = round((t - t0) * 1000)
        dt = target_ms - emitted_ms
        while dt > 65535:
            lines.append("    { 65535, EV_NOP },")
            dt -= 65535
            emitted_ms += 65535
        emitted_ms = target_ms
        lines.append(f"    {{ {dt}, {cmd}, {ch}, {a}, {b} }},")
        n_ev += 1
    lines.append("    { 1500, EV_END }")
    lines.append("};")

    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")

    dur = evs[-1][0] - t0 if evs else 0
    print(f"wrote {out}: {n_ev} events, {n_ev * 6} bytes, "
          f"{dur:.0f}s of music", file=sys.stderr)


if __name__ == "__main__":
    main()
