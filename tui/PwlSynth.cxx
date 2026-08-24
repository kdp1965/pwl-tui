/************************************************************************************
 * pwl-test/tui/PwlSynth.cxx
 *
 * CPwlSynth: CTuiSource implementation for the TT Sky 25a PiecewiseOrion
 * synth (pwl_synth, peripheral 33) on TinyQV.  See PwlSynth.h for the
 * design notes.
 ************************************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "Tui.h"
#include "PwlSynth.h"
#include "MidiFile.h"
#include "Mid2Pwl.h"

extern "C" {
// Angle brackets resolve in the SDK include path (this is the driver,
// not our PwlSynth.h)
#include <pwl_synth.h>
#include <csr.h>
#include <tqv_fs.h>
#include "../pwl_test.h"

// runtime.c: stdout redirection hook used by _write()
extern int (*__tinyqv_stdout_hook)(const char *buffer, int length);
}

// Both hooks are plain function pointers, so route through a singleton.
static CPwlSynth *g_pPwl;

/*
==============================================================================
Notes tab: one note track per synth channel.

Each channel owns an equal share of the tab window - the height is split
four ways, so a taller terminal simply gives every channel more history.
Lines fill left to right with the note tokens main.c streams through
pwl_note_out ("C#4", "A4~" for vibrato, K/S/H for percussion); when the
head line runs out of width the channel moves to its next line and clears
it, so a channel's track always shows its most recent notes without ever
scrolling the other channels.
==============================================================================
*/

#define NOTE_ROWS_MAX   12      /* storage cap on rows per channel */
#define NOTE_LINE_MAX   160     /* chars kept per line */
#define NOTE_GUTTER     4       /* "ch0 " label column */
#define NOTE_VIS_DEF    44      /* wrap width before the tab window exists */
#define NOTE_ROWS_DEF   2       /* rows per channel before one is measured */

static char s_NoteText[PWL_NUM_CHANNELS][NOTE_ROWS_MAX][NOTE_LINE_MAX + 1];
static int  s_NoteRow[PWL_NUM_CHANNELS];    // line of the block being filled
static int  s_NoteLen[PWL_NUM_CHANNELS];    // chars in the head line
static int  s_NoteRows = NOTE_ROWS_DEF;     // rows per channel, from the tab
static char s_LastNote[PWL_NUM_CHANNELS][10];
static int  s_NotesCtxMarker;               // &s_NotesCtxMarker = notes tab

// Per-channel note colors.  Own color pairs rather than the framework's
// syntax pairs: those are tuned for source listings (and the cyan one
// lands on the same grey the terminal backend gives normal text, which
// made one track unreadable).  The bright entries of the base 16 are the
// only ones the vt100 backend maps back to a plain ANSI index.
#define NOTE_PAIR(ch)   (20 + (ch))     // pairs 20..23; framework uses 1..13
#define INSTR_PAIR      24              // instrument panel: bright white

static const short s_ChanColor[PWL_NUM_CHANNELS] =
{
  (short)(COLOR_BLUE   | 8),
  (short)(COLOR_GREEN  | 8),
  (short)(COLOR_CYAN   | 8),
  (short)(COLOR_RED    | 8)
};

/*
==============================================================================
MIDI file tabs ('open <file>.mid').

Each open file gets its own tab whose context is the CMidiFile holding
the parsed summary - and, later, the mid2pwl conversion settings, which
then live and die with the tab.  Contexts are heap pointers, so they are
told apart from the two static tab markers by elimination (IsMidiCtx).

Layout per track: a text line (name / instrument / channel / beats /
notes) followed by three braille lines plotting pitch against time.  A
braille cell is 2 dots wide by 4 tall, so three lines give 12 vertical
dots spanning MIDI_LOW_NOTE..MIDI_HIGH_NOTE - a rough staff.  Those
glyphs are ordinary curses output: the terminal backend now encodes any
16-bit character as UTF-8 (PDC_encode_utf8_term), one glyph per cell.
==============================================================================
*/

#define MIDI_HEAD_LINES 2               // title + blank
#define MIDI_TRK_LINES  4               // text line + 3 braille lines
#define MIDI_FOOT_LINES 4               // conversion settings footer
#define MIDI_GUTTER     3               // "12 " track number column

// Track colors, cycled.  Light on black, and no red or green: Ken is
// colorblind, so the set has to stay distinguishable without them.
#define MIDI_PAIR(n)    (30 + ((n) & 3))

static const short s_MidiColor[4] =
{
  (short)(COLOR_CYAN   | 8),
  (short)(COLOR_YELLOW | 8),
  (short)(COLOR_WHITE  | 8),
  (short)(COLOR_BLUE   | 8)
};

// Braille dot bits: left column is dots 1,2,3,7 and right is 4,5,6,8
static const uint8_t s_BrailleBit[2][4] =
{
  { 0x01, 0x02, 0x04, 0x40 },
  { 0x08, 0x10, 0x20, 0x80 }
};

// Cells of score a window that wide can show.  Drawing and scrolling
// must agree on this or a page lands somewhere other than where the
// eye left off.
static int midi_view_cells(int cols)
{
  int width = cols - MIDI_GUTTER - 1;

  return width < 8 ? 8 : width;
}

// Claim the pairs.  start_color() wipes every pair and CTui::UIInit calls
// it on each 'tui' invocation, so this must run per TUI session, not once
// per boot - the full source-window repaint calls it unconditionally
// (init_pair only dirties the screen when a pair's colors actually
// change) and the per-note path uses the m_ColorsReady latch.
void CPwlSynth::InitNoteColors(void)
{
  int ch;

  for (ch = 0; ch < PWL_NUM_CHANNELS; ch++)
    init_pair(NOTE_PAIR(ch), s_ChanColor[ch], COLOR_BLACK);
  for (ch = 0; ch < 4; ch++)
    init_pair(MIDI_PAIR(ch), s_MidiColor[ch], COLOR_BLACK);
  m_ColorsReady = true;
}

/*
==============================================================================
Command table.  Every console command appears here so tab completion and
'help' cover the whole CLI; all but the UI commands forward to main.c's
dispatcher (Legacy).  min_args of 0 disables CTui's argc validation - the
CLI handlers do their own checking and usage prints.

NOTE: CTui::ProcessCommand prefix-matches the typed word against this
table in order, so keep it alphabetical and list exact names.
==============================================================================
*/

typedef struct PwlCmd
{
   const char *      name;
   int               min_args;
   int               max_args;
   CPwlFunc_t        pFunc;
   const char *      usage;
   const char *      help;
} PwlCmd_t;

static const PwlCmd_t s_TuiCmds[] =
{
  { "adsr",     0, 5, &CPwlSynth::Legacy, "adsr <ch> <a d s r|off>", "Amp envelope: rates 0-15, sustain 0-7" },
  { "amp",      0, 2, &CPwlSynth::Amp,    "amp <ch|chN> <v|pct>", "Set voice amp, or MIDI-ch mix gain (amp ch7 50%)" },
  { "automap",  0, 0, &CPwlSynth::Automap,"automap",              "Guess melody/bass/pad/drums from the MIDI" },
  { "bend",     0, 3, &CPwlSynth::Legacy, "bend <ch> up|down <r>","Period sweep (pitch bend)" },
  { "brighten", 0, 2, &CPwlSynth::Legacy, "brighten <ch> <rate>", "Sweep slopes up (lowpass opening)" },
  { "cat",      0, 1, &CPwlSynth::Legacy, "cat <file>",           "Print a host file" },
  { "chord",    0, 4, &CPwlSynth::Legacy, "chord <n1> [n2 n3 n4]","Play notes on channels 0..3" },
  { "clear",    0, 0, &CPwlSynth::Clear,  "clear",                "Clear the command window" },
  { "close",    0, 0, &CPwlSynth::Close,  "close",                "Close the active source tab" },
  { "cnt",      0, 0, &CPwlSynth::Legacy, "cnt",                  "Read the 24-bit sample counter" },
  { "convert",  0, 0, &CPwlSynth::Convert,"convert",              "Build the MIDI tab's seq table in RAM" },
  { "cset",     0, 2, &CPwlSynth::Cset,   "cset <key> <val>",     "Conversion knob (xpose/legato/roll/...)" },
  { "darken",   0, 2, &CPwlSynth::Legacy, "darken <ch> <rate>",   "Sweep slopes down (lowpass closing)" },
  { "demo",     0, 2, &CPwlSynth::Legacy, "demo <1-16> [q]",      "Play a demo ('q' skips the Notes tab)" },
  { "detune",   0, 2, &CPwlSynth::Legacy, "detune <ch> <n|off>",  "Auto-detune strength (semitones)" },
  { "envlog",   0, 0, &CPwlSynth::Legacy, "envlog",               "Dump the envelope event log" },
  { "fade",     0, 3, &CPwlSynth::Legacy, "fade <ch> <tgt> <r>",  "Amp sweep to target 0-7" },
  { "fs",       0, 1, &CPwlSynth::Legacy, "fs [probe]",           "Host filesystem status / re-probe" },
  { "hat",      0, 1, &CPwlSynth::Legacy, "hat [ch]",             "Hi-hat one-shot (default ch 3)" },
  { "help",     0, 1, &CPwlSynth::Help,   "help [cmd]",           "Paged list; 'help <cmd>' one entry, 'help cli' console text" },
  { "inst",     0, 2, &CPwlSynth::Inst,   "inst <role|chN> <name>", "Role or MIDI-channel instrument ('inst' lists)" },
  { "kick",     0, 1, &CPwlSynth::Legacy, "kick [ch]",            "Kick drum one-shot (default ch 3)" },
  { "ls",       0, 1, &CPwlSynth::Legacy, "ls [dir]",             "List host files" },
  { "map",      0, 2, &CPwlSynth::Map,    "map <role> <ch|off|+ch|-ch>", "MIDI channels -> roles (+/- edit the melody list)" },
  { "note",     0, 3, &CPwlSynth::Legacy, "note <ch> <n> [amp]",  "Play a note (C4, A#3, Eb2 or MIDI)" },
  { "notes",    0, 0, &CPwlSynth::Notes,  "notes",                "Open the per-channel note track tab" },
  { "off",      0, 1, &CPwlSynth::Legacy, "off [ch]",             "Note off (no arg: all channels)" },
  { "open",     0, 1, &CPwlSynth::Open,   "open <file.mid>",      "Open a MIDI file in its own tab" },
  { "penv",     0, 5, &CPwlSynth::Legacy, "penv <ch> <semis> <rate> [rel]", "Pitch env: slide into each note ('penv <ch> off')" },
  { "phase",    0, 2, &CPwlSynth::Legacy, "phase <ch> <v>",       "Set oscillator phase" },
  { "play",     0, 3, &CPwlSynth::Play,   "play [file|chN [inst]] [q]", "Play the conversion, a .pwl, or one channel solo" },
  { "pos",      0, 2, &CPwlSynth::Legacy, "pos <ch> <0-7>",       "Stereo position (needs 'stereo pos')" },
  { "rd",       0, 1, &CPwlSynth::Legacy, "rd <off>",             "Raw 16-bit register read" },
  { "regs",     0, 0, &CPwlSynth::Legacy, "regs",                 "Dump readable registers, all channels" },
  { "save",     0, 1, &CPwlSynth::Save,   "save [file.pwl|.c]",   "Write the conversion to the host" },
  { "selftest", 0, 0, &CPwlSynth::Legacy, "selftest",             "Register access self test" },
  { "snare",    0, 1, &CPwlSynth::Legacy, "snare [ch]",           "Snare one-shot (default ch 3)" },
  { "stereo",   0, 1, &CPwlSynth::Legacy, "stereo off|on|pos",    "Mono / stereo voice / stereo position" },
  { "studio",   0, 0, &CPwlSynth::Studio, "studio",               "Instrument studio tab (CTRL-W focuses it)" },
  { "tenv",     0, 5, &CPwlSynth::Legacy, "tenv <ch> pwm|slope <d> <r>", "Timbre env: bow bite / wah ('tenv <ch> off')" },
  { "trim",     0, 2, &CPwlSynth::Trim,   "trim <n> [bars]",      "Cut the first n beats (or bars) of the MIDI" },
  { "vib",      0, 4, &CPwlSynth::Legacy, "vib <ch> <rate> <depth> [delay]", "Vibrato LFO (rate in 0.1Hz, depth cents)" },
  { "watch",    0, 1, &CPwlSynth::Watch,  "watch <0-3>",          "Choose the channel in the watch window" },
  { "wave",     0, 2, &CPwlSynth::Legacy, "wave <ch> <preset>",   "Set waveform ('wave list' lists presets)" },
  { "wr",       0, 2, &CPwlSynth::Legacy, "wr <off> <v>",         "Raw 16-bit register write" },
  { NULL,       0, 0, NULL,               NULL,                   NULL }
};

/*
==============================================================================
Construction
==============================================================================
*/

CPwlSynth::CPwlSynth()
{
  m_pCmdTabList     = NULL;
  m_OutLen          = 0;
  m_OutOpen         = false;
  m_WatchCh         = 0;
  m_ColorsReady     = false;
  m_pParent         = NULL;    // set by CTui::AttachTuiSource
  m_Filename[0]     = 0;
  m_WorkingDir      = "/";
  // CTui reads/writes this file itself (window layout + command history)
  // through stdio, which reaches tqv.py's served directory.  Empty when
  // no host is serving one - fopen would fail anyway, but settling it
  // here keeps the framework from retrying on every save.
  //
  // Re-probe rather than trust the cache: the console can be detached
  // and reattached (or reattached with --no-fs) between TUI sessions, so
  // "was there a filesystem last time" is not the question.
  tqv_fs_reprobe();
  m_PrefsFile       = tqv_fs_available() ? "tui.cfg" : "";
  g_pPwl            = this;
}

CPwlSynth::~CPwlSynth()
{
  RemoveStdoutHook();
  if (m_pCmdTabList != NULL)
  {
    // Free the persistent list for real
    TuiSortItem_t *pItem = m_pCmdTabList->pFirst;
    while (pItem != NULL)
    {
      TuiSortItem_t *pNext = pItem->pNext;
      free(pItem);
      pItem = pNext;
    }
    free(m_pCmdTabList);
    m_pCmdTabList = NULL;
  }
  if (g_pPwl == this)
    g_pPwl = NULL;
}

/*
==============================================================================
Stdout redirection: printf() output from CLI commands lands here (via
runtime.c's __tinyqv_stdout_hook) and is folded into the command window
line by line.  UICommandPrintString starts a new line per call, so track
whether the current line is still "open" for appends.
==============================================================================
*/

static int pwl_stdout_hook(const char *buffer, int length)
{
  if (g_pPwl != NULL)
    g_pPwl->StdoutChunk(buffer, length);
  return length;
}

static void pwl_note_sink(int channel, const char *text)
{
  if (g_pPwl != NULL)
    g_pPwl->NoteChunk(channel, text);
}

static void pwl_note_clear_sink(void)
{
  if (g_pPwl != NULL)
    g_pPwl->NotesClear();
}

void CPwlSynth::InstallStdoutHook(void)
{
  m_OutLen  = 0;
  m_OutOpen = false;
  __tinyqv_stdout_hook = pwl_stdout_hook;
  pwl_note_out         = pwl_note_sink;
  pwl_note_clear       = pwl_note_clear_sink;
}

void CPwlSynth::RemoveStdoutHook(void)
{
  if (__tinyqv_stdout_hook == pwl_stdout_hook)
    __tinyqv_stdout_hook = NULL;
  if (pwl_note_out == pwl_note_sink)
    pwl_note_out = NULL;
  if (pwl_note_clear == pwl_note_clear_sink)
    pwl_note_clear = NULL;
}

void CPwlSynth::StdoutChunk(const char *buffer, int length)
{
  int x;

  // Drop output once the UI is tearing down - the command window may
  // already be deleted (late prints from the exit path).
  if (m_pParent == NULL || m_pParent->m_Terminate)
    return;

  for (x = 0; x < length; x++)
  {
    char ch = buffer[x];

    if (ch == '\n')
    {
      // Complete the current line
      m_OutLine[m_OutLen] = 0;
      if (m_OutOpen)
        m_pParent->UICommandAppendString(m_OutLine);
      else
        m_pParent->UICommandPrintString(m_OutLine);
      m_OutLen  = 0;
      m_OutOpen = false;
    }
    else
    {
      if (m_OutLen < (int)sizeof(m_OutLine) - 1)
        m_OutLine[m_OutLen++] = ch;

      // Long running commands print progress with '\r'; flush so the
      // user sees it live (CommandProcessLastLine gives '\r' overwrite
      // semantics in the window).
      if (ch == '\r')
      {
        m_OutLine[m_OutLen] = 0;
        if (m_OutOpen)
          m_pParent->UICommandAppendString(m_OutLine);
        else
          m_pParent->UICommandPrintString(m_OutLine);
        m_OutLen  = 0;
        m_OutOpen = true;
      }
    }
  }
}

void CPwlSynth::FlushStdoutLine(void)
{
  if (m_OutLen > 0 && m_pParent != NULL)
  {
    m_OutLine[m_OutLen] = 0;
    if (m_OutOpen)
      m_pParent->UICommandAppendString(m_OutLine);
    else
      m_pParent->UICommandPrintString(m_OutLine);
  }
  m_OutLen  = 0;
  m_OutOpen = false;
}

int CPwlSynth::CmdPrintf(const char *fmt, ...)
{
  char    buf[256];
  va_list ap;
  int     len;

  va_start(ap, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  StdoutChunk(buf, strlen(buf));
  return len;
}

void CPwlSynth::DebugPrintf(const char *fmt, ...)
{
  // Key/debug chatter is not routed anywhere yet
  (void)fmt;
}

/*
==============================================================================
Notes tab plumbing
==============================================================================
*/

CTab *CPwlSynth::FindNotesTab(void)
{
  CTab *pTab;

  if (m_pParent == NULL)
    return NULL;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL; pTab = pTab->GetNextTab())
    if (pTab->SourceContext() == &s_NotesCtxMarker)
      return pTab;

  return NULL;
}

