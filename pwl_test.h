#pragma once

// Shared declarations for the PWL synth test / control CLI.
//
//   main.c            CLI plumbing, commands, presets, demo songs
//   tui/PwlSynth.cxx  CTuiSource behind the full-screen TUI ('tui')
//   tui/TuiMain.cxx   'tui' command entry point
//   tui/tuitest.c     'tuitest' curses smoke test

#include <stdint.h>
#include <stdbool.h>

#include "seq.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sequencer: play an event table (any key stops; returns 1 if stopped).
// The TUI plays its RAM conversions through this, and 'play <file.pwl>'
// plays tables loaded from the host filesystem.
int  seq_play(const seq_ev_t *ev, const char *name);

// When set, the note stream is suppressed for the duration of playback
// ('play q' / 'demo <n> q'): on fast songs, drawing every note into the
// Notes tab spends enough UART time to starve pwl_env_service between
// events.  Callers set it around seq_play and clear it after.
extern volatile uint8_t seq_quiet;

// True for a "q"/"-q"/"quiet" trailing argument
bool seq_quiet_arg(const char *s);

// ==========================================================================
// main.c: console plumbing.  The TUI forwards every command it does not
// implement itself to cli_execute(), with printf output redirected into
// its command window.
// ==========================================================================
char *cli_readline(void);
int   cli_split(char *line, char *argv[], int max);
void  cli_execute(int argc, char *argv[]);

// Note name of a MIDI number ("C#4"); points at a static buffer
const char *note_name(int midi);

// Waveform preset table (TUI tab completion / 'wave' help)
int         pwl_preset_count(void);
const char *pwl_preset_name(int idx);
const char *pwl_preset_desc(int idx);

// Instrument panel: apply a preset by index, and match a channel's
// current cached waveform back to a preset index (-1 = custom)
void        pwl_apply_preset(int ch, int idx);
int         pwl_match_preset(int ch);

// ==========================================================================
// Note progression sink.  The TUI installs one to route per-channel note
// events into its Notes tab; while it is NULL the note stream prints to
// stdout as it always has.  'text' is a short token: a note name, a
// note name with '~' for vibrato, or K/S/H for percussion.
// ==========================================================================
extern void (*pwl_note_out)(int channel, const char *text);

// Called at the start of every song / demo (seq_reset): the TUI wipes
// its note tracks so the new piece starts on empty lines.
extern void (*pwl_note_clear)(void);

// ==========================================================================
// TUI entry points
// ==========================================================================
void tui_run(void);             // tui/TuiMain.cxx ('tui' command)
void tui_smoke_test(void);      // tui/tuitest.c ('tuitest' command)

#ifdef __cplusplus
}
#endif
