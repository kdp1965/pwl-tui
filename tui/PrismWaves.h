/************************************************************************************
 * tui/PrismWaves.h
 *
 * CPrismWaves: a braille oscilloscope for the OTHER synth.  The PRISM
 * peripheral runs the dutymeter chroma, integrating the HIGH time of
 * the PWL synth's PWM output looped back into ui_in[0] by a jumper.
 * Delta-polling count1 against rdtime decodes the audio; triggered
 * frames render as 6 rows of braille at the bottom of the Notes tab.
 *
 * Disabled by default (it needs the jumper); CPwlSynth's "scope on|off"
 * command arms and disarms it.  CPwlSynth passes its tab window into
 * Service()/DrawFrame() - this class knows curses, not the TUI.
 ************************************************************************************/

#ifndef _PWL_TUI_PRISMWAVES_H
#define _PWL_TUI_PRISMWAVES_H

#include <stdint.h>
#include "Tui.h"

#define PRISMWAVES_ROWS   4         /* waveform lines (the middle of
                                       * the old 6: extremes crop) */
#define PRISMWAVES_REGION (PRISMWAVES_ROWS + 1)  /* + the frame row */

class CPrismWaves
{
  public:
    CPrismWaves();

    bool  Enabled(void) const { return m_On; }
    int   Enable(void);         // load the dutymeter chroma, arm, baseline
    void  Disable(void);        // park PRISM, forget the frame
    void  Rebase(void);         // fresh baseline at song start
    bool  BuildFrame(int cols); // false = flat capture, frame held

    // Called at high rate from the player wait loops (via the C hook).
    // pWnd may be NULL (Notes tab not on screen): sampling continues so
    // the deltas stay fresh, drawing is skipped.
    bool  Service(WINDOW *pWnd, int top, int cols);  // true = painted

    // Full repaint of the scope region (tab switch / resize)
    void  DrawFrame(WINDOW *pWnd, int top, int cols);   // one-shot
    bool  DrawChunk(WINDOW *pWnd, int top, int cols);   // one piece/call
    void  DrawBorder(WINDOW *pWnd, int top, int x0, int cells);
    void  DrawRows(WINDOW *pWnd, int top, int x0, int cells,
                   int y0, int y1);

  private:
    void  Rearm(void);          // enable-pulse reload + fresh baseline
    int   Sample(void);         // one duty sample, -1 = not due yet

    bool      m_On;
    uint32_t  m_LastCount;      // count1 at the previous sample
    uint32_t  m_LastTicks;      // rdtime at the previous sample
    uint32_t  m_NextSample;     // rdtime deadline of the next sample
    uint32_t  m_NextFrame;      // rdtime deadline of the next frame
    int       m_Collect;        // samples still wanted (0 = idle gap)
    int       m_Want;           // samples per capture (2x drawn cols)
    uint8_t   m_Raw[480];       // captured duty samples (0..255)
    int       m_RawN;
    uint8_t   m_Dots[240];      // dot heights after trigger (2/cell)
    int       m_DotN;
    bool      m_Fresh;          // a new frame is ready to draw
    int       m_DrawStep;       // chunked paint: 0=border,1=rows0-1,2=rows2-3
    int       m_DrawX0, m_DrawCells;  // geometry cached at step 0
};

#endif /* _PWL_TUI_PRISMWAVES_H */