// Visible width available for note text in the notes window
int CPwlSynth::NotesWidth(WINDOW *pWnd)
{
  int rows, cols;

  if (pWnd == NULL)
    return NOTE_VIS_DEF;

  getmaxyx(pWnd, rows, cols);
  (void)rows;

  cols -= NOTE_GUTTER + 1;
  if (cols < 8)
    cols = 8;
  if (cols > NOTE_LINE_MAX)
    cols = NOTE_LINE_MAX;
  return cols;
}

// Adopt a new rows-per-channel split (terminal resize, or the first
// time the tab is measured).  Slots coming into use may still hold text
// from an earlier, taller layout, and a head line can fall off the end
// of a shrunken block - fix both so no stale row is ever drawn.
static void notes_set_rows(int n)
{
  int ch, r;

  if (n < 1)
    n = 1;
  if (n > NOTE_ROWS_MAX)
    n = NOTE_ROWS_MAX;
  if (n == s_NoteRows)
    return;

  for (ch = 0; ch < PWL_NUM_CHANNELS; ch++)
  {
    for (r = s_NoteRows; r < n; r++)
      s_NoteText[ch][r][0] = 0;
    if (s_NoteRow[ch] >= n)
    {
      // head fell outside the block: continue on its last line
      s_NoteRow[ch] = n - 1;
      s_NoteLen[ch] = (int)strlen(s_NoteText[ch][n - 1]);
    }
  }
  s_NoteRows = n;
}

// Rows per channel for a window that tall: an equal share each, so the
// tracks always fill the tab and never need scrolling.
static int notes_rows_for(int winRows)
{
  return winRows / PWL_NUM_CHANNELS;
}

// "ch0 " label at the head of a channel's first line
static void notes_draw_gutter(WINDOW *pWnd, int row, int channel, int which)
{
  wattron(pWnd, COLOR_PAIR(NOTE_PAIR(channel)) | A_BOLD);
  if (which == 0)
    mvwprintw(pWnd, row, 0, "ch%d", channel);
  else
    mvwprintw(pWnd, row, 0, "   ");
  wattroff(pWnd, COLOR_PAIR(NOTE_PAIR(channel)) | A_BOLD);
}

// Draw a channel's head line from 'fromCol' on (fromCol 0 repaints the
// whole row).  Playback updates only ever touch the one row that just
// changed: a full-window repaint costs enough UART time to disturb the
// song's timing.
void CPwlSynth::NoteLineUpdate(int channel, int fromCol)
{
  CTab   *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  WINDOW *pWnd;
  int     rows, cols, row, which, col;

  if (pTab == NULL || pTab->SourceContext() != &s_NotesCtxMarker)
    return;
  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return;

  if (!m_ColorsReady)
    InitNoteColors();
  which = s_NoteRow[channel];
  row   = channel * s_NoteRows + which - pTab->SourceFirstLine();
  getmaxyx(pWnd, rows, cols);
  if (row < 0 || row >= rows)
    return;

  if (fromCol <= 0)
  {
    fromCol = 0;
    wmove(pWnd, row, 0);
    wclrtoeol(pWnd);
    notes_draw_gutter(pWnd, row, channel, which);
  }

  col = NOTE_GUTTER + fromCol;
  if (col < cols - 1)
  {
    wattron(pWnd, COLOR_PAIR(NOTE_PAIR(channel)));
    mvwprintw(pWnd, row, col, "%.*s", cols - 1 - col,
              &s_NoteText[channel][which][fromCol]);
    wattroff(pWnd, COLOR_PAIR(NOTE_PAIR(channel)));
  }
  wrefresh(pWnd);
}

void CPwlSynth::NoteChunk(int channel, const char *text)
{
  CTab *pTab;
  int   budget, len, from;
  char *pLine;

  if (channel < 0 || channel >= PWL_NUM_CHANNELS || text == NULL)
    return;

  snprintf(s_LastNote[channel], sizeof(s_LastNote[channel]), "%s", text);

  // Wrap against the real geometry of the notes window when it exists,
  // so notes arriving before the first repaint already land on the
  // layout the tab will draw
  pTab   = FindNotesTab();
  budget = NotesWidth(pTab != NULL ? pTab->GetWindow() : NULL);
  if (pTab != NULL && pTab->GetWindow() != NULL)
  {
    int wrows, wcols;

    getmaxyx(pTab->GetWindow(), wrows, wcols);
    (void)wcols;
    notes_set_rows(notes_rows_for(wrows));
  }

  len = (int)strlen(text) + 1;            // token + separating space
  if (len > budget)
    len = budget;                         // pathological width: never wrap forever

  // Out of room: move to this channel's next line and start it empty.
  // The other channels' tracks are untouched.
  if (s_NoteLen[channel] + len > budget)
  {
    s_NoteRow[channel] = (s_NoteRow[channel] + 1) % s_NoteRows;
    s_NoteLen[channel] = 0;
    s_NoteText[channel][s_NoteRow[channel]][0] = 0;
    from = 0;                             // repaint the recycled row
  }
  else
    from = s_NoteLen[channel];            // append only

  pLine = s_NoteText[channel][s_NoteRow[channel]];
  s_NoteLen[channel] += snprintf(&pLine[s_NoteLen[channel]],
                                 NOTE_LINE_MAX + 1 - s_NoteLen[channel],
                                 "%s ", text);
  if (s_NoteLen[channel] > NOTE_LINE_MAX)
    s_NoteLen[channel] = NOTE_LINE_MAX;

  NoteLineUpdate(channel, from);
}

// Every song / demo starts from seq_reset(), which calls this through
// pwl_note_clear: the new piece gets empty tracks rather than scrolling
// out of the previous song's notes.
void CPwlSynth::NotesClear(void)
{
  CTab *pTab;

  memset(s_NoteText, 0, sizeof(s_NoteText));
  memset(s_NoteRow,  0, sizeof(s_NoteRow));
  memset(s_NoteLen,  0, sizeof(s_NoteLen));
  memset(s_LastNote, 0, sizeof(s_LastNote));

  if (m_pParent == NULL)
    return;

  pTab = m_pParent->GetActiveSrcTab();
  if (pTab != NULL && pTab->SourceContext() == &s_NotesCtxMarker)
    m_pParent->DrawSourceWindow();
}

CTab *CPwlSynth::EnsureNotesTab(void)
{
  CTab *pTab = FindNotesTab();

  if (pTab == NULL)
  {
    pTab = m_pParent->CreateNewTab("Notes");
    if (pTab == NULL)
      return NULL;
    pTab->AttachTuiSource(this, &s_NotesCtxMarker);
  }

  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return pTab;
}

/*
==============================================================================
Instrument panel tab ('instr'; auto-created when the TUI starts).

A keyboard-synth front panel: one row per parameter, arrows to select
and adjust, SPACE gates a note on the edited channel.  Values are read
from the driver's pwl_ch[] shadow state, so the panel always shows the
truth (even after a demo reprogrammed a channel), and every adjustment
is applied immediately - live where the hardware allows it, else at the
next note-on.  Selecting a channel also points the watch window at it,
so the raw registers track the knobs while sweeps run.

Key routing: CTui gives the active tab's source first pick of the keys
(WantProcessKey/ProcessKey) when the source window has focus - hit
CTRL-W until the source frame highlights, then the arrows are knobs.
==============================================================================
*/

static int s_InstrCtxMarker;            // &s_InstrCtxMarker = instrument tab

enum
{
  IR_CHAN, IR_GATE, IR_NOTE, IR_VEL, IR_DUR, IR_PRESET, IR_DETUNE,
  IR_SLOPE_R, IR_SLOPE_F, IR_PWM,
  IR_ENV_EN, IR_ATK, IR_DEC, IR_SUS, IR_REL,
  IR_POFF, IR_PRATE, IR_PREL,
  IR_TPWM_D, IR_TPWM_R, IR_TSLP_D, IR_TSLP_R, IR_TDIR,
  IR_VIB_RATE, IR_VIB_DEPTH, IR_VIB_DELAY,
  IR_COUNT
};

// Screen order: parameter rows with group headers between sections
typedef struct { int8_t id; const char *hdr; } instr_line_t;
static const instr_line_t s_InstrLayout[] =
{
  { IR_CHAN,    NULL }, { IR_GATE,   NULL }, { IR_NOTE,   NULL },
  { IR_VEL,     NULL }, { IR_DUR,    NULL }, { IR_PRESET, NULL },
  { IR_DETUNE,  NULL },
  { -1, "waveform" },
  { IR_SLOPE_R, NULL }, { IR_SLOPE_F, NULL }, { IR_PWM,   NULL },
  { -1, "amp envelope (ADSR)" },
  { IR_ENV_EN,  NULL }, { IR_ATK,    NULL }, { IR_DEC,    NULL },
  { IR_SUS,     NULL }, { IR_REL,    NULL },
  { -1, "pitch envelope" },
  { IR_POFF,    NULL }, { IR_PRATE,  NULL }, { IR_PREL,   NULL },
  { -1, "timbre envelope" },
  { IR_TPWM_D,  NULL }, { IR_TPWM_R, NULL }, { IR_TSLP_D, NULL },
  { IR_TSLP_R,  NULL }, { IR_TDIR,   NULL },
  { -1, "vibrato" },
  { IR_VIB_RATE, NULL }, { IR_VIB_DEPTH, NULL }, { IR_VIB_DELAY, NULL },
};
#define INSTR_LINES ((int)(sizeof(s_InstrLayout) / sizeof(s_InstrLayout[0])))

static int     s_InstrSel = IR_NOTE;    // selected parameter row
static uint8_t s_InstrSkipDraw;         // ProcessKey already updated rows
static uint32_t s_InstrDirty;           // dependent rows an adjust touched
static uint8_t s_InstrCh  = 0;
static uint8_t s_InstrNote[PWL_NUM_CHANNELS] = { 69, 69, 69, 69 };
static uint8_t s_InstrVel[PWL_NUM_CHANNELS]  = { 50, 50, 50, 50 };

// 'r' play mode: continuous (retrigger and ring) or timed (gate off
// after the Duration row's ms; 'c' toggles).  Timed note-offs are
// serviced read-free from IdlePoll deadlines.
static uint16_t s_InstrDur[PWL_NUM_CHANNELS] = { 500, 500, 500, 500 };
static uint8_t  s_InstrCont[PWL_NUM_CHANNELS] = { 1, 1, 1, 1 };
static uint8_t  s_InstrOffPending[PWL_NUM_CHANNELS];
static uint32_t s_InstrOffAt[PWL_NUM_CHANNELS];

static int instr_clamp(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

/*
------------------------------------------------------------------------------
Studio recordings: numbered snapshots of the edited channel's complete
sound ('r' records, ,/. select and restore, [/] reorder, X deletes).
The list lives in a plain-text KEY=VALUE file on the host filesystem
(s_StudioFile - one file for now; open/save-as can arrive later) so
sounds survive reboots: opening the tab loads it once per boot and
every change autosaves ~2s after the keys settle (IdlePoll flushes,
so a burst of edits costs one host write).
------------------------------------------------------------------------------
*/

typedef struct
{
  uint8_t  note, vel;               // audition pitch / velocity
  uint16_t dur;                     // 'p' duration, ms
  uint8_t  cont;                    // continuous (vs timed) play mode
  uint8_t  slope_r, slope_f, pwm;   // waveform
  uint16_t mode;                    //   (per-note detune_exp masked out)
  uint16_t sweep_pa, sweep_ws;      // sweep caches (perc pitch drops)
  int8_t   detune;                  // PWL_DETUNE_OFF = off
  uint8_t  env_en, env_a, env_d, env_s, env_r;
  int8_t   p_off;                   // pitch envelope
  uint8_t  p_rate, p_rel, p_up;
  int16_t  t_pwm_d;                 // timbre envelope
  uint8_t  t_pwm_r;
  int8_t   t_slp_d;
  uint8_t  t_slp_r, t_dir;
  uint8_t  v_rate, v_depth;         // vibrato
  uint16_t v_delay;
} StudioRec_t;

#define STUDIO_MAX_RECS 16

static StudioRec_t s_StudioRecs[STUDIO_MAX_RECS];
static int      s_StudioCount;
static int      s_StudioSel;            // 0-based; valid while count > 0
static char     s_StudioFile[40] = "studio.rec";
static uint8_t  s_StudioLoaded;         // file read once per boot
static uint8_t  s_StudioDirty;          // autosave pending
static uint32_t s_StudioSaveAt;         // us deadline for the flush

static void studio_touch(void)
{
  s_StudioDirty = 1;
  s_StudioSaveAt = read_time() + 2000000u;
}

static void studio_capture(StudioRec_t *r)
{
  pwl_channel_state_t *pCh = &pwl_ch[s_InstrCh];

  memset(r, 0, sizeof(*r));
  r->note = s_InstrNote[s_InstrCh];
  r->vel  = s_InstrVel[s_InstrCh];
  r->dur  = s_InstrDur[s_InstrCh];
  r->cont = s_InstrCont[s_InstrCh];
  r->slope_r = pCh->slope_r;
  r->slope_f = pCh->slope_f;
  r->pwm     = pCh->pwm_offset;
  r->mode    = (uint16_t)(pCh->mode & ~7u);
  r->sweep_pa = pCh->sweep_pa;
  r->sweep_ws = pCh->sweep_ws;
  r->detune  = pCh->relative_detune;
  r->env_en  = pCh->env_enabled;
  r->env_a = pCh->env_a;  r->env_d = pCh->env_d;
  r->env_s = pCh->env_s;  r->env_r = pCh->env_r;
  r->p_off = pCh->penv_offset;    r->p_rate = pCh->penv_rate;
  r->p_rel = pCh->penv_rel_rate;  r->p_up   = pCh->penv_rel_up;
  r->t_pwm_d = pCh->tenv_pwm_delta;    r->t_pwm_r = pCh->tenv_pwm_rate;
  r->t_slp_d = pCh->tenv_slope_delta;  r->t_slp_r = pCh->tenv_slope_rate;
  r->t_dir   = pCh->tenv_slope_dir;
  r->v_rate  = pCh->vib_rate;  r->v_depth = pCh->vib_depth;
  r->v_delay = pCh->vib_delay_ms;
}

// Push a recording into the edited channel through the same driver
// calls the knobs use; a sounding note retriggers so what rings IS the
// recording.  Whatever channel it was captured on, it applies HERE -
// which is also how a sound is copied between channels.
static void studio_apply(const StudioRec_t *r)
{
  int ch = s_InstrCh;
  pwl_channel_state_t *pCh = &pwl_ch[ch];

  s_InstrNote[ch] = r->note;
  s_InstrVel[ch]  = r->vel;
  s_InstrDur[ch]  = r->dur;
  s_InstrCont[ch] = r->cont;
  pwl_set_waveform(ch, r->slope_r, r->slope_f, r->pwm, r->mode);
  pwl_set_sweeps(ch, r->sweep_pa, r->sweep_ws);
  pCh->relative_detune = r->detune;
  pCh->env_a = r->env_a;  pCh->env_d = r->env_d;  // keep the knob values
  pCh->env_s = r->env_s;  pCh->env_r = r->env_r;  //   even when disabled
  if (r->env_en)
    pwl_set_adsr(ch, r->env_a, r->env_d, r->env_s, r->env_r);
  else
    pwl_adsr_off(ch);
  pwl_set_pitch_env(ch, r->p_off, r->p_rate, r->p_rel, r->p_up != 0);
  pwl_set_timbre_env(ch, r->t_pwm_d, r->t_pwm_r,
                     r->t_slp_d, r->t_slp_r, r->t_dir);
  pwl_set_vibrato(ch, r->v_rate, r->v_depth, r->v_delay);
  if (pCh->on)
    pwl_note_on(ch, r->note, r->vel);   // note-on re-arms the vibrato too
}

// Parse up to n comma-separated ints - "0x" hex or signed decimal
// (this platform's stdlib has no strtol)
static int studio_ints(const char *s, int *v, int n)
{
  int i = 0;

  while (i < n)
  {
    int neg = 0, val = 0, any = 0;

    if (*s == '+' || *s == '-')
      neg = (*s++ == '-');
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
      for (s += 2; ; s++, any = 1)
      {
        int d = (*s >= '0' && *s <= '9') ? *s - '0'
              : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
              : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;

        if (d < 0)
          break;
        val = val * 16 + d;
      }
    }
    else
      for (; *s >= '0' && *s <= '9'; s++, any = 1)
        val = val * 10 + (*s - '0');
    if (!any)
      break;
    v[i++] = neg ? -val : val;
    if (*s != ',')
      break;
    s++;
  }
  return i;
}

static void studio_save_file(void)
{
  FILE *f;
  int   i;

  if ((f = fopen(s_StudioFile, "w")) == NULL)
    return;                     // no host FS: silent, like the .cfg saves
  fprintf(f, "PWLSTUDIO1\n");
  fprintf(f, "SEL=%d\n", s_StudioCount ? s_StudioSel + 1 : 0);
  for (i = 0; i < s_StudioCount; i++)
  {
    const StudioRec_t *r = &s_StudioRecs[i];

    fprintf(f, "REC=%d\n", i + 1);
    fprintf(f, "  NOTE=%d,%d\n", r->note, r->vel);
    fprintf(f, "  DUR=%u,%s\n", r->dur, r->cont ? "cont" : "timed");
    fprintf(f, "  WAVE=%d,%d,%d,0x%03X\n", r->slope_r, r->slope_f,
            r->pwm, r->mode);
    fprintf(f, "  SWEEP=0x%04X,0x%04X\n", r->sweep_pa, r->sweep_ws);
    if (r->detune == PWL_DETUNE_OFF)
      fprintf(f, "  DETUNE=off\n");
    else
      fprintf(f, "  DETUNE=%d\n", r->detune);
    fprintf(f, "  ADSR=%s,%d,%d,%d,%d\n", r->env_en ? "on" : "off",
            r->env_a, r->env_d, r->env_s, r->env_r);
    fprintf(f, "  PENV=%d,%d,%d,%d\n", r->p_off, r->p_rate, r->p_rel,
            r->p_up);
    fprintf(f, "  TENV=%d,%d,%d,%d,%d\n", r->t_pwm_d, r->t_pwm_r,
            r->t_slp_d, r->t_slp_r, r->t_dir);
    fprintf(f, "  VIB=%d,%d,%d\n", r->v_rate, r->v_depth, r->v_delay);
  }
  fclose(f);
}

