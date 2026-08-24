/************************************************************************************
 * pwl-test/tui/PwlSynth.h
 *
 * CPwlSynth: the CTuiSource behind the PWL synth debug TUI on TinyQV.
 *
 * Ported from prism-test/tui/Prism.cxx (the PRISM debugger TUI) to the
 * PiecewiseOrionSynth (pwl_synth, peripheral 33).  The framework is
 * unchanged; what differs is the peripheral:
 *
 *   WATCH window  every register of ONE synth channel, live - the
 *                 channel is chosen with the 'watch <0-3>' command.
 *                 Write-only registers (mode / sweeps / cfg) come from
 *                 the driver's shadow state in pwl_ch[].
 *   NOTES tab     the note progression PER CHANNEL: two window lines per
 *                 channel, filled left to right and recycled when full,
 *                 so all four voices are visible at once while a song
 *                 plays.
 *   commands      the pwl> CLI (main.c cli_execute), forwarded with
 *                 printf output redirected into the command window.
 *
 * There is no chroma listing / Verilog source tab here (the synth has no
 * program to show) and no flash / PSRAM upload - pwl-test has none.
 *
 *   Author: Ken Pettit <pettitkd@gmail.com>  (original TUI framework)
 ************************************************************************************/

#ifndef _PWL_TEST_TUI_PWLSYNTH_H
#define _PWL_TEST_TUI_PWLSYNTH_H

#include "TuiSource.h"

class CPwlSynth;
typedef int (CPwlSynth::*CPwlFunc_t)(int argc, char *argv[]);

class CPwlSynth : public CTuiSource
{
  public:
    CPwlSynth();
    ~CPwlSynth();

  /* CTuiSource interface */

  public:
    int                 GetSourceLineCount(void *pCtx);
    void                DrawSourceWindow(void *pCtx, WINDOW *pWnd, int topLine,
                                         int lineCount);
    void                DrawWatchWindow(WINDOW *pWnd, int topLine);
    const TuiCmd_t *    GetCommandTable(void);
    int                 GetCommandTabList(char *pCmd, const char *pBuffer,
                                          TuiSortList_t *&pList);
    void                FreeTabList(TuiSortList_t *pList);
    void                DebugPrintf(const char *fmt, ...);
    int                 ProcessLine(char *line);
    void                DrawSplash(WINDOW *pWnd);
    int                 HandleCtrlC(void);
    bool                ScrollSource(int dir, bool page);
    void                CloseTab(CTab *pTab);
    void                SaveWatchItems(FILE *fd);
    void                RestoreWatchItem(char *pStr);

  /* Instrument panel tab: WantKeys routing + idle service hook */

  public:
    bool                WantProcessKey(void);
    int                 ProcessKey(int key);
    int                 WantFocus(void);
    void                SetFocus(void *pCtx, WINDOW *pWnd, int topLine,
                                 int lineCount);
    void                IdlePoll(void);

  /* Command handlers (called through the TuiCmd_t table) */

  public:
    int                 Legacy(int argc, char *argv[]);   // -> cli_execute()
    int                 Amp(int argc, char *argv[]);      // ch gain / legacy
    int                 Help(int argc, char *argv[]);
    int                 Clear(int argc, char *argv[]);
    int                 Close(int argc, char *argv[]);    // close active tab
    int                 Notes(int argc, char *argv[]);    // note tracks tab
    int                 Watch(int argc, char *argv[]);    // select watch channel
    int                 Studio(int argc, char *argv[]);   // instrument studio tab
    int                 Open(int argc, char *argv[]);     // MIDI file tab

  /* MIDI conversion (tui/Mid2Pwl.cxx does the work) */

  public:
    int                 Automap(int argc, char *argv[]);  // guess the mapping
    int                 Map(int argc, char *argv[]);      // role <- MIDI channel
    int                 Inst(int argc, char *argv[]);     // role instrument
    int                 Cset(int argc, char *argv[]);     // conversion knobs
    int                 Convert(int argc, char *argv[]);  // build the seq table
    int                 Play(int argc, char *argv[]);     // RAM seq / .pwl file
    int                 Save(int argc, char *argv[]);     // write .pwl / .c
    int                 Trim(int argc, char *argv[]);     // cut the intro

  /* Stdout redirection: legacy printf output -> command window */

  public:
    void                InstallStdoutHook(void);
    void                RemoveStdoutHook(void);
    void                StdoutChunk(const char *buffer, int length);
    void                FlushStdoutLine(void);
    int                 CmdPrintf(const char *fmt, ...);
    void                NoteChunk(int channel, const char *text);
    void                NotesClear(void);      // new song: wipe the tracks

  private:
    void                AddTuiSortItem(TuiSortList_t *pList, const char *pStr);
    TuiSortList_t *     BuildListFromNames(const char *const *names);
    TuiSortList_t *     BuildFsTabList(const char *pBuffer);
    int                 HelpEmit(const char *usage, const char *help,
                                 int &onPage, int rows);
    void                DrawMidiTab(class CMidiFile *pMidi, WINDOW *pWnd,
                                    int topLine, int lineCount);
    bool                IsMidiCtx(void *pCtx);
    class CMidiFile *   ActiveMidi(bool complain);
    void                ActivateMidiTab(class CMidiFile *pMidi);
    void                CvtChanged(class CMidiFile *pMidi);
    CTab *              EnsureNotesTab(void);
    CTab *              FindNotesTab(void);
    void                InitNoteColors(void);
    int                 NotesWidth(WINDOW *pWnd);
    void                NoteLineUpdate(int channel, int fromCol);
    CTab *              EnsureInstrTab(void);
    CTab *              FindInstrTab(void);
    void                DrawInstrPanel(WINDOW *pWnd, int topLine);
    void                InstrRowText(int id, char *label, char *val,
                                     int *barCur, int *barMax);
    void                InstrRowRepaint(int id);
    void                InstrRecsRepaint(void);
    void                InstrSongRepaint(void);
    void                InstrAdjust(int id, int dir, bool coarse);
    void                InstrGate(bool on);
    void                InstrSetNote(int midi);
    void                InstrSelectChannel(int ch);

  private:
    TuiSortList_t     * m_pCmdTabList;      // persistent command-name list
    char                m_OutLine[256];     // stdout hook line accumulator
    int                 m_OutLen;
    bool                m_OutOpen;          // partial line already printed
    int                 m_WatchCh;          // channel shown in the watch window
    bool                m_ColorsReady;      // note pairs claimed this session
};

#endif /* _PWL_TEST_TUI_PWLSYNTH_H */