static void studio_load_file(void)
{
  StudioRec_t *r = NULL;
  FILE        *f;
  char         line[96];
  int          v[8], sel = 0;

  if ((f = fopen(s_StudioFile, "r")) == NULL)
    return;
  s_StudioCount = 0;
  while (fgets(line, sizeof(line), f) != NULL)
  {
    char *key = line;
    char *val;

    while (*key == ' ')                 // params sit indented under REC=
      key++;
    val = strchr(key, '=');
    if (val == NULL)
      continue;
    *val++ = 0;
    if (strcmp(key, "SEL") == 0)
      sel = atoi(val) - 1;
    else if (strcmp(key, "REC") == 0)
    {
      if (s_StudioCount >= STUDIO_MAX_RECS)
        break;
      r = &s_StudioRecs[s_StudioCount++];
      memset(r, 0, sizeof(*r));
      r->note = 69;  r->vel = 50;  r->detune = PWL_DETUNE_OFF;  r->t_dir = 3;
      r->dur = 500;  r->cont = 1;      // pre-DUR files: the panel defaults
    }
    else if (r == NULL)
      continue;
    else if (strcmp(key, "NOTE") == 0 && studio_ints(val, v, 2) == 2)
    {
      r->note = (uint8_t)v[0];  r->vel = (uint8_t)v[1];
    }
    else if (strcmp(key, "DUR") == 0)
    {
      char *tok = strchr(val, ',');

      if (studio_ints(val, v, 1) == 1)
        r->dur = (uint16_t)instr_clamp(v[0], 50, 5000);
      if (tok != NULL)
        r->cont = tok[1] == 'c';
    }
    else if (strcmp(key, "WAVE") == 0 && studio_ints(val, v, 4) == 4)
    {
      r->slope_r = (uint8_t)v[0];  r->slope_f = (uint8_t)v[1];
      r->pwm = (uint8_t)v[2];      r->mode = (uint16_t)v[3];
    }
    else if (strcmp(key, "SWEEP") == 0 && studio_ints(val, v, 2) == 2)
    {
      r->sweep_pa = (uint16_t)v[0];  r->sweep_ws = (uint16_t)v[1];
    }
    else if (strcmp(key, "DETUNE") == 0)
      r->detune = (int8_t)(strncmp(val, "off", 3) == 0 ? PWL_DETUNE_OFF
                                                       : atoi(val));
    else if (strcmp(key, "ADSR") == 0)
    {
      r->env_en = strncmp(val, "on", 2) == 0;
      val = strchr(val, ',');
      if (val != NULL && studio_ints(val + 1, v, 4) == 4)
      {
        r->env_a = (uint8_t)v[0];  r->env_d = (uint8_t)v[1];
        r->env_s = (uint8_t)v[2];  r->env_r = (uint8_t)v[3];
      }
    }
    else if (strcmp(key, "PENV") == 0 && studio_ints(val, v, 4) == 4)
    {
      r->p_off = (int8_t)v[0];   r->p_rate = (uint8_t)v[1];
      r->p_rel = (uint8_t)v[2];  r->p_up = (uint8_t)v[3];
    }
    else if (strcmp(key, "TENV") == 0 && studio_ints(val, v, 5) == 5)
    {
      r->t_pwm_d = (int16_t)v[0];  r->t_pwm_r = (uint8_t)v[1];
      r->t_slp_d = (int8_t)v[2];   r->t_slp_r = (uint8_t)v[3];
      r->t_dir = (uint8_t)v[4];
    }
    else if (strcmp(key, "VIB") == 0 && studio_ints(val, v, 3) == 3)
    {
      r->v_rate = (uint8_t)v[0];  r->v_depth = (uint8_t)v[1];
      r->v_delay = (uint16_t)v[2];
    }
  }
  fclose(f);
  s_StudioSel = s_StudioCount ? instr_clamp(sel, 0, s_StudioCount - 1) : 0;
}

// The numbered-recordings footer line; the selection shows reversed
static void studio_draw_recs(WINDOW *pWnd, int line)
{
  int i;

  wmove(pWnd, line, 0);
  wclrtoeol(pWnd);
  mvwprintw(pWnd, line, 1, "sounds:");
  if (s_StudioCount == 0)
    wprintw(pWnd, " (none)");
  for (i = 0; i < s_StudioCount; i++)
  {
    waddch(pWnd, ' ');
    if (i == s_StudioSel)
      wattron(pWnd, A_REVERSE);
    wprintw(pWnd, "%d", i + 1);
    if (i == s_StudioSel)
      wattroff(pWnd, A_REVERSE);
  }
  wprintw(pWnd, "   r add  u upd  ,/. pick  [/] move  X del  -> %s%s",
           s_StudioFile, s_StudioDirty ? " *" : "");
}

/*
------------------------------------------------------------------------------
Song recorder: 'R' captures a performance on the panel - the note keys
(a-g # + -), SPACE gates, p one-shots, s silences, and ,/. sound picks
(h/l alias them, vi-style) - as timestamped events.  The take opens with a SOUND
anchor (the sound selected at 'R'), then ,/. replay as the relative
moves they are.  'P' replays the take by feeding the keys back through
ProcessKey on the IdlePoll clock; SOUND events re-apply the numbered
sound, so reordering the sounds list re-orchestrates the song.  'R'
with a take already recorded asks append-or-overwrite first.  The take
lives in RAM and autosaves to s_SongFile (plain text, PWLSONG1) after
the recording stops - never during it, a host-FS write would stall the
performance clock.
------------------------------------------------------------------------------
*/

#define SONG_KIND_SOUND 0
#define SONG_KIND_KEY   1
#define SONG_MAX_EVS    512

typedef struct
{
  uint16_t dt;                      // ms since the previous event
  uint8_t  kind, arg;               // SOUND: 0-based index; KEY: the char
} SongEv_t;

static SongEv_t s_SongEvs[SONG_MAX_EVS];
static int      s_SongCount;
static int      s_SongIdx;              // replay cursor
static uint8_t  s_SongRecording, s_SongPlaying, s_SongAsk;
static uint32_t s_SongTLast;            // us: previous event's moment
static uint32_t s_SongNextAt;           // us: next replay event due
static char     s_SongFile[40] = "song.studio";
static uint8_t  s_SongDirty;
static uint32_t s_SongSaveAt;

static void song_touch(void)
{
  s_SongDirty = 1;
  s_SongSaveAt = read_time() + 2000000u;
}

static uint16_t song_ms(uint32_t us)
{
  uint32_t ms = us / 1000u;

  return ms > 65535u ? (uint16_t)65535u : (uint16_t)ms;
}

static void song_push(uint8_t kind, uint8_t arg, uint16_t dt)
{
  if (s_SongCount >= SONG_MAX_EVS)
  {
    if (s_SongRecording)
    {
      s_SongRecording = 0;              // full: the take just ends
      song_touch();
    }
    return;
  }
  s_SongEvs[s_SongCount].dt = dt;
  s_SongEvs[s_SongCount].kind = kind;
  s_SongEvs[s_SongCount].arg = arg;
  s_SongCount++;
}

// Begin (or resume) recording; wipe = overwrite the take
static void song_start(int wipe)
{
  uint32_t now = read_time();

  if (wipe)
    s_SongCount = 0;
  s_SongRecording = 1;
  s_SongTLast = now;
  if (s_StudioCount > 0)                // the take opens with its sound
    song_push(SONG_KIND_SOUND, (uint8_t)s_StudioSel, 0);
}

// A performance key landed: log it with its delay
static void song_tap(int key)
{
  uint32_t now = read_time();

  song_push(SONG_KIND_KEY, (uint8_t)key, song_ms(now - s_SongTLast));
  s_SongTLast = now;
}

static void studio_save_song(void)
{
  FILE *f;
  int   i;

  if ((f = fopen(s_SongFile, "w")) == NULL)
    return;                             // no host FS: silent
  fprintf(f, "PWLSONG1\n");
  for (i = 0; i < s_SongCount; i++)
  {
    const SongEv_t *e = &s_SongEvs[i];

    if (e->kind == SONG_KIND_SOUND)
      fprintf(f, "SOUND=%u,%u\n", e->dt, e->arg + 1);
    else if (e->arg == ' ')
      fprintf(f, "KEY=%u,SP\n", e->dt);
    else
      fprintf(f, "KEY=%u,%c\n", e->dt, e->arg);
  }
  fclose(f);
}

static void studio_load_song(void)
{
  FILE *f;
  char  line[64];
  int   v[2];

  if ((f = fopen(s_SongFile, "r")) == NULL)
    return;
  s_SongCount = 0;
  while (fgets(line, sizeof(line), f) != NULL)
  {
    char *val = strchr(line, '=');

    if (val == NULL)
      continue;
    *val++ = 0;
    if (strcmp(line, "SOUND") == 0 && studio_ints(val, v, 2) == 2)
      song_push(SONG_KIND_SOUND, (uint8_t)(v[1] - 1), (uint16_t)v[0]);
    else if (strcmp(line, "KEY") == 0)
    {
      char *tok = strchr(val, ',');

      if (tok == NULL || studio_ints(val, v, 1) != 1)
        continue;
      tok++;
      song_push(SONG_KIND_KEY,
                (uint8_t)(tok[0] == 'S' && tok[1] == 'P' ? ' ' : tok[0]),
                (uint16_t)v[0]);
    }
  }
  fclose(f);
}

// The song footer line: take state, progress, and the pending question
static void studio_draw_song(WINDOW *pWnd, int line)
{
  wmove(pWnd, line, 0);
  wclrtoeol(pWnd);
  mvwprintw(pWnd, line, 1, "song:");
  if (s_SongAsk)
  {
    wprintw(pWnd, " already recorded - (a)ppend, (o)verwrite, other = cancel");
    return;
  }
  if (s_SongRecording)
    wprintw(pWnd, " REC %d events   R stop", s_SongCount);
  else if (s_SongPlaying)
    wprintw(pWnd, " playing %d/%d   P stop", s_SongIdx, s_SongCount);
  else if (s_SongCount == 0)
    wprintw(pWnd, " (empty)   R rec");
  else
    wprintw(pWnd, " %d events   R rec  P play", s_SongCount);
  wprintw(pWnd, "  -> %s%s", s_SongFile, s_SongDirty ? " *" : "");
}

static bool instr_tab_active(CTui *pParent)
{
  CTab *pTab = (pParent != NULL) ? pParent->GetActiveSrcTab() : NULL;

  return pTab != NULL && pTab->SourceContext() == &s_InstrCtxMarker;
}

CTab *CPwlSynth::FindInstrTab(void)
{
  CTab *pTab;

  if (m_pParent == NULL)
    return NULL;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL; pTab = pTab->GetNextTab())
    if (pTab->SourceContext() == &s_InstrCtxMarker)
      return pTab;

  return NULL;
}

CTab *CPwlSynth::EnsureInstrTab(void)
{
  CTab *pTab = FindInstrTab();

  if (pTab == NULL)
  {
    pTab = m_pParent->CreateNewTab("Studio");
    if (pTab == NULL)
      return NULL;
    pTab->AttachTuiSource(this, &s_InstrCtxMarker);
    keypad(pTab->GetWindow(), TRUE);    // arrows must arrive as KEY_*
    if (!s_StudioLoaded)
    {
      // First open this boot: restore the sounds + song files
      s_StudioLoaded = 1;
      studio_load_file();
      studio_load_song();
    }
  }

  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return pTab;
}

int CPwlSynth::Studio(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  return EnsureInstrTab() != NULL ? OK : -1;
}

void CPwlSynth::IdlePoll(void)
{
  uint32_t now;
  int      ch, fired = 0;

  // ADSR software stages + pitch/timbre slide stop-writes
  pwl_env_service();

  // Timed one-shots ('r' in play-for-duration mode)
  now = read_time();
  for (ch = 0; ch < PWL_NUM_CHANNELS; ch++)
  {
    if (s_InstrOffPending[ch] &&
        (int32_t)(now - s_InstrOffAt[ch]) >= 0)
    {
      s_InstrOffPending[ch] = 0;
      pwl_note_off(ch);
      fired = 1;
    }
  }

  // Studio recordings: flush the pending autosave once keys settle
  if (s_StudioDirty && !s_SongRecording && !s_SongPlaying &&
      (int32_t)(now - s_StudioSaveAt) >= 0)
  {
    s_StudioDirty = 0;
    studio_save_file();
    InstrRecsRepaint();                 // retire the unsaved-changes '*'
  }

  // Song replay: fire due events; keys go back through ProcessKey so
  // replay IS a performance (row repaints included)
  if (s_SongPlaying)
  {
    int sound = 0, any = 0;

    while (s_SongPlaying && (int32_t)(now - s_SongNextAt) >= 0)
    {
      SongEv_t *e = &s_SongEvs[s_SongIdx];

      if (e->kind == SONG_KIND_SOUND)
      {
        if (e->arg < s_StudioCount)
        {
          s_StudioSel = e->arg;
          studio_apply(&s_StudioRecs[e->arg]);
          sound = 1;
        }
      }
      else
      {
        ProcessKey(e->arg);
        s_InstrSkipDraw = 0;            // direct call: no SetFocus follows
        if (e->arg == ',' || e->arg == '.' ||
            e->arg == 'h' || e->arg == 'l')
          sound = 1;                    // selection moved: rows changed
      }
      any = 1;
      if (++s_SongIdx >= s_SongCount)
        s_SongPlaying = 0;              // the take is over
      else
        s_SongNextAt += (uint32_t)s_SongEvs[s_SongIdx].dt * 1000u;
    }
    if (sound && instr_tab_active(m_pParent))
      m_pParent->DrawSourceWindow();    // sound change repaints the rows
    else if (any)
      InstrSongRepaint();
  }

  // Song autosave (recording stopped: a host write mid-take would
  // stall the performance clock)
  if (s_SongDirty && !s_SongRecording && !s_SongPlaying &&
      (int32_t)(now - s_SongSaveAt) >= 0)
  {
    s_SongDirty = 0;
    studio_save_song();
    InstrSongRepaint();
  }

  // A note just ended: refresh the panel's gate/title state
  if (fired && instr_tab_active(m_pParent))
    m_pParent->DrawSourceWindow();
}

bool CPwlSynth::WantProcessKey(void)
{
  return instr_tab_active(m_pParent);
}

int CPwlSynth::WantFocus(void)
{
  return instr_tab_active(m_pParent) ? 1 : 0;
}

void CPwlSynth::SetFocus(void *pCtx, WINDOW *pWnd, int topLine, int lineCount)
{
  (void)lineCount;
  if (pCtx != &s_InstrCtxMarker)
    return;
  if (s_InstrSkipDraw)
  {
    // ProcessKey already repainted just the touched rows - a full
    // werase+redraw here would re-emit kilobytes per keystroke over
    // the UART and the design drops keys arriving mid-transmission
    s_InstrSkipDraw = 0;
    return;
  }
  DrawInstrPanel(pWnd, topLine);
}

// Set the current channel's note from the musical keys.  Retunes a
// sounding note immediately; a pending timed one-shot restarts its
// duration (each press is a fresh note).
void CPwlSynth::InstrSetNote(int midi)
{
  int ch = s_InstrCh;

  midi = instr_clamp(midi, 11, 106);
  s_InstrNote[ch] = (uint8_t)midi;
  if (!s_InstrCont[ch])
  {
    // note mode: audition every pitch change for the set duration
    pwl_note_on(ch, midi, s_InstrVel[ch]);
    s_InstrOffAt[ch] = read_time() + (uint32_t)s_InstrDur[ch] * 1000u;
    s_InstrOffPending[ch] = 1;
  }
  else if (pwl_ch[ch].on)
    pwl_note_on(ch, midi, s_InstrVel[ch]);      // retune the held note
}

void CPwlSynth::InstrGate(bool on)
{
  s_InstrOffPending[s_InstrCh] = 0;     // manual gate cancels timed off
  if (on)
    pwl_note_on(s_InstrCh, s_InstrNote[s_InstrCh], s_InstrVel[s_InstrCh]);
  else
    pwl_note_off(s_InstrCh);
}

void CPwlSynth::InstrSelectChannel(int ch)
{
  if (ch < 0 || ch >= PWL_NUM_CHANNELS)
    return;
  s_InstrCh = (uint8_t)ch;
  m_WatchCh = ch;                       // the watch registers follow
}                                       //   (idle timer redraws it)

// Label + value text for a parameter row; barCur/barMax describe the
// slider (barMax 0 = no slider for this row)
void CPwlSynth::InstrRowText(int id, char *label, char *val,
                             int *barCur, int *barMax)
{
  static const char *const dirs[4] = { "opp", "r only", "f only", "both" };
  pwl_channel_state_t *pCh = &pwl_ch[s_InstrCh];
  int p;

  *barCur = 0;
  *barMax = 0;
  label[0] = 0;
  val[0] = 0;

  switch (id)
  {
    case IR_CHAN:
      strcpy(label, "Channel");
      sprintf(val, "%d", s_InstrCh);
      break;
    case IR_GATE:
      strcpy(label, "Gate");
      strcpy(val, pCh->on ? "ON" : "off");
      break;
    case IR_NOTE:
      strcpy(label, "Note");
      sprintf(val, "%-4s (%d)", note_name(s_InstrNote[s_InstrCh]),
              s_InstrNote[s_InstrCh]);
      break;
    case IR_VEL:
      strcpy(label, "Velocity");
      sprintf(val, "%d", s_InstrVel[s_InstrCh]);
      *barCur = s_InstrVel[s_InstrCh]; *barMax = 63;
      break;
    case IR_DUR:
      strcpy(label, "Duration");
      sprintf(val, "%dms %s", s_InstrDur[s_InstrCh],
              s_InstrCont[s_InstrCh] ? "(cont)" : "(timed)");
      *barCur = s_InstrDur[s_InstrCh]; *barMax = 5000;
      break;
    case IR_PRESET:
      strcpy(label, "Preset");
      p = pwl_match_preset(s_InstrCh);
      if (p >= 0)
        sprintf(val, "%s", pwl_preset_name(p));
      else
        strcpy(val, "custom");
      break;
    case IR_DETUNE:
      strcpy(label, "Detune");
      if (pCh->relative_detune == PWL_DETUNE_OFF)
        strcpy(val, "off");
      else
        sprintf(val, "%+d", pCh->relative_detune);
      break;
    case IR_SLOPE_R:
      strcpy(label, "Slope R");
      sprintf(val, "%d", pCh->slope_r);
      *barCur = pCh->slope_r; *barMax = 255;
      break;
    case IR_SLOPE_F:
      strcpy(label, "Slope F");
      sprintf(val, "%d", pCh->slope_f);
      *barCur = pCh->slope_f; *barMax = 255;
      break;
    case IR_PWM:
      strcpy(label, "PWM offset");
      sprintf(val, "%d", pCh->pwm_offset);
      *barCur = pCh->pwm_offset; *barMax = 255;
      break;
    case IR_ENV_EN:
      strcpy(label, "Envelope");
      strcpy(val, pCh->env_enabled ? "on" : "off");
      break;
    case IR_ATK:
      strcpy(label, "Attack");
      sprintf(val, "%d%s", pCh->env_a, pCh->env_a ? "" : " (instant)");
      *barCur = pCh->env_a; *barMax = 15;
      break;
    case IR_DEC:
      strcpy(label, "Decay");
      sprintf(val, "%d%s", pCh->env_d, pCh->env_d ? "" : " (instant)");
      *barCur = pCh->env_d; *barMax = 15;
      break;
    case IR_SUS:
      strcpy(label, "Sustain");
      sprintf(val, "%d (amp %d)", pCh->env_s, 9 * pCh->env_s);
      *barCur = pCh->env_s; *barMax = 7;
      break;
    case IR_REL:
      strcpy(label, "Release");
      sprintf(val, "%d%s", pCh->env_r, pCh->env_r ? "" : " (instant)");
      *barCur = pCh->env_r; *barMax = 15;
      break;
    case IR_POFF:
      strcpy(label, "Slide-in");
      if (pCh->penv_offset == 0)
        strcpy(val, "off");
      else
        sprintf(val, "%+d semis", pCh->penv_offset);
      break;
    case IR_PRATE:
      strcpy(label, "Slide rate");
      sprintf(val, "%d", pCh->penv_rate);
      *barCur = pCh->penv_rate; *barMax = 15;
      break;
    case IR_PREL:
      strcpy(label, "Rel fall");
      if (pCh->penv_rel_rate == 0)
        strcpy(val, "off");
      else
        sprintf(val, "r%d%s", pCh->penv_rel_rate,
                pCh->penv_rel_up ? " (up)" : "");
      *barCur = pCh->penv_rel_rate; *barMax = 15;
      break;
    case IR_TPWM_D:
      strcpy(label, "PWM delta");
      if (pCh->tenv_pwm_delta == 0)
        strcpy(val, "off");
      else
        sprintf(val, "%+d", pCh->tenv_pwm_delta);
      break;
    case IR_TPWM_R:
      strcpy(label, "PWM rate");
      sprintf(val, "%d", pCh->tenv_pwm_rate);
      *barCur = pCh->tenv_pwm_rate; *barMax = 15;
      break;
    case IR_TSLP_D:
      strcpy(label, "Slope delta");
      if (pCh->tenv_slope_delta == 0)
        strcpy(val, "off");
      else
        sprintf(val, "%+d", pCh->tenv_slope_delta);
      break;
    case IR_TSLP_R:
      strcpy(label, "Slope rate");
      sprintf(val, "%d", pCh->tenv_slope_rate);
      *barCur = pCh->tenv_slope_rate; *barMax = 15;
      break;
    case IR_TDIR:
      strcpy(label, "Slope dir");
      strcpy(val, dirs[pCh->tenv_slope_dir & 3]);
      break;
    case IR_VIB_RATE:
      strcpy(label, "Rate");
      if (pCh->vib_rate == 0)
        strcpy(val, "off");
      else
        sprintf(val, "%d.%dHz", pCh->vib_rate / 10, pCh->vib_rate % 10);
      *barCur = pCh->vib_rate; *barMax = 255;
      break;
    case IR_VIB_DEPTH:
      strcpy(label, "Depth");
      if (pCh->vib_depth == 0)
        strcpy(val, "off");
      else
        sprintf(val, "%d cents", pCh->vib_depth);
      *barCur = pCh->vib_depth; *barMax = 127;
      break;
    case IR_VIB_DELAY:
      strcpy(label, "Delay");
      sprintf(val, "%dms", pCh->vib_delay_ms);
      *barCur = pCh->vib_delay_ms; *barMax = 5000;
      break;
  }
}

// Turn one knob.  dir is -1/+1; coarse uses the bigger step.  Applies
// straight to the driver so the change is audible immediately.
void CPwlSynth::InstrAdjust(int id, int dir, bool coarse)
{
  pwl_channel_state_t *pCh = &pwl_ch[s_InstrCh];
  int ch = s_InstrCh;
  int v;

  switch (id)
  {
    case IR_CHAN:
      InstrSelectChannel(instr_clamp(ch + dir, 0, PWL_NUM_CHANNELS - 1));
      return;
    case IR_GATE:
      InstrGate(!pCh->on);
      break;
    case IR_NOTE:
      v = instr_clamp(s_InstrNote[ch] + dir * (coarse ? 12 : 1), 11, 106);
      s_InstrNote[ch] = (uint8_t)v;
      if (pCh->on)
        pwl_note_on(ch, v, s_InstrVel[ch]);
      break;
    case IR_VEL:
      v = instr_clamp(s_InstrVel[ch] + dir * (coarse ? 8 : 1), 1, 63);
      s_InstrVel[ch] = (uint8_t)v;
      if (pCh->on)
        pwl_note_on(ch, s_InstrNote[ch], v);
      break;
    case IR_DUR:
      v = instr_clamp(s_InstrDur[ch] + dir * (coarse ? 250 : 50), 50, 5000);
      s_InstrDur[ch] = (uint16_t)v;
      break;
    case IR_PRESET:
      v = pwl_match_preset(ch);
      if (v < 0)
        v = dir > 0 ? 0 : pwl_preset_count() - 1;
      else
        v = (v + dir + pwl_preset_count()) % pwl_preset_count();
      pwl_apply_preset(ch, v);
      s_InstrDirty |= (1u << IR_SLOPE_R) | (1u << IR_SLOPE_F) |
                      (1u << IR_PWM);
      break;
    case IR_DETUNE:
      v = (pCh->relative_detune == PWL_DETUNE_OFF) ? -25
                                                   : pCh->relative_detune;
      v = instr_clamp(v + dir * (coarse ? 6 : 1), -25, 24);
      pCh->relative_detune = (int8_t)(v == -25 ? PWL_DETUNE_OFF : v);
      if (pCh->on)                      // detune applies at note-on
        pwl_note_on(ch, s_InstrNote[ch], s_InstrVel[ch]);
      break;
    case IR_SLOPE_R:
    case IR_SLOPE_F:
    case IR_PWM:
    {
      int r = pCh->slope_r, f = pCh->slope_f, w = pCh->pwm_offset;
      int step = dir * (coarse ? 16 : 1);

      if (id == IR_SLOPE_R) r = instr_clamp(r + step, 0, 255);
      if (id == IR_SLOPE_F) f = instr_clamp(f + step, 0, 255);
      if (id == IR_PWM)     w = instr_clamp(w + step, 0, 255);
      pwl_set_waveform(ch, (uint8_t)r, (uint8_t)f, (uint8_t)w, pCh->mode);
      s_InstrDirty |= 1u << IR_PRESET;  // may have become 'custom'
      break;
    }
    case IR_ENV_EN:
      if (pCh->env_enabled)
        pwl_adsr_off(ch);
      else
      {
        if (pCh->env_a == 0 && pCh->env_d == 0 &&
            pCh->env_s == 0 && pCh->env_r == 0)
        {
          // first enable: seed something audible
          pCh->env_a = 5; pCh->env_d = 12; pCh->env_s = 5; pCh->env_r = 10;
        }
        pwl_set_adsr(ch, pCh->env_a, pCh->env_d, pCh->env_s, pCh->env_r);
      }
      s_InstrDirty |= (1u << IR_ATK) | (1u << IR_DEC) |
                      (1u << IR_SUS) | (1u << IR_REL);
      break;
    case IR_ATK: case IR_DEC: case IR_SUS: case IR_REL:
    {
      uint8_t *pv = (id == IR_ATK) ? &pCh->env_a
                  : (id == IR_DEC) ? &pCh->env_d
                  : (id == IR_SUS) ? &pCh->env_s : &pCh->env_r;

      *pv = (uint8_t)instr_clamp(*pv + dir, 0, id == IR_SUS ? 7 : 15);
      if (pCh->env_enabled)
        pwl_set_adsr(ch, pCh->env_a, pCh->env_d, pCh->env_s, pCh->env_r);
      break;
    }
    case IR_POFF:
      v = instr_clamp(pCh->penv_offset + dir * (coarse ? 12 : 1), -24, 24);
      if (v != 0 && pCh->penv_rate == 0)
      {
        pCh->penv_rate = 8;             // audible default
        s_InstrDirty |= 1u << IR_PRATE;
      }
      pwl_set_pitch_env(ch, (int8_t)v, pCh->penv_rate,
                        pCh->penv_rel_rate, pCh->penv_rel_up != 0);
      break;
    case IR_PRATE:
      pwl_set_pitch_env(ch, pCh->penv_offset,
                        (uint8_t)instr_clamp(pCh->penv_rate + dir, 0, 15),
                        pCh->penv_rel_rate, pCh->penv_rel_up != 0);
      break;
    case IR_PREL:
      pwl_set_pitch_env(ch, pCh->penv_offset, pCh->penv_rate,
                        (uint8_t)instr_clamp(pCh->penv_rel_rate + dir, 0, 15),
                        pCh->penv_rel_up != 0);
      break;
    case IR_TPWM_D:
      v = instr_clamp(pCh->tenv_pwm_delta + dir * (coarse ? 50 : 10),
                      -250, 250);
      if (v != 0 && pCh->tenv_pwm_rate == 0)
      {
        pCh->tenv_pwm_rate = 10;
        s_InstrDirty |= 1u << IR_TPWM_R;
      }
      pwl_set_timbre_env(ch, (int16_t)v, pCh->tenv_pwm_rate,
                         pCh->tenv_slope_delta, pCh->tenv_slope_rate,
                         pCh->tenv_slope_dir);
      break;
    case IR_TPWM_R:
      pwl_set_timbre_env(ch, pCh->tenv_pwm_delta,
                         (uint8_t)instr_clamp(pCh->tenv_pwm_rate + dir, 0, 15),
                         pCh->tenv_slope_delta, pCh->tenv_slope_rate,
                         pCh->tenv_slope_dir);
      break;
    case IR_TSLP_D:
      v = instr_clamp(pCh->tenv_slope_delta + dir * (coarse ? 16 : 4),
                      -120, 120);
      if (v != 0 && pCh->tenv_slope_rate == 0)
      {
        pCh->tenv_slope_rate = 9;
        s_InstrDirty |= 1u << IR_TSLP_R;
      }
      pwl_set_timbre_env(ch, pCh->tenv_pwm_delta, pCh->tenv_pwm_rate,
                         (int8_t)v, pCh->tenv_slope_rate,
                         pCh->tenv_slope_dir);
      break;
    case IR_TSLP_R:
      pwl_set_timbre_env(ch, pCh->tenv_pwm_delta, pCh->tenv_pwm_rate,
                         pCh->tenv_slope_delta,
                         (uint8_t)instr_clamp(pCh->tenv_slope_rate + dir,
                                              0, 15),
                         pCh->tenv_slope_dir);
      break;
    case IR_TDIR:
      pwl_set_timbre_env(ch, pCh->tenv_pwm_delta, pCh->tenv_pwm_rate,
                         pCh->tenv_slope_delta, pCh->tenv_slope_rate,
                         (uint8_t)((pCh->tenv_slope_dir + dir) & 3));
      break;
    case IR_VIB_RATE:
      v = pCh->vib_rate + dir * (coarse ? 10 : 5);
      if (v < 10)                       // 0 = off, else the 1.0Hz floor
        v = dir < 0 ? 0 : 10;
      v = instr_clamp(v, 0, 255);
      if (v != 0 && pCh->vib_depth == 0)
      {
        pCh->vib_depth = 26;            // audible default
        s_InstrDirty |= 1u << IR_VIB_DEPTH;
      }
      pwl_set_vibrato(ch, (uint8_t)v, pCh->vib_depth, pCh->vib_delay_ms);
      pwl_vibrato_arm(ch, s_InstrNote[ch]);     // bloom into a held note
      break;
    case IR_VIB_DEPTH:
      v = instr_clamp(pCh->vib_depth + dir * (coarse ? 10 : 2), 0, 127);
      if (v != 0 && pCh->vib_rate == 0)
      {
        pCh->vib_rate = 60;             // 6Hz default
        s_InstrDirty |= 1u << IR_VIB_RATE;
      }
      pwl_set_vibrato(ch, pCh->vib_rate, (uint8_t)v, pCh->vib_delay_ms);
      pwl_vibrato_arm(ch, s_InstrNote[ch]);
      break;
    case IR_VIB_DELAY:
      // takes effect live: the service reads the delay each tick
      v = instr_clamp(pCh->vib_delay_ms + dir * (coarse ? 250 : 50), 0, 5000);
      pwl_set_vibrato(ch, pCh->vib_rate, pCh->vib_depth, (uint16_t)v);
      break;
  }

  // NOTE: no watch-window refresh here.  The ~200ms idle timer picks
  // it up; refreshing per keystroke floods the UART and the design
  // drops the NEXT key while still transmitting (1-byte RX, no IRQ).
}

// Repaint a single parameter row (and the title) in place.  Assumes
// the panel is already fully drawn and the layout/topLine unchanged -
// tiny UART traffic per keystroke.
void CPwlSynth::InstrRowRepaint(int id)
{
  CTab   *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  WINDOW *pWnd;
  char    label[24], val[48];
  int     i, line, rows, cols, barCur, barMax, topLine;

  if (pTab == NULL || pTab->SourceContext() != &s_InstrCtxMarker)
    return;
  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return;
  getmaxyx(pWnd, rows, cols);
  topLine = pTab->SourceFirstLine();

  // Bright white, not the frame's leftover dim grey (init_pair only
  // dirties the screen when the colors actually change)
  init_pair(INSTR_PAIR, (short)(COLOR_WHITE | 8), COLOR_BLACK);
  wattrset(pWnd, COLOR_PAIR(INSTR_PAIR));

  // Title (gate / channel / note state lives there)
  line = -topLine;
  if (line >= 0 && line < rows)
  {
    wmove(pWnd, line, 1);
    wclrtoeol(pWnd);
    wattron(pWnd, A_BOLD);
    mvwprintw(pWnd, line, 1, "INSTRUMENT   ch %d   %s vel %d   %s",
              s_InstrCh, note_name(s_InstrNote[s_InstrCh]),
              s_InstrVel[s_InstrCh],
              pwl_ch[s_InstrCh].on ? "[PLAYING]" : "[silent]");
    wattroff(pWnd, A_BOLD);
  }

  for (i = 0; i < INSTR_LINES; i++)
  {
    if (s_InstrLayout[i].id != id)
      continue;
    line = i + 2 - topLine;
    if (line < 0 || line >= rows - 1)
      break;

    InstrRowText(id, label, val, &barCur, &barMax);
    if (id == s_InstrSel)
      wattron(pWnd, A_REVERSE);
    mvwprintw(pWnd, line, 2, "%-12s %-14s", label, val);
    if (id == s_InstrSel)
      wattroff(pWnd, A_REVERSE);
    if (barMax > 0 && cols > 48)
    {
      int bw = cols - 34;
      int b, fill;

      if (bw > 40)
        bw = 40;
      fill = barCur * bw / barMax;
      mvwaddch(pWnd, line, 30, '[');
      for (b = 0; b < bw; b++)
        waddch(pWnd, b < fill ? '=' : ' ');
      waddch(pWnd, ']');
    }
    break;
  }
  wrefresh(pWnd);
}

// Repaint just the recordings footer (r/X/[/] change nothing else, so
// a full panel emit would only be UART noise)
void CPwlSynth::InstrRecsRepaint(void)
{
  CTab   *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  WINDOW *pWnd;
  int     rows, cols, line;

  if (pTab == NULL || pTab->SourceContext() != &s_InstrCtxMarker)
    return;
  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return;
  getmaxyx(pWnd, rows, cols);
  (void)cols;
  line = INSTR_LINES + 3 - pTab->SourceFirstLine();
  if (line < 0 || line >= rows)
    return;
  init_pair(INSTR_PAIR, (short)(COLOR_WHITE | 8), COLOR_BLACK);
  wattrset(pWnd, COLOR_PAIR(INSTR_PAIR));
  studio_draw_recs(pWnd, line);
  wrefresh(pWnd);
}

// Same, for the song status line below it
void CPwlSynth::InstrSongRepaint(void)
{
  CTab   *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  WINDOW *pWnd;
  int     rows, cols, line;

  if (pTab == NULL || pTab->SourceContext() != &s_InstrCtxMarker)
    return;
  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return;
  getmaxyx(pWnd, rows, cols);
  (void)cols;
  line = INSTR_LINES + 4 - pTab->SourceFirstLine();
  if (line < 0 || line >= rows)
    return;
  init_pair(INSTR_PAIR, (short)(COLOR_WHITE | 8), COLOR_BLACK);
  wattrset(pWnd, COLOR_PAIR(INSTR_PAIR));
  studio_draw_song(pWnd, line);
  wrefresh(pWnd);
}

int CPwlSynth::ProcessKey(int key)
{
  int prevSel = s_InstrSel;
  int i, cur = 0;

  // A pending append-or-overwrite question eats the next key
  if (s_SongAsk)
  {
    s_SongAsk = 0;
    if (key == 'a' || key == 'A')
      song_start(0);
    else if (key == 'o' || key == 'O')
      song_start(1);
    InstrSongRepaint();
    s_InstrSkipDraw = 1;
    return 1;
  }

  for (i = 0; i < INSTR_LINES; i++)
    if (s_InstrLayout[i].id == s_InstrSel)
      cur = i;

  switch (key)
  {
    case KEY_UP:
      do
        cur = (cur + INSTR_LINES - 1) % INSTR_LINES;
      while (s_InstrLayout[cur].id < 0);
      s_InstrSel = s_InstrLayout[cur].id;
      break;
    case KEY_DOWN:
      do
        cur = (cur + 1) % INSTR_LINES;
      while (s_InstrLayout[cur].id < 0);
      s_InstrSel = s_InstrLayout[cur].id;
      break;
    case KEY_LEFT:  InstrAdjust(s_InstrSel, -1, false); break;
    case KEY_RIGHT: InstrAdjust(s_InstrSel,  1, false); break;
    case '<': InstrAdjust(s_InstrSel, -1, true); break;
    case '>': InstrAdjust(s_InstrSel,  1, true); break;
    case ' ':
      InstrGate(!pwl_ch[s_InstrCh].on);
      break;
    case 'p':                           // play (one-shot in timed mode)
      pwl_note_on(s_InstrCh, s_InstrNote[s_InstrCh], s_InstrVel[s_InstrCh]);
      if (s_InstrCont[s_InstrCh])
        s_InstrOffPending[s_InstrCh] = 0;
      else
      {
        // play-for-duration: schedule the gate-off (IdlePoll fires it)
        s_InstrOffAt[s_InstrCh] = read_time() +
                                  (uint32_t)s_InstrDur[s_InstrCh] * 1000u;
        s_InstrOffPending[s_InstrCh] = 1;
      }
      break;
    case 'n':                           // "note mode": continuous <-> timed
      s_InstrCont[s_InstrCh] ^= 1;
      break;
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
    {
      // letter picks the pitch class within the displayed octave
      static const int8_t cls[7] = { 9, 11, 0, 2, 4, 5, 7 };

      InstrSetNote((s_InstrNote[s_InstrCh] / 12) * 12 + cls[key - 'a']);
      break;
    }
    case '#':                           // sharp <-> natural (E/B: no-op)
    {
      int  m = s_InstrNote[s_InstrCh];
      bool sharp = strchr(note_name(m), '#') != NULL;

      if (sharp)
        InstrSetNote(m - 1);
      else if (strchr(note_name(m + 1), '#') != NULL)
        InstrSetNote(m + 1);
      break;
    }
    case '+':
      InstrSetNote(s_InstrNote[s_InstrCh] + 12);
      break;
    case '-':
      InstrSetNote(s_InstrNote[s_InstrCh] - 12);
      break;
    case 's':                           // silence everything
      for (i = 0; i < PWL_NUM_CHANNELS; i++)
        s_InstrOffPending[i] = 0;
      pwl_all_off();
      break;
    case '0': case '1': case '2': case '3':
      InstrSelectChannel(key - '0');
      break;
    case 'r':                           // record the current sound
      if (s_StudioCount >= STUDIO_MAX_RECS)
        break;
      studio_capture(&s_StudioRecs[s_StudioCount]);
      s_StudioSel = s_StudioCount++;
      studio_touch();
      InstrRecsRepaint();
      s_InstrSkipDraw = 1;
      return 1;
    case 'u':                           // update the selection in place
      if (s_StudioCount == 0)
        break;
      studio_capture(&s_StudioRecs[s_StudioSel]);
      studio_touch();
      InstrRecsRepaint();               // the '*': proof the key landed
      s_InstrSkipDraw = 1;
      return 1;
    case 'X':                           // delete the selected recording
      if (s_StudioCount == 0)
        break;
      for (i = s_StudioSel; i < s_StudioCount - 1; i++)
        s_StudioRecs[i] = s_StudioRecs[i + 1];
      s_StudioCount--;
      if (s_StudioSel >= s_StudioCount && s_StudioSel > 0)
        s_StudioSel--;
      studio_touch();
      InstrRecsRepaint();
      s_InstrSkipDraw = 1;
      return 1;
    case ',': case '.':                 // select left/right and restore
    case 'h': case 'l':                 // select left/right and restore
      if (s_StudioCount == 0)
        break;
      s_StudioSel = instr_clamp(s_StudioSel + ((key == '.' || key == 'l') ? 1 : -1),
                                0, s_StudioCount - 1);
      studio_apply(&s_StudioRecs[s_StudioSel]);   // at the ends: re-apply
      studio_touch();                   // the file remembers SEL
      if (s_SongRecording && !s_SongPlaying)
        song_tap(key);                  // ,/. are part of the take
      return 1;                         // full redraw - every row changed
    case 'R':                           // record a performance take
      if (s_SongPlaying)
        break;
      if (s_SongRecording)
      {
        s_SongRecording = 0;            // stop -> autosave shortly
        song_touch();
      }
      else if (s_SongCount > 0)
        s_SongAsk = 1;                  // the next key answers (a/o)
      else
        song_start(1);
      InstrSongRepaint();
      s_InstrSkipDraw = 1;
      return 1;
    case 'P':                           // replay the take
      if (s_SongRecording)
        break;
      if (s_SongPlaying)
        s_SongPlaying = 0;
      else if (s_SongCount > 0)
      {
        s_InstrOffPending[s_InstrCh] = 0;   // deterministic start:
        pwl_note_off(s_InstrCh);            //   the gate begins closed
        s_SongIdx = 0;
        s_SongPlaying = 1;
        s_SongNextAt = read_time() + (uint32_t)s_SongEvs[0].dt * 1000u;
      }
      InstrSongRepaint();
      s_InstrSkipDraw = 1;
      return 1;
    case '[': case ']':                 // drag the selection along the list
    {
      int j = s_StudioSel + (key == ']' ? 1 : -1);

      if (s_StudioCount < 2 || j < 0 || j >= s_StudioCount)
        break;

      StudioRec_t t = s_StudioRecs[j];

      s_StudioRecs[j] = s_StudioRecs[s_StudioSel];
      s_StudioRecs[s_StudioSel] = t;
      s_StudioSel = j;
      studio_touch();
      InstrRecsRepaint();
      s_InstrSkipDraw = 1;
      return 1;
    }
    default:
      return 0;                         // not ours: normal handling
  }

  // Song recorder: performance keys only (knob moves, channel switches
  // and the sound-list editing keys stay out of the take)
  if (s_SongRecording && !s_SongPlaying &&
      (key == ' ' || key == 'p' || key == 's' || key == '#' ||
       key == '+' || key == '-' || (key >= 'a' && key <= 'g')))
  {
    song_tap(key);
    InstrSongRepaint();
  }

  // Repaint only what a key can change: the previous and current
  // selection rows (covers value edits too - the value lives in the
  // selected row) plus the title via InstrRowRepaint.  Channel
  // switches change every row, so let SetFocus do the full redraw.
  if (key >= '0' && key <= '3')
    return 1;

  // Selection moved outside the visible window (row repaints never
  // scroll): take the full-redraw path, which follows the selection.
  // Costs a full panel emit, but only when crossing the window edge.
  if (s_InstrSel != prevSel)
  {
    CTab *pTab = m_pParent->GetActiveSrcTab();

    if (pTab != NULL && pTab->GetWindow() != NULL)
    {
      int rows, cols, selLine, topLine;

      getmaxyx(pTab->GetWindow(), rows, cols);
      (void)cols;
      topLine = pTab->SourceFirstLine();
      selLine = 2;
      for (i = 0; i < INSTR_LINES; i++)
        if (s_InstrLayout[i].id == s_InstrSel)
          selLine = i + 2;
      if (selLine - topLine > rows - 2 || selLine - topLine < 0)
        return 1;                       // no skip: SetFocus scrolls + redraws
    }
  }
  InstrRowRepaint(prevSel);
  if (s_InstrSel != prevSel)
    InstrRowRepaint(s_InstrSel);
  if (s_InstrDirty)
  {
    // rows the adjustment changed as a side effect (preset -> slopes,
    // envelope enable -> seeded A/D/S/R, delta -> auto-seeded rate)
    for (i = 0; i < IR_COUNT; i++)
      if (s_InstrDirty & (1u << i))
        InstrRowRepaint(i);
    s_InstrDirty = 0;
  }
  if (key == ' ' || key == 'p' || key == 's')
    InstrRowRepaint(IR_GATE);           // gate state lives off-selection
  if (key == 'n')
    InstrRowRepaint(IR_DUR);            // mode shows on the Duration row
  if ((key >= 'a' && key <= 'g') || key == '#' || key == '+' || key == '-')
  {
    InstrRowRepaint(IR_NOTE);           // musical keys retune off-selection
    InstrRowRepaint(IR_GATE);           //   ...and audition in note mode
  }
  s_InstrSkipDraw = 1;
  return 1;                             // consumed
}

void CPwlSynth::DrawInstrPanel(WINDOW *pWnd, int topLine)
{
  CTab *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  char  label[24], val[48];
  int   rows, cols, line, i, selLine, barCur, barMax;

  if (pWnd == NULL)
    return;
  getmaxyx(pWnd, rows, cols);

  // Bright white for every panel row (see InstrRowRepaint)
  init_pair(INSTR_PAIR, (short)(COLOR_WHITE | 8), COLOR_BLACK);
  wattrset(pWnd, COLOR_PAIR(INSTR_PAIR));

  // Keep the selection visible (title takes the top two lines)
  selLine = 2;
  for (i = 0; i < INSTR_LINES; i++)
    if (s_InstrLayout[i].id == s_InstrSel)
      selLine = i + 2;
  if (pTab != NULL && pTab->SourceContext() == &s_InstrCtxMarker)
  {
    topLine = pTab->SourceFirstLine();
    if (selLine - topLine > rows - 2)
      topLine = selLine - (rows - 2);
    if (selLine - topLine < 0)
      topLine = selLine;
    if (topLine < 0)
      topLine = 0;
    pTab->SourceFirstLine(topLine);
  }

  werase(pWnd);
  line = -topLine;

  if (line >= 0 && line < rows)
  {
    wattron(pWnd, A_BOLD);
    mvwprintw(pWnd, line, 1, "INSTRUMENT   ch %d   %s vel %d   %s",
              s_InstrCh, note_name(s_InstrNote[s_InstrCh]),
              s_InstrVel[s_InstrCh],
              pwl_ch[s_InstrCh].on ? "[PLAYING]" : "[silent]");
    wattroff(pWnd, A_BOLD);
  }
  line += 2;

  for (i = 0; i < INSTR_LINES; i++, line++)
  {
    if (line < 0 || line >= rows - 1)
      continue;

    if (s_InstrLayout[i].id < 0)
    {
      mvwprintw(pWnd, line, 1, "--- %s ---", s_InstrLayout[i].hdr);
      continue;
    }

    InstrRowText(s_InstrLayout[i].id, label, val, &barCur, &barMax);

    if (s_InstrLayout[i].id == s_InstrSel)
      wattron(pWnd, A_REVERSE);
    mvwprintw(pWnd, line, 2, "%-12s %-14s", label, val);
    if (s_InstrLayout[i].id == s_InstrSel)
      wattroff(pWnd, A_REVERSE);
    wattrset(pWnd, COLOR_PAIR(INSTR_PAIR));

    if (barMax > 0 && cols > 48)
    {
      int bw = cols - 34;
      int b, fill;

      if (bw > 40)
        bw = 40;
      fill = barCur * bw / barMax;
      mvwaddch(pWnd, line, 30, '[');
      for (b = 0; b < bw; b++)
        waddch(pWnd, b < fill ? '=' : ' ');
      waddch(pWnd, ']');
    }
  }

  if (line >= 0 && line < rows)
    mvwprintw(pWnd, line, 1, "Up/Dn  Lt/Rt adjust  </> coarse  SPACE gate  "
                             "p play  s silence  n cont/timed  "
                             "a-g # +/- note  0-3 chan  ^W focus");
  line++;
  if (line >= 0 && line < rows)
    studio_draw_recs(pWnd, line);
  line++;
  if (line >= 0 && line < rows)
    studio_draw_song(pWnd, line);

  wrefresh(pWnd);
}

/*
==============================================================================
Command handling
==============================================================================
*/

const TuiCmd_t *CPwlSynth::GetCommandTable(void)
{
  return (const TuiCmd_t *)&s_TuiCmds[0];
}

int CPwlSynth::Legacy(int argc, char *argv[])
{
  // Demos stream their note progression to the Notes tab - bring it up
  // front before they start playing (unless asked to play quietly: the
  // per-note drawing is exactly what 'q' avoids)
  if (strcmp(argv[0], "demo") == 0 &&
      !(argc > 2 && seq_quiet_arg(argv[2])))
    EnsureNotesTab();

  // Forward to the plain console dispatcher; its printf output comes
  // back through the stdout hook into the command window.
  cli_execute(argc, argv);
  FlushStdoutLine();

  // Almost every command here changes a synth register: refresh the
  // watch window now rather than waiting for the idle poll.
  m_pParent->UpdateWatchWindows();
  return OK;
}

// One help row, paged against the command window.  Returns 0 once the
// user has asked to stop.
int CPwlSynth::HelpEmit(const char *usage, const char *help,
                        int &onPage, int rows)
{
  char line[110];

  snprintf(line, sizeof(line), "  %-25s %s", usage, help);
  m_pParent->UICommandPrintString(line);

  // rows - 1: the pause prompt itself takes the last line of the window,
  // so a full page plus the prompt is exactly what fits
  if (++onPage >= rows - 1)
  {
    onPage = 0;
    if (m_pParent->PauseCmdListing() == 'q')
      return 0;
  }
  return 1;
}

int CPwlSynth::Help(int argc, char *argv[])
{
  const PwlCmd_t *pCmd;
  char            line[110];
  int             rows, cols, onPage = 0, len;

  if (argc > 1 && strcmp(argv[1], "cli") == 0)
  {
    char *help[2];
    char  cmd[8];

    strcpy(cmd, "help");                   // console.c help text
    help[0] = cmd;
    help[1] = NULL;
    return Legacy(1, help);
  }

  // The command window is what the listing has to fit; it changes with
  // ALT-+/ALT-- and with the terminal, so ask for it every time
  m_pParent->GetCmdWinGeometry(rows, cols);
  (void)cols;
  if (rows < 3)
    rows = 3;                              // always make forward progress

  // 'help <name>': that command alone.  An exact name wins outright;
  // otherwise every command the text prefixes is shown, so 'help c'
  // still surveys the c's.
  if (argc > 1)
  {
    int matched = 0;

    len = (int)strlen(argv[1]);
    for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
      if (strcmp(pCmd->name, argv[1]) == 0)
      {
        HelpEmit(pCmd->usage, pCmd->help, onPage, rows);
        return OK;
      }

    for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
    {
      if (strncmp(pCmd->name, argv[1], len) != 0)
        continue;
      matched++;
      if (!HelpEmit(pCmd->usage, pCmd->help, onPage, rows))
        return OK;
    }
    if (matched == 0)
    {
      snprintf(line, sizeof(line), "No help for '%s' ('help' lists"
                                   " everything)", argv[1]);
      m_pParent->UICommandPrintString(line);
      return -1;
    }
    return OK;
  }

  for (pCmd = s_TuiCmds; pCmd->name != NULL; pCmd++)
    if (!HelpEmit(pCmd->usage, pCmd->help, onPage, rows))
      return OK;

  HelpEmit("exit | quit", "Leave the TUI (plain console resumes)",
           onPage, rows);
  return OK;
}

int CPwlSynth::Clear(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  m_pParent->m_CmdLineCount   = 0;
  m_pParent->m_CmdCurrentLine = 0;
  m_pParent->m_CmdTopLine     = 0;
  m_pParent->m_CmdPrevLine    = 0;
  werase(m_pParent->m_pCmdwin);
  m_pParent->RedrawCommandWindow();
  return OK;
}

int CPwlSynth::Notes(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  return EnsureNotesTab() != NULL ? OK : -1;
}

int CPwlSynth::Watch(int argc, char *argv[])
{
  int ch;

  if (argc < 2)
  {
    CmdPrintf("watch: channel %d (usage: watch <0-%d>)\n", m_WatchCh,
              PWL_NUM_CHANNELS - 1);
    return OK;
  }

  ch = atoi(argv[1]);
  if (ch < 0 || ch >= PWL_NUM_CHANNELS)
  {
    CmdPrintf("watch: channel must be 0-%d\n", PWL_NUM_CHANNELS - 1);
    return -1;
  }

  m_WatchCh = ch;
  m_pParent->UpdateWatchWindows();
  return OK;
}

/*
==============================================================================
MIDI file tab
==============================================================================
*/

// Contexts are either one of the two static markers or a CMidiFile the
// 'open' command allocated
bool CPwlSynth::IsMidiCtx(void *pCtx)
{
  return pCtx != NULL && pCtx != &s_NotesCtxMarker &&
         pCtx != &s_InstrCtxMarker;
}

int CPwlSynth::Open(int argc, char *argv[])
{
  CMidiFile *pMidi;
  CTab      *pTab;
  char       err[80];

  if (argc < 2)
  {
    CmdPrintf("usage: open <file.mid>   ('ls' lists the host files)\n");
    return -1;
  }

  // Reading comes off the host a chunk at a time, so a big file takes a
  // few seconds - say so before going quiet
  CmdPrintf("reading %s ...\n", argv[1]);
  m_pParent->RedrawCommandWindow();

  pMidi = new CMidiFile;
  if (pMidi == NULL)
  {
    CmdPrintf("out of memory\n");
    return -1;
  }
  err[0] = 0;
  if (!pMidi->Load(argv[1], err, (int)sizeof(err)))
  {
    CmdPrintf("%s\n", err[0] ? err : "load failed");
    delete pMidi;
    return -1;
  }

  CmdPrintf("%s: format %d, %d track%s, %d ticks/beat", pMidi->m_Title,
            pMidi->m_Format, pMidi->m_Tracks,
            pMidi->m_Tracks == 1 ? "" : "s", pMidi->m_Division);
  if (pMidi->TempoBpm() > 0)
    CmdPrintf(", %d bpm", pMidi->TempoBpm());
  CmdPrintf("\n");
  if (pMidi->m_FileTracks > pMidi->m_Tracks)
    CmdPrintf("(showing the first %d of %d tracks)\n", pMidi->m_Tracks,
              pMidi->m_FileTracks);

  // Conversion defaults, then whatever the file's .cfg remembers
  pMidi->m_Cvt.inst[M2P_ROLE_MEL] = (uint8_t)mid2pwl_inst_default(M2P_ROLE_MEL);
  pMidi->m_Cvt.inst[M2P_ROLE_BASS] = (uint8_t)mid2pwl_inst_default(M2P_ROLE_BASS);
  pMidi->m_Cvt.inst[M2P_ROLE_PAD] = (uint8_t)mid2pwl_inst_default(M2P_ROLE_PAD);
  if (pMidi->LoadCfg())
    CmdPrintf("loaded .cfg: mel=%s bass=%s pad=%s%s\n",
              mid2pwl_inst(pMidi->m_Cvt.inst[M2P_ROLE_MEL])->name,
              mid2pwl_inst(pMidi->m_Cvt.inst[M2P_ROLE_BASS])->name,
              mid2pwl_inst(pMidi->m_Cvt.inst[M2P_ROLE_PAD])->name,
              pMidi->m_Cvt.satb >= 0 ? " (satb)" : "");

  pTab = m_pParent->CreateNewTab(pMidi->m_Title);
  if (pTab == NULL)
  {
    delete pMidi;
    return -1;
  }
  pTab->AttachTuiSource(this, pMidi);
  pTab->SourceFirstLine(0);
  m_pParent->MakeTabActive(pTab);
  m_pParent->DrawSourceWindow();
  return OK;
}

/*
==============================================================================
Conversion commands.  All act on the active MIDI tab; every settings
change autosaves the .cfg (host filesystem) and repaints the footer.
==============================================================================
*/

CMidiFile *CPwlSynth::ActiveMidi(bool complain)
{
  CTab *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  void *pCtx = (pTab != NULL) ? pTab->SourceContext() : NULL;

  if (IsMidiCtx(pCtx))
    return (CMidiFile *)pCtx;

  // Not the active tab (playing raises the Notes tab, for one): fall
  // back to the only open MIDI tab; with several the user must pick.
  {
    CMidiFile *pOnly = NULL;
    int nMidi = 0;

    for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
         pTab = pTab->GetNextTab())
    {
      if (IsMidiCtx(pTab->SourceContext()))
      {
        pOnly = (CMidiFile *)pTab->SourceContext();
        nMidi++;
      }
    }
    if (nMidi == 1)
      return pOnly;
    if (complain)
      CmdPrintf(nMidi == 0
                  ? "no MIDI tab ('open <file.mid>' first)\n"
                  : "several MIDI tabs open: CTRL-T to the one you mean\n");
  }
  return NULL;
}

// Bring the tab that owns this file back in front (playback raises the
// Notes tab; the eye wants to land where the work is)
void CPwlSynth::ActivateMidiTab(CMidiFile *pMidi)
{
  CTab *pTab;

  for (pTab = m_pParent->GetFirstTab(); pTab != NULL;
       pTab = pTab->GetNextTab())
  {
    if (pTab->SourceContext() == pMidi)
    {
      m_pParent->MakeTabActive(pTab);
      m_pParent->DrawSourceWindow();
      return;
    }
  }
}

void CPwlSynth::CvtChanged(CMidiFile *pMidi)
{
  pMidi->SaveCfg();

  // The RAM conversion no longer matches the settings: drop it so the
  // next 'play' rebuilds.  (Keeping it made instrument changes silently
  // inaudible - play kept replaying the stale table.)
  free(pMidi->m_Seq);
  pMidi->m_Seq = NULL;
  pMidi->m_SeqCount = 0;
  pMidi->m_Saved = 0;

  m_pParent->DrawSourceWindow();        // footer shows the new settings
}

int CPwlSynth::Automap(int argc, char *argv[])
{
  static char report[1200];
  CMidiFile *pMidi = ActiveMidi(true);
  char *line, *nl;
  int ret;

  (void)argc;
  (void)argv;
  if (pMidi == NULL)
    return -1;

  ret = mid2pwl_automap(pMidi, report, (int)sizeof(report));

  // One CmdPrintf per line (its buffer is smaller than the report)
  for (line = report; line != NULL && *line != 0; line = nl)
  {
    if ((nl = strchr(line, '\n')) != NULL)
      *nl++ = 0;
    CmdPrintf("%s\n", line);
  }

  if (ret < 0)
    return -1;
  CmdPrintf("('map'/'inst' adjust, then 'play')\n");
  CvtChanged(pMidi);                    // autosave + footer + reconvert
  return OK;
}

// amp ch<N> [pct[%]]: per-MIDI-channel volume for the conversion,
// layered on the role's velocity mapping - ch7 at 50% plays half as
// loud wherever its notes land, melody fills included.  Any other
// first argument is the plain hardware amp command, passed through.
int CPwlSynth::Amp(int argc, char *argv[])
{
  CMidiFile *pMidi;
  MidiCvt_t *cvt;
  char       list[80];
  int        c, i, n;

  if (argc < 2 || argv[1][0] != 'c' || argv[1][1] != 'h' ||
      argv[1][2] < '0' || argv[1][2] > '9')
    return Legacy(argc, argv);

  if ((pMidi = ActiveMidi(true)) == NULL)
    return -1;
  cvt = &pMidi->m_Cvt;
  c = atoi(&argv[1][2]);
  if (c > 15)
  {
    CmdPrintf("MIDI channels are 0-15\n");
    return -1;
  }
  if (argc >= 3)
  {
    int pct = atoi(argv[2]);

    if (pct < 0 || pct > 200)
    {
      CmdPrintf("gain is 0-200%%\n");
      return -1;
    }
    cvt->chan_gain[c] = (uint8_t)pct;
    CvtChanged(pMidi);
  }
  n = 0;
  list[0] = 0;
  for (i = 0; i < 16; i++)
    if (cvt->chan_gain[i] != 100)
      n += snprintf(&list[n], sizeof(list) - n, " ch%d=%u%%", i,
                    cvt->chan_gain[i]);
  CmdPrintf("gain:%s\n", list[0] ? list : " all channels 100%");
  return 0;
}

int CPwlSynth::Map(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  MidiCvt_t *cvt;
  int ch;

  if (pMidi == NULL)
    return -1;
  cvt = &pMidi->m_Cvt;

  if (argc < 3)
  {
    CmdPrintf("usage: map melody <ch[,ch..]|+ch|-ch> | map"
              " bass|pad|drums|satb <ch|off>\n(+ch/-ch add/remove one"
              " melody source; 'satb' splits one channel onto all 4"
              " voices)\n");
    return -1;
  }

  ch = strcmp(argv[2], "off") == 0 ? -1 : atoi(argv[2]);
  if (ch > 15)
  {
    CmdPrintf("MIDI channels are 0-15\n");
    return -1;
  }

  switch (argv[1][0])
  {
    case 'm':                           // melody: comma list, or +ch/-ch
    {
      int i, j;

      if (argv[2][0] == '+' || argv[2][0] == '-')
      {
        // Edit the existing priority list in place
        int c = atoi(&argv[2][1]);

        if (c < 0 || c > 15)
        {
          CmdPrintf("MIDI channels are 0-15\n");
          return -1;
        }
        for (i = 0; i < MIDI_MEL_SRCS; i++)
          if (cvt->melody[i] == c)
            break;
        if (argv[2][0] == '+')
        {
          if (i < MIDI_MEL_SRCS)
          {
            CmdPrintf("ch%d is already a melody source\n", c);
            return -1;
          }
          for (i = 0; i < MIDI_MEL_SRCS; i++)
            if (cvt->melody[i] < 0)
            {
              cvt->melody[i] = (int8_t)c;
              break;
            }
          if (i == MIDI_MEL_SRCS)
          {
            CmdPrintf("melody list is full (%d sources)\n", MIDI_MEL_SRCS);
            return -1;
          }
        }
        else
        {
          if (i == MIDI_MEL_SRCS)
          {
            CmdPrintf("ch%d is not in the melody list\n", c);
            return -1;
          }
          // drop it, keeping the others' priority order
          for (j = i; j < MIDI_MEL_SRCS - 1; j++)
            cvt->melody[j] = cvt->melody[j + 1];
          cvt->melody[MIDI_MEL_SRCS - 1] = -1;
        }
      }
      else
      {
        char *tok = strtok(argv[2], ",");

        for (i = 0; i < MIDI_MEL_SRCS; i++)
          cvt->melody[i] = -1;
        for (i = 0; tok != NULL && i < MIDI_MEL_SRCS; i++)
        {
          cvt->melody[i] = (int8_t)(strcmp(tok, "off") == 0 ? -1 : atoi(tok));
          tok = strtok(NULL, ",");
        }
      }

      // say what the list is now (the footer may be scrolled away)
      {
        char list[24];
        int n = 0;

        list[0] = 0;
        for (i = 0; i < MIDI_MEL_SRCS && cvt->melody[i] >= 0; i++)
          n += snprintf(&list[n], sizeof(list) - n, "%s%d", i ? "," : "",
                        cvt->melody[i]);
        CmdPrintf("melody: %s\n", list[0] ? list : "(none)");
      }
      break;
    }
    case 'b': cvt->bass = (int8_t)ch; break;
    case 'p': cvt->pad = (int8_t)ch; break;
    case 'd': cvt->drums = (int8_t)ch; break;
    case 's': cvt->satb = (int8_t)ch; break;
    default:
      CmdPrintf("roles: melody bass pad drums satb\n");
      return -1;
  }

  CvtChanged(pMidi);
  return OK;
}

int CPwlSynth::Inst(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  int role, idx;

  if (pMidi == NULL)
    return -1;

  if (argc < 3)
  {
    char line[160];                     // the whole roster must fit
    int n = 0;

    line[0] = 0;
    for (idx = 0; idx < mid2pwl_inst_count(); idx++)
    {
      n += snprintf(&line[n], sizeof(line) - n, "%s%s", idx ? " " : "",
                    mid2pwl_inst(idx)->name);
      if (n > (int)sizeof(line) - 12)
        break;
    }
    CmdPrintf("usage: inst melody|bass|pad <name> | inst ch<N> <name|off>\n"
              "instruments: %s\n", line);
    return -1;
  }

  // 'inst ch<N> <name|off>': pin an instrument to a MIDI channel.  The
  // conversion reprograms the voice whenever a note from that channel
  // plays, and falls back to the role instrument for everything else.
  if (argv[1][0] == 'c' && argv[1][1] == 'h' &&
      argv[1][2] >= '0' && argv[1][2] <= '9')
  {
    char list[100];
    int c = atoi(&argv[1][2]), i, n;

    if (c > 15)
    {
      CmdPrintf("MIDI channels are 0-15\n");
      return -1;
    }
    if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "-") == 0)
      pMidi->m_Cvt.chan_inst[c] = -1;
    else
    {
      idx = mid2pwl_inst_find(argv[2]);
      if (idx < 0)
      {
        CmdPrintf("unknown instrument '%s' ('inst' lists them)\n", argv[2]);
        return -1;
      }
      pMidi->m_Cvt.chan_inst[c] = (int8_t)idx;
    }
    n = 0;
    list[0] = 0;
    for (i = 0; i < 16; i++)
      if (pMidi->m_Cvt.chan_inst[i] >= 0)
        n += snprintf(&list[n], sizeof(list) - n, " ch%d:%s", i,
                      mid2pwl_inst(pMidi->m_Cvt.chan_inst[i])->name);
    CmdPrintf("channel instruments:%s\n", list[0] ? list : " (none)");
    CvtChanged(pMidi);
    return OK;
  }

  role = argv[1][0] == 'b' ? M2P_ROLE_BASS
       : argv[1][0] == 'p' ? M2P_ROLE_PAD : M2P_ROLE_MEL;
  idx = mid2pwl_inst_find(argv[2]);
  if (idx < 0)
  {
    CmdPrintf("unknown instrument '%s' ('inst' lists them)\n", argv[2]);
    return -1;
  }
  pMidi->m_Cvt.inst[role] = (uint8_t)idx;
  CvtChanged(pMidi);
  return OK;
}

int CPwlSynth::Cset(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  MidiCvt_t *cvt;
  int v;

  if (pMidi == NULL)
    return -1;
  cvt = &pMidi->m_Cvt;

  if (argc < 3)
  {
    CmdPrintf("usage: cset <key> <val>; keys: xpose legato roll melfloor"
              " bassceil padlo padhi arp (pad arpeggiator ms, 0=off)\n");
    return -1;
  }
  v = atoi(argv[2]);

  if (strncmp(argv[1], "xp", 2) == 0)
    cvt->transpose = (int8_t)v;
  else if (strncmp(argv[1], "le", 2) == 0)
    cvt->legato_ms = (uint16_t)v;
  else if (strncmp(argv[1], "ro", 2) == 0)
    cvt->roll_ms = (uint16_t)v;
  else if (strncmp(argv[1], "ar", 2) == 0)
    cvt->arp_ms = (uint8_t)(v < 0 ? 0 : v > 200 ? 200 : v);
  else if (strncmp(argv[1], "me", 2) == 0)
    cvt->mel_floor = (uint8_t)v;
  else if (strncmp(argv[1], "ba", 2) == 0)
    cvt->bass_ceil = (uint8_t)v;
  else if (strncmp(argv[1], "padl", 4) == 0)
    cvt->pad_lo = (uint8_t)v;
  else if (strncmp(argv[1], "padh", 4) == 0)
    cvt->pad_hi = (uint8_t)v;
  else
  {
    CmdPrintf("keys: xpose legato roll melfloor bassceil padlo padhi\n");
    return -1;
  }
  CvtChanged(pMidi);
  return OK;
}

int CPwlSynth::Convert(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  char err[80];
  int n;

  (void)argc;
  (void)argv;
  if (pMidi == NULL)
    return -1;

  n = mid2pwl_convert(pMidi, err, (int)sizeof(err));
  if (n < 0)
  {
    CmdPrintf("convert: %s\n", err);
    return -1;
  }

  // duration = sum of dts (the table is delta-coded)
  {
    uint32_t ms = 0;

    for (uint32_t i = 0; i < pMidi->m_SeqCount; i++)
      ms += pMidi->m_Seq[i].dt_ms;
    CmdPrintf("%d events, %lu bytes, %lu:%02lu of music ('play' hears it)\n",
              n, (unsigned long)(n * sizeof(seq_ev_t)),
              (unsigned long)(ms / 60000u),
              (unsigned long)(ms / 1000u % 60u));
  }
  m_pParent->DrawSourceWindow();
  return OK;
}

int CPwlSynth::Play(int argc, char *argv[])
{
  CMidiFile *pMidi;
  bool quiet = argc > 1 && seq_quiet_arg(argv[argc - 1]);

  // 'play ch<N> [inst] [q]': solo one MIDI channel of the open file,
  // rebased to its first note - for deciding what the channel IS
  if (argc > 1 && argv[1][0] == 'c' && argv[1][1] == 'h' &&
      argv[1][2] >= '0' && argv[1][2] <= '9')
  {
    seq_ev_t *seq;
    char err[64], title[16];
    int i, n, chan = atoi(&argv[1][2]);
    int inst = mid2pwl_inst_find("sine");

    pMidi = ActiveMidi(true);
    if (pMidi == NULL)
      return -1;

    // an 'inst ch<N>' assignment is what the channel will sound like
    // in the conversion - audition with it unless overridden
    if (chan <= 15 && pMidi->m_Cvt.chan_inst[chan] >= 0)
      inst = pMidi->m_Cvt.chan_inst[chan];

    for (i = 2; i < argc; i++)
    {
      if (seq_quiet_arg(argv[i]))
        continue;
      inst = mid2pwl_inst_find(argv[i]);
      if (inst < 0)
      {
        CmdPrintf("unknown instrument '%s' ('inst' lists them)\n", argv[i]);
        return -1;
      }
    }

    n = mid2pwl_audition(pMidi, chan, inst, &seq, err, (int)sizeof(err));
    if (n < 0)
    {
      CmdPrintf("%s\n", err);
      return -1;
    }
    CmdPrintf("ch%d solo as %s (any key stops)\n", chan,
              mid2pwl_inst(inst)->name);
    snprintf(title, sizeof(title), "ch%d solo", chan);

    if (!quiet)
      EnsureNotesTab();
    seq_quiet = quiet ? 1 : 0;
    seq_play(seq, title);
    seq_quiet = 0;
    free(seq);
    FlushStdoutLine();
    if (!quiet)
      ActivateMidiTab(pMidi);           // back to the .mid tab
    return OK;
  }

  if (argc > 1 && !(argc == 2 && quiet))
    return Legacy(argc, argv);          // 'play <file.pwl> [q]' off the host

  pMidi = ActiveMidi(true);
  if (pMidi == NULL)
    return -1;
  if (pMidi->m_Seq == NULL)
  {
    char err[80];
    int n = mid2pwl_convert(pMidi, err, (int)sizeof(err));

    if (n < 0)
    {
      CmdPrintf("convert: %s\n", err);
      return -1;
    }
    CmdPrintf("(converted: %d events)\n", n);
  }

  {
    // The note stream lands on the Notes tab; put the owning MIDI tab
    // (and its footer) back in front when the music stops.  Quiet
    // playback touches no tab at all and mutes the stream - the drawing
    // is UART time taken away from pwl_env_service on fast songs.
    if (!quiet)
      EnsureNotesTab();
    seq_quiet = quiet ? 1 : 0;
    seq_play(pMidi->m_Seq, pMidi->m_Title);
    seq_quiet = 0;
    FlushStdoutLine();
    if (!quiet)
      ActivateMidiTab(pMidi);
  }
  return OK;
}

int CPwlSynth::Trim(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  int beats, total;

  if (pMidi == NULL)
    return -1;
  total = (int)(pMidi->m_MaxTicks / (uint32_t)pMidi->m_Division);

  if (argc < 2)
  {
    CmdPrintf("trim: %u beats (song is %d beats, %d/bar)\n"
              "usage: trim <n> [bars] - cut the first n beats (or bars);"
              " trim 0 restores\n",
              pMidi->m_Cvt.trim_beats, total, pMidi->m_TimeSigNum);
    return OK;
  }

  beats = atoi(argv[1]);
  if (argc > 2 && argv[2][0] == 'b' && argv[2][1] == 'a')
    beats *= pMidi->m_TimeSigNum;       // bars -> beats via the time sig
  if (beats < 0)
    beats = 0;

  if (beats >= total)
  {
    CmdPrintf("trim %d is the whole song (%d beats)\n", beats, total);
    return -1;
  }

  pMidi->m_Cvt.trim_beats = (uint16_t)beats;
  if (beats > 0)
    CmdPrintf("trimmed %d beats (%lu.%lus); playback starts at beat %d\n",
              beats, (unsigned long)(pMidi->BeatToMs(beats) / 1000u),
              (unsigned long)(pMidi->BeatToMs(beats) / 100u % 10u),
              beats + 1);
  else
    CmdPrintf("trim cleared\n");
  CvtChanged(pMidi);
  return OK;
}

int CPwlSynth::Save(int argc, char *argv[])
{
  CMidiFile *pMidi = ActiveMidi(true);
  char path[112];
  long bytes;
  int n;

  if (pMidi == NULL)
    return -1;
  if (pMidi->m_Seq == NULL)
  {
    CmdPrintf("nothing converted yet ('convert' first)\n");
    return -1;
  }

  if (argc > 1)
    snprintf(path, sizeof(path), "%s", argv[1]);
  else
    pMidi->CfgPath(path, sizeof(path), ".pwl");

  n = (int)strlen(path);
  // byte compare: newlib strcmp misbehaves on odd-aligned strings here
  if (n > 2 && path[n - 2] == '.' && path[n - 1] == 'c')
  {
    CmdPrintf("writing C source (a big file takes a while)...\n");
    m_pParent->RedrawCommandWindow();
    bytes = mid2pwl_save_c(pMidi, path);
  }
  else
    bytes = mid2pwl_save_pwl(pMidi, path);

  if (bytes < 0)
  {
    CmdPrintf("save: cannot write %s\n", path);
    return -1;
  }
  pMidi->m_Saved = 1;
  if (n > 2 && path[n - 2] == '.' && path[n - 1] == 'c')
    CmdPrintf("wrote %s: %ld bytes (compile it into the firmware)\n",
              path, bytes);
  else
    CmdPrintf("wrote %s: %ld bytes ('play %s' replays it)\n", path, bytes,
              path);
  m_pParent->DrawSourceWindow();
  return OK;
}

// One track's three braille lines plus its text line.  The grid holds
// MIDI_GRID_COLS time buckets; several collapse into each dot column
// when the window is narrower, which is what keeps the picture readable
// at any width.
void CPwlSynth::DrawMidiTab(CMidiFile *pMidi, WINDOW *pWnd, int topLine,
                            int lineCount)
{
  char line[160];
  int  rows, cols, width, t, y, winRows;

  getmaxyx(pWnd, winRows, cols);
  rows = winRows;
  if (lineCount < rows)
    rows = lineCount;
  if (topLine < 0)
    topLine = 0;
  width = midi_view_cells(cols);

  // The bottom rows belong to the conversion footer; tracks clip above
  if (winRows > MIDI_FOOT_LINES + 4)
    rows = (rows < winRows - MIDI_FOOT_LINES) ? rows
                                              : winRows - MIDI_FOOT_LINES;

  werase(pWnd);

  // Title line: the file, what the header says, and where in the piece
  // the view currently sits (the time axis is fixed, so this is a
  // window onto the score rather than the whole thing)
  y = 0 - topLine;
  if (y >= 0 && y < rows)
  {
    int beat0 = (pMidi->m_ScrollCell * 2) / MIDI_DOTS_PER_BEAT + 1;
    int beats = (width * 2) / MIDI_DOTS_PER_BEAT;
    int total = (int)(pMidi->m_GridCols / MIDI_DOTS_PER_BEAT);

    snprintf(line, sizeof(line),
             "%s  %d %s %d/beat  beats %d-%d of %d",
             pMidi->m_Title, pMidi->m_Tracks,
             pMidi->m_RowsAreChans ? "ch" : "trk", pMidi->m_Division,
             beat0, beat0 + beats - 1, total);
    wattron(pWnd, A_BOLD);
    mvwprintw(pWnd, y, 0, "%.*s", cols - 1, line);
    wattroff(pWnd, A_BOLD);
  }

  for (t = 0; t < pMidi->m_Tracks; t++)
  {
    const MidiTrack_t *pTrk = &pMidi->m_Track[t];
    int base = MIDI_HEAD_LINES + t * MIDI_TRK_LINES - topLine;
    int pair = MIDI_PAIR(t);
    int l, x;

    // Text line
    y = base;
    if (y >= 0 && y < rows)
    {
      char chan[6];

      // 0-based, the same numbering map/automap/'play chN' use
      if (pTrk->channel == 0xFF)
        strcpy(chan, "-");
      else
        snprintf(chan, sizeof(chan), "%d%s", pTrk->channel,
                 pTrk->drums ? "*" : "");

      // Two widths: the roomy one when the tab has space, otherwise a
      // compact one that still shows every field rather than letting
      // the notes count fall off the edge
      if (cols - MIDI_GUTTER >= 62)
        snprintf(line, sizeof(line),
                 "%-14.14s %-18.18s ch%-4s %5d beats %6lu notes",
                 pTrk->name[0] ? pTrk->name : "(unnamed)",
                 pMidi->Instrument(t), chan,
                 pMidi->Beats(t), (unsigned long)pTrk->notes);
      else
        snprintf(line, sizeof(line), "%-11.11s %-15.15s c%-3s %4db %5lun",
                 pTrk->name[0] ? pTrk->name : "(unnamed)",
                 pMidi->Instrument(t), chan,
                 pMidi->Beats(t), (unsigned long)pTrk->notes);
      wattron(pWnd, COLOR_PAIR(pair) | A_BOLD);
      mvwprintw(pWnd, y, 0, "%2d ", t + 1);
      wattroff(pWnd, A_BOLD);
      mvwprintw(pWnd, y, MIDI_GUTTER, "%.*s", cols - MIDI_GUTTER - 1, line);
      wattroff(pWnd, COLOR_PAIR(pair));
    }

    // Three braille lines
    for (l = 0; l < 3; l++)
    {
      y = base + 1 + l;
      if (y < 0 || y >= rows)
        continue;

      if (pTrk->grid == NULL)
        continue;

      wattron(pWnd, COLOR_PAIR(pair));
      for (x = 0; x < width; x++)
      {
        unsigned bits = 0;
        int dx;

        for (dx = 0; dx < 2; dx++)
        {
          // One dot column IS one sixteenth of the score - no
          // aggregation, so a column holds exactly the notes struck
          // together at that moment
          uint32_t dot = (uint32_t)((pMidi->m_ScrollCell + x) * 2 + dx);
          uint16_t acc;
          int r;

          if (dot >= pMidi->m_GridCols)
            break;
          acc = pTrk->grid[dot];
          for (r = 0; r < 4; r++)
            if (acc & (1u << (l * 4 + r)))
              bits |= s_BrailleBit[dx][r];
        }
        if (bits != 0)
          mvwaddch(pWnd, y, MIDI_GUTTER + x, (chtype)(0x2800 + bits));
      }
      wattroff(pWnd, COLOR_PAIR(pair));
    }
  }

  // ---- conversion footer: pinned to the window bottom, never scrolls
  if (winRows > MIDI_FOOT_LINES + 4)
  {
    const MidiCvt_t *cvt = &pMidi->m_Cvt;
    int fy = winRows - MIDI_FOOT_LINES;
    int n, i;

    wattron(pWnd, COLOR_PAIR(INSTR_PAIR));
    mvwhline(pWnd, fy, 0, '-', cols > 1 ? cols - 1 : 1);
    mvwprintw(pWnd, fy, 2, " mid2pwl ");

    if (cvt->satb >= 0)
      snprintf(line, sizeof(line), "map: satb=%d (4-voice split)"
               "  drums=%s", cvt->satb,
               cvt->drums >= 0 ? "on" : "off");
    else
    {
      char mel[24];

      n = 0;
      mel[0] = 0;
      for (i = 0; i < MIDI_MEL_SRCS && cvt->melody[i] >= 0; i++)
        n += snprintf(&mel[n], sizeof(mel) - n, "%s%d", i ? "," : "",
                      cvt->melody[i]);
      n = snprintf(line, sizeof(line), "map: melody=%s", mel[0] ? mel : "-");
      if (cvt->bass >= 0)
        n += snprintf(&line[n], sizeof(line) - n, " bass=%d", cvt->bass);
      else
        n += snprintf(&line[n], sizeof(line) - n, " bass=-");
      if (cvt->pad >= 0)
        n += snprintf(&line[n], sizeof(line) - n, " pad=%d", cvt->pad);
      else
        n += snprintf(&line[n], sizeof(line) - n, " pad=-");
      if (cvt->drums >= 0)
        n += snprintf(&line[n], sizeof(line) - n, " drums=%d", cvt->drums);
      else
        n += snprintf(&line[n], sizeof(line) - n, " drums=-");
    }
    n = (int)strlen(line);
    for (i = 0; i < 16; i++)
      if (cvt->chan_gain[i] != 100)
        n += snprintf(&line[n], sizeof(line) - n, " ch%d@%u%%", i,
                      cvt->chan_gain[i]);
    for (i = 0; i < 16; i++)
      if (cvt->chan_inst[i] >= 0)
        n += snprintf(&line[n], sizeof(line) - n, " ch%d:%s", i,
                      mid2pwl_inst(cvt->chan_inst[i])->name);
    wmove(pWnd, fy + 1, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 1, 1, "%.*s", cols - 2, line);

    snprintf(line, sizeof(line),
             "inst: mel=%s bass=%s pad=%s  xpose=%+d legato=%u trim=%ub",
             mid2pwl_inst(cvt->inst[M2P_ROLE_MEL])->name,
             mid2pwl_inst(cvt->inst[M2P_ROLE_BASS])->name,
             mid2pwl_inst(cvt->inst[M2P_ROLE_PAD])->name,
             cvt->transpose, cvt->legato_ms, cvt->trim_beats);
    wmove(pWnd, fy + 2, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 2, 1, "%.*s", cols - 2, line);

    if (pMidi->m_Seq != NULL)
      snprintf(line, sizeof(line),
               "conv: %lu events (%luB) %s   map/inst/cset convert play save",
               (unsigned long)pMidi->m_SeqCount,
               (unsigned long)(pMidi->m_SeqCount * sizeof(seq_ev_t)),
               pMidi->m_Saved ? "[saved]" : "[in RAM]");
    else
      snprintf(line, sizeof(line),
               "conv: none yet   map/inst/cset convert play save");
    wmove(pWnd, fy + 3, 0);
    wclrtoeol(pWnd);
    mvwprintw(pWnd, fy + 3, 1, "%.*s", cols - 2, line);
    wattroff(pWnd, COLOR_PAIR(INSTR_PAIR));
  }
}

int CPwlSynth::Close(int argc, char *argv[])
{
  CTab *pTab = m_pParent->GetActiveSrcTab();

  (void)argc;
  (void)argv;

  if (pTab == NULL)
  {
    CmdPrintf("close: no tab open\n");
    return -1;
  }
  CloseTab(pTab);
  return OK;
}

/*
==============================================================================
Preferences.  CTui owns the file (window layout, then command history);
these two hooks add what only this source knows.  Saved on exit and on
every layout change, restored during UIInit.
==============================================================================
*/

void CPwlSynth::SaveWatchItems(FILE *fd)
{
  fprintf(fd, "WATCH=%d\n", m_WatchCh);
}

void CPwlSynth::RestoreWatchItem(char *pStr)
{
  int ch = atoi(pStr);

  if (ch >= 0 && ch < PWL_NUM_CHANNELS)
    m_WatchCh = ch;
}

int CPwlSynth::ProcessLine(char *line)
{
  // Everything known lives in the command table; unmatched lines are
  // genuinely unknown.
  (void)line;
  return -1;
}

// ALT-L / ALT-R move the score half a window at a time - far enough to
// cross a long piece in a few presses, with enough overlap to keep your
// place - and dir 0 (HOME) rewinds to the downbeat.  Only the MIDI tab
// is wider than its window; the track titles are drawn from the window
// edge and never move with it.
bool CPwlSynth::ScrollSource(int dir, bool page)
{
  CTab *pTab = (m_pParent != NULL) ? m_pParent->GetActiveSrcTab() : NULL;
  void *pCtx = (pTab != NULL) ? pTab->SourceContext() : NULL;
  CMidiFile *pMidi;
  WINDOW *pWnd;
  int rows, cols, visible;

  if (!IsMidiCtx(pCtx))
    return false;
  pMidi = (CMidiFile *)pCtx;

  pWnd = pTab->GetWindow();
  if (pWnd == NULL)
    return false;
  getmaxyx(pWnd, rows, cols);
  (void)rows;
  visible = midi_view_cells(cols);

  if (dir == 0)                         // HOME: back to the downbeat
    return pMidi->Scroll(-pMidi->m_ScrollCell, visible);

  return pMidi->Scroll(dir * (page ? visible / 2
                                   : MIDI_DOTS_PER_BEAT * 4 / 2),
                       visible);
}

int CPwlSynth::HandleCtrlC(void)
{
  // Long running commands (demos / songs) stop on any keypress in their
  // own wait loops; nothing extra to do here.
  return 0;
}

/*
==============================================================================
Tab completion
==============================================================================
*/

void CPwlSynth::AddTuiSortItem(TuiSortList_t *pList, const char *pStr)
{
  TuiSortItem_t *pItem;
  TuiSortItem_t *pCurr;
  TuiSortItem_t *pPrev;

  // Don't add duplicates
  for (pItem = pList->pFirst; pItem != NULL; pItem = pItem->pNext)
    if (strcmp(pItem->name, pStr) == 0)
      return;

  pItem = (TuiSortItem_t *)malloc(sizeof(TuiSortItem_t) + strlen(pStr) + 1);
  if (pItem == NULL)
    return;
  pItem->name = ((char *)pItem) + sizeof(TuiSortItem_t);
  strcpy((char *)pItem->name, pStr);

  // Sorted insert
  pPrev = NULL;
  pCurr = pList->pFirst;
  while (pCurr != NULL && strcmp(pCurr->name, pStr) < 0)
  {
    pPrev = pCurr;
    pCurr = pCurr->pNext;
  }
  pItem->pNext = pCurr;
  if (pPrev == NULL)
    pList->pFirst = pItem;
  else
    pPrev->pNext = pItem;
}

TuiSortList_t *CPwlSynth::BuildListFromNames(const char *const *names)
{
  TuiSortList_t *pList;
  int            x;

  pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
  if (pList == NULL)
    return NULL;
  pList->pFirst = NULL;

  for (x = 0; names[x] != NULL; x++)
    AddTuiSortItem(pList, names[x]);

  return pList;
}

/*
==============================================================================
Filename completion off the host filesystem, bash style: TAB completes
as far as the names agree, a second TAB lists the matches.  CTui frees
and refetches the list on every TAB in argument mode, so each press
re-runs the ls for whatever directory the partial path names now.

CTui's matcher compares candidates against the text after the LAST '/',
'\\' or '.' in the typed argument, so candidates are handed over sliced
at exactly that point (a full path would never match once a directory
or an extension dot is present).  Directories gain a trailing '/':
completing one keeps the "no space" behavior, so the next TAB descends
into it - 'open so<TAB>' -> 'open songs/' -> <TAB> lists the songs.
==============================================================================
*/

TuiSortList_t *CPwlSynth::BuildFsTabList(const char *pBuffer)
{
  static char listing[1024];
  char dir[96], full[120];
  const char *partial, *sp;
  TuiSortList_t *pList;
  int i, n, plen, split, dirlen;

  // The argument being completed: text after the last space
  sp = strrchr(pBuffer, ' ');
  partial = (sp != NULL) ? sp + 1 : "";
  plen = (int)strlen(partial);

  // Directory to list: the partial up to its last '/'.  dirlen marks
  // where entry names splice onto the typed text - NOT the same place
  // the matcher slices, which also breaks at '.'.
  {
    const char *slash = strrchr(partial, '/');

    if (slash != NULL)
    {
      dirlen = (int)(slash - partial) + 1;
      n = dirlen - 1;
      if (n >= (int)sizeof(dir))
        n = (int)sizeof(dir) - 1;
      memcpy(dir, partial, n);
      dir[n] = 0;
    }
    else
    {
      dirlen = 0;
      strcpy(dir, ".");
    }
  }

  // Where CTui's matcher slices the typed text
  split = 0;
  for (i = plen; i > 0; i--)
  {
    if (partial[i - 1] == '/' || partial[i - 1] == '\\' ||
        partial[i - 1] == '.')
    {
      split = i;
      break;
    }
  }

  n = tqv_fs_list(dir, listing, (int)sizeof(listing) - 1);
  if (n <= 0)
    return NULL;                        // no host filesystem: no completion
  listing[n] = 0;

  pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
  if (pList == NULL)
    return NULL;
  pList->pFirst = NULL;

  // Rows are "<f|d> <size> <name>\n"
  {
    char *line = listing;

    while (line != NULL && *line != 0)
    {
      char *nl = strchr(line, '\n');
      char type = line[0];
      char *name;

      if (nl != NULL)
        *nl = 0;
      name = strchr(line, ' ');
      name = (name != NULL) ? strchr(name + 1, ' ') : NULL;
      if (name != NULL && (type == 'f' || type == 'd'))
      {
        name++;
        snprintf(full, sizeof(full), "%.*s%s%s", dirlen, partial, name,
                 type == 'd' ? "/" : "");
        // Keep entries the typed text is a prefix of (case-blind, the
        // same test the matcher applies to its slice)
        if (strncasecmp(full, partial, plen) == 0)
          AddTuiSortItem(pList, full + split);
      }
      line = (nl != NULL) ? nl + 1 : NULL;
    }
  }
  return pList;
}

int CPwlSynth::GetCommandTabList(char *pCmd, const char *pBuffer,
                                 TuiSortList_t *&pList)
{
  (void)pBuffer;

  // Completing the command word itself: hand out the persistent list
  if (pCmd == NULL || pCmd[0] == 0)
  {
    if (m_pCmdTabList == NULL)
    {
      const PwlCmd_t *p;

      m_pCmdTabList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
      if (m_pCmdTabList == NULL)
        return 0;
      m_pCmdTabList->pFirst = NULL;
      for (p = s_TuiCmds; p->name != NULL; p++)
        AddTuiSortItem(m_pCmdTabList, p->name);
      AddTuiSortItem(m_pCmdTabList, "exit");
      AddTuiSortItem(m_pCmdTabList, "quit");
    }
    pList = m_pCmdTabList;
    return 1;
  }

  // Filename arguments complete off the host filesystem
  if (strcmp(pCmd, "open") == 0 || strcmp(pCmd, "cat") == 0 ||
      strcmp(pCmd, "ls") == 0 ||
      strcmp(pCmd, "play") == 0 || strcmp(pCmd, "save") == 0)
  {
    pList = BuildFsTabList(pBuffer);
    return pList != NULL;
  }

  // Conversion commands: the first argument is a role / knob keyword,
  // and inst's second is an instrument name.  Which argument is being
  // completed shows in the buffer: a space after a non-empty first
  // argument means we are on to the second.
  if (strcmp(pCmd, "inst") == 0 || strcmp(pCmd, "map") == 0 ||
      strcmp(pCmd, "cset") == 0)
  {
    const char *sp = strchr(pBuffer, ' ');
    bool second = false;

    if (sp != NULL)
    {
      while (*sp == ' ')
        sp++;
      second = (*sp != 0 && strchr(sp, ' ') != NULL);
    }

    if (!second)
    {
      if (strcmp(pCmd, "inst") == 0)
      {
        // no drums here: percussion is fixed one-shots, not an instrument
        static const char *const roles[] = { "bass", "melody", "pad", NULL };

        pList = BuildListFromNames(roles);
      }
      else if (strcmp(pCmd, "map") == 0)
      {
        static const char *const roles[] =
          { "bass", "drums", "melody", "pad", "satb", NULL };

        pList = BuildListFromNames(roles);
      }
      else
      {
        static const char *const keys[] =
          { "bassceil", "legato", "melfloor", "padhi", "padlo", "roll",
            "xpose", NULL };

        pList = BuildListFromNames(keys);
      }
      return pList != NULL;
    }

    if (strcmp(pCmd, "inst") == 0)
    {
      int i;

      pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
      if (pList == NULL)
        return 0;
      pList->pFirst = NULL;
      for (i = 0; i < mid2pwl_inst_count(); i++)
        AddTuiSortItem(pList, mid2pwl_inst(i)->name);
      return 1;
    }
    pList = NULL;
    return 0;                           // map/cset second arg is a number
  }

  // Argument completion for commands with fixed keyword arguments
  if (strcmp(pCmd, "wave") == 0)
  {
    const char *pName;
    int         x;

    pList = (TuiSortList_t *)malloc(sizeof(TuiSortList_t));
    if (pList == NULL)
      return 0;
    pList->pFirst = NULL;
    AddTuiSortItem(pList, "list");
    for (x = 0; (pName = pwl_preset_name(x)) != NULL; x++)
      AddTuiSortItem(pList, pName);
    return 1;
  }
  if (strcmp(pCmd, "stereo") == 0)
  {
    static const char *const names[] = { "off", "on", "pos", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "bend") == 0)
  {
    static const char *const names[] = { "down", "up", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "adsr") == 0 || strcmp(pCmd, "detune") == 0 ||
      strcmp(pCmd, "penv") == 0 || strcmp(pCmd, "vib") == 0)
  {
    static const char *const names[] = { "off", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "tenv") == 0)
  {
    static const char *const names[] = { "off", "pwm", "slope", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "help") == 0)
  {
    static const char *const names[] = { "cli", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }
  if (strcmp(pCmd, "fs") == 0)
  {
    static const char *const names[] = { "probe", NULL };
    pList = BuildListFromNames(names);
    return pList != NULL;
  }

  pList = NULL;
  return 0;
}

void CPwlSynth::FreeTabList(TuiSortList_t *pList)
{
  TuiSortItem_t *pItem;
  TuiSortItem_t *pNext;

  // The persistent command list survives for the session
  if (pList == NULL || pList == m_pCmdTabList)
    return;

  pItem = pList->pFirst;
  while (pItem != NULL)
  {
    pNext = pItem->pNext;
    free(pItem);
    pItem = pNext;
  }
  free(pList);
}

/*
==============================================================================
Source window: the Notes tab (the only source tab this app has - the
synth has no program listing to show)
==============================================================================
*/

int CPwlSynth::GetSourceLineCount(void *pCtx)
{
  if (pCtx == &s_NotesCtxMarker)
    return PWL_NUM_CHANNELS * s_NoteRows;       // exactly fills the tab
  if (pCtx == &s_InstrCtxMarker)
    return INSTR_LINES + 5;     // title + rows + help + sounds + song
  if (IsMidiCtx(pCtx))
  {
    // Title + blank + four lines per track.  The real count is what
    // lets PGUP/PGDN scroll a long file (CTui clamps against this).
    CMidiFile *pMidi = (CMidiFile *)pCtx;

    return MIDI_HEAD_LINES + pMidi->m_Tracks * MIDI_TRK_LINES +
           MIDI_FOOT_LINES;
  }
  return 0;
}

void CPwlSynth::DrawSourceWindow(void *pCtx, WINDOW *pWnd, int topLine,
                                 int lineCount)
{
  int rows, cols, row, ch, which;

  if (pCtx == &s_InstrCtxMarker)
  {
    DrawInstrPanel(pWnd, topLine);
    return;
  }
  if (IsMidiCtx(pCtx) && pWnd != NULL)
  {
    InitNoteColors();                   // also claims the MIDI track pairs
    DrawMidiTab((CMidiFile *)pCtx, pWnd, topLine, lineCount);
    return;
  }
  if (pCtx != &s_NotesCtxMarker || pWnd == NULL)
    return;

  // The tracks live at fixed rows (channel * s_NoteRows + line), so this
  // full repaint only runs on tab switches / resizes; note arrivals go
  // through NoteLineUpdate.
  InitNoteColors();
  getmaxyx(pWnd, rows, cols);
  if (lineCount < rows)
    rows = lineCount;
  if (topLine < 0)
    topLine = 0;

  // Re-split the window: this is the authoritative measurement, and it
  // runs on every resize (CTui redraws the active tab from RedrawScreen)
  notes_set_rows(notes_rows_for(rows));

  werase(pWnd);
  if (cols - 1 - NOTE_GUTTER < 1)
    return;                             // window too narrow for a track
  for (ch = 0; ch < PWL_NUM_CHANNELS; ch++)
  {
    for (which = 0; which < s_NoteRows; which++)
    {
      row = ch * s_NoteRows + which - topLine;
      if (row >= rows)
        break;
      if (row < 0)
        continue;

      notes_draw_gutter(pWnd, row, ch, which);
      wattron(pWnd, COLOR_PAIR(NOTE_PAIR(ch)));
      mvwprintw(pWnd, row, NOTE_GUTTER, "%.*s", cols - 1 - NOTE_GUTTER,
                s_NoteText[ch][which]);
      wattroff(pWnd, COLOR_PAIR(NOTE_PAIR(ch)));
    }
  }
}

void CPwlSynth::DrawSplash(WINDOW *pWnd)
{
  int rows, cols, y;

  if (pWnd == NULL)
    return;

  // CTabContainer::Redraw() also calls this whenever its first-visible-tab
  // pointer is NULL, which happens on every MakeTabActive - painting the
  // splash straight over the live tab (first the note tracks, then the
  // instrument panel).  Only splash while there really are no tabs.
  if (m_pParent != NULL && m_pParent->GetFirstTab() != NULL)
    return;

  getmaxyx(pWnd, rows, cols);
  y = rows / 2 - 3;
  if (y < 0)
    y = 0;

  werase(pWnd);
  wattron(pWnd, A_BOLD);
  mvwprintw(pWnd, y,     (cols - 25) / 2, "P W L   S Y N T H   T U I");
  wattroff(pWnd, A_BOLD);
  mvwprintw(pWnd, y + 2, (cols - 35) / 2, "TinyQV / TT Sky 25a  peripheral 33");
  mvwprintw(pWnd, y + 3, (cols - 35) / 2, "4 channels, piecewise linear osc");
  mvwprintw(pWnd, y + 5, (cols - 30) / 2, "type 'help' for commands");
  wrefresh(pWnd);
}

void CPwlSynth::CloseTab(CTab *pTab)
{
  if (pTab != NULL)
  {
    // The notes and studio contexts are static markers; a MIDI tab owns
    // its parsed file (and, later, its conversion settings)
    void *pCtx = pTab->SourceContext();

    m_pParent->DeleteTab(pTab);
    if (IsMidiCtx(pCtx))
      delete (CMidiFile *)pCtx;
  }
}

/*
==============================================================================
Watch window: every register of the watched channel, refreshed ~5x/sec by
the CTui main loop (and after each command).

period / phase / amp / slopes / pwm_offset read back from the peripheral;
mode, the two sweep registers and cfg are write-only in hardware, so they
come from the driver's shadow state in pwl_ch[] / pwl_get_cfg().
==============================================================================
*/

static const char *const s_WfNames[4]   = { "osc", "noise", "pwl", "orion" };
static const char *const s_SyncNames[4] = { "off", "hard", "4bit", "soft" };
static const char *const s_EnvNames[5]  =
  { "idle", "att", "dec", "held", "rel" };
// slope_dir field: 3 = both slopes, 1/2 = one, 0 = opposite directions
static const char *const s_SlopeDir[4]  = { "opp", "r", "f", "r+f" };
static const char *const s_TenvDir[4]   = { "o", "r", "f", "" };

void CPwlSynth::DrawWatchWindow(WINDOW *pWnd, int topLine)
{
  const pwl_channel_state_t *pCh = &pwl_ch[m_WatchCh];
  uint16_t period, phase, amp, slope_r, slope_f, pwm_offset;
  uint16_t mode, pa, ws, cfg;
  char     autodet[8];
  int      line = -topLine;

  if (pWnd == NULL)
    return;

  period     = pwl_read(m_WatchCh, PWL_REG_PERIOD);
  phase      = pwl_read(m_WatchCh, PWL_REG_PHASE);
  amp        = pwl_read(m_WatchCh, PWL_REG_AMP);
  slope_r    = pwl_read(m_WatchCh, PWL_REG_SLOPE_R);
  slope_f    = pwl_read(m_WatchCh, PWL_REG_SLOPE_F);
  pwm_offset = pwl_read(m_WatchCh, PWL_REG_PWM_OFFSET);

  mode = pCh->mode;
  pa   = pCh->sweep_pa;
  ws   = pCh->sweep_ws;
  cfg  = pwl_get_cfg();

  if (pCh->relative_detune == PWL_DETUNE_OFF)
    strcpy(autodet, "off");
  else
    snprintf(autodet, sizeof(autodet), "%d", pCh->relative_detune);

#define WATCH_LINE(fmt, ...)                                   \
  do {                                                         \
    if (line >= 0) {                                           \
      wmove(pWnd, line, 0);                                    \
      wclrtoeol(pWnd);                                         \
      mvwprintw(pWnd, line, 0, fmt, ##__VA_ARGS__);            \
    }                                                          \
    line++;                                                    \
  } while (0)

  // Every armed feature costs a line and the window has ~13 rows on an
  // 80x24 fallback terminal, so lines are packed: the violin case (adsr
  // + penv + tenv + vib) fits exactly.
  WATCH_LINE("ch%d %-3s %-4s det %s", m_WatchCh, pCh->on ? "ON" : "off",
             s_LastNote[m_WatchCh][0] ? s_LastNote[m_WatchCh] : "-",
             autodet);
  WATCH_LINE("period %04x e%d m%d", period, (period >> 10) & 7,
             period & 0x3FF);
  WATCH_LINE("phase 0x%03x amp %2d", phase & 0xFFF, amp & 63);
  WATCH_LINE("slope r%d f%d pwm %d", slope_r & 0xFF, slope_f & 0xFF,
             pwm_offset & 0xFF);
  WATCH_LINE("mode  0x%03x %s", mode & 0xFFF,
             s_WfNames[((mode >> 3) & 1) | ((mode >> 7) & 2)]);
  // detune_exp / detune_5th are the live mode fields; 'autodet' is the
  // driver's per-note auto-detune strength that computes them
  WATCH_LINE(" det %d %s  mult %d", mode & 7,
             (mode & PWL_MODE_DETUNE_5TH) ? "5th" : "   ",
             (mode >> 4) & 7);
  WATCH_LINE(" sync %-4s sat %s", s_SyncNames[(mode >> 9) & 3],
             (mode & PWL_MODE_COMMON_SAT) ? "on" : "off");

  // Sweeps: raw value plus a one-line decode when armed.  The whole
  // display has to fit the ~13 rows the watch window gets on an 80x24
  // terminal, so nothing readable gets its own line unless it earns it.
  //   sw_pa <raw> <period dir><rate> t<amp target> r<amp rate>
  //   sw_ws <raw> <pwm dir><rate> <slope dir><rate> <slopes swept>
  if (pa == 0 && ws == 0)
    WATCH_LINE("sweeps  off");
  else
  {
    if (pa == 0)
      WATCH_LINE("sw_pa   off");
    else
      WATCH_LINE("sw_pa %04x %s%-2d t%d r%d", pa,
                 (pa & 0x1000) ? "dn" : "up", (pa >> 8) & 15,
                 (pa >> 4) & 7, pa & 15);
    if (ws == 0)
      WATCH_LINE("sw_ws   off");
    else
      WATCH_LINE("sw_ws %04x %s%-2d %s%-2d %s", ws,
                 (ws & 0x1000) ? "dn" : "up", (ws >> 8) & 15,
                 (ws & 0x10) ? "dn" : "up", ws & 15,
                 s_SlopeDir[(ws >> 5) & 3]);
  }

  if (pCh->env_enabled)
    WATCH_LINE("adsr a%dd%ds%dr%d %s %d", pCh->env_a, pCh->env_d,
               pCh->env_s, pCh->env_r,
               s_EnvNames[pCh->env_state < 5 ? pCh->env_state : 0],
               pCh->env_last);
  else
    WATCH_LINE("adsr    off");

  // Pitch / timbre envelopes: one compact line each, only when armed
  if (pCh->penv_offset || pCh->penv_rel_rate)
    WATCH_LINE("penv %+d r%-2d rel r%d%s", pCh->penv_offset,
               pCh->penv_rate, pCh->penv_rel_rate,
               pCh->penv_rel_up ? "^" : "");
  if (pCh->tenv_pwm_delta || pCh->tenv_slope_delta)
    // compact dir suffix: o/r/f, none for the default r+f
    WATCH_LINE("tenv p%+dr%d s%+dr%d%s", (int)pCh->tenv_pwm_delta,
               pCh->tenv_pwm_rate, pCh->tenv_slope_delta,
               pCh->tenv_slope_rate, s_TenvDir[pCh->tenv_slope_dir & 3]);

  if (pCh->vib_rate && pCh->vib_depth)
    WATCH_LINE("vib %d.%dHz %dc %dms", pCh->vib_rate / 10,
               pCh->vib_rate % 10, pCh->vib_depth, pCh->vib_delay_ms);

  WATCH_LINE("%s cnt %lu",
             (cfg & PWL_CFG_STEREO_POS_EN) ? "st+pos"
             : (cfg & PWL_CFG_STEREO_EN)   ? "stereo" : "mono",
             (unsigned long)pwl_read_counter());

#undef WATCH_LINE

  wnoutrefresh(pWnd);
}
