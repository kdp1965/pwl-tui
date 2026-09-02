/************************************************************************************
 * tui/PrismWaves.cxx - see PrismWaves.h.
 *
 * Timing model: rdtime ticks are clk/64, count1 decrements at the full
 * clock while the input is HIGH, so duty = dCount / (dTicks * 64).  A
 * sample every ~64us gives ~12-bit deltas; 2 samples per braille
 * column, one triggered frame every ~250ms keeps the UART cost of a
 * repaint out of the envelope service budget.
 ************************************************************************************/

#include <string.h>

#include "PrismWaves.h"

extern "C" {
#include "peripherals/prism.h"
#include "csr.h"
extern uint32_t clock_hz;
extern const uint32_t chroma_dutymeter[];
}

#define SCOPE_PAIR      25          /* after the note/instr/midi pairs */
#define SAMPLE_US       64          /* per-sample cadence */
#define FRAME_GAP_US    250000      /* triggered frame rate */
#define RELOAD_FLOOR    (1u << 20)  /* rearm before count1 saturates */

static inline uint32_t us2ticks(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * clock_hz) / 64000000u);
}

CPrismWaves::CPrismWaves()
{
  m_On = false;
  m_RawN = m_DotN = m_Collect = m_Want = 0;
  m_Fresh = false;
  m_DrawStep = 0;
}

void CPrismWaves::Rebase(void)
{
  if (!m_On)
    return;
  Rearm();
  m_Collect = 0;
  m_Fresh = false;
  m_NextSample = m_LastTicks;
  m_NextFrame = m_LastTicks;
}

void CPrismWaves::Rearm(void)
{
  prism_disable();                  // FSM through IDLE: the entry arm
  prism_enable();                   // reloads count1 from the preload
  m_LastCount = prism_get_count1();
  m_LastTicks = read_time();
}

int CPrismWaves::Enable(void)
{
  int res = prism_load_chroma(chroma_dutymeter, 0x00000000u);

  if (res)
    return res;
  prism_set_count1_preload(0xFFFFFF);
  prism_host_write(0x01);           // arm bit: IDLE -> MEASURE
  prism_enable();
  Rearm();
  m_NextSample = m_LastTicks;
  m_NextFrame = m_LastTicks;
  m_Collect = 0;
  m_Fresh = false;
  m_On = true;
  return 0;
}

void CPrismWaves::Disable(void)
{
  if (!m_On)
    return;
  prism_host_write(0x00);
  prism_disable();
  g_TuiCursorSuppress = 0;          // in case a song was mid-loop
  curs_set(1);
  m_On = false;
  m_Fresh = false;
  m_DotN = 0;
}

// One duty sample when due.  Returns the 0..255 duty, or -1.
int CPrismWaves::Sample(void)
{
  uint32_t now = read_time();

  if ((int32_t)(now - m_NextSample) < 0)
    return -1;
  m_NextSample = now + us2ticks(SAMPLE_US);

  uint32_t c = prism_get_count1();
  uint32_t dHigh = (m_LastCount - c) & 0xFFFFFFu;
  uint32_t dTicks = now - m_LastTicks;

  m_LastCount = c;
  m_LastTicks = now;
  if (c < RELOAD_FLOOR)
    Rearm();
  if (dTicks == 0)
    return -1;

  uint32_t duty = dHigh * 4u / dTicks;      // dHigh*256 / (dTicks*64)
  return duty > 255u ? 255 : (int)duty;
}

bool CPrismWaves::BuildFrame(int cols)
{
  int need = m_RawN / 2;

  (void)cols;                       // the draw clamps to the window
  if (need > (int)sizeof(m_Dots))
    need = (int)sizeof(m_Dots);

  // Rising-edge trigger through the capture's own midline, searched in
  // the first half so a full frame follows it
  int lo = 255, hi = 0, t = 0;

  for (int i = 0; i < m_RawN; i++)
  {
    if (m_Raw[i] < lo) lo = m_Raw[i];
    if (m_Raw[i] > hi) hi = m_Raw[i];
  }
  if (hi - lo < 16)
    return false;               // no signal: hold the last trace

  int mid = (lo + hi) / 2;

  for (int i = 1; i < m_RawN / 2; i++)
    if (m_Raw[i - 1] < mid && m_Raw[i] >= mid)
    {
      t = i;
      break;
    }

  if (need * 2 > (int)sizeof(m_Dots))
    need = (int)sizeof(m_Dots) / 2;

  // Zero-order hold reads as a square wave (flat, cliff, flat).  Two
  // classics fix it: a 1-2-1 smoothing tap over the samples, then the
  // cell's second dot column takes the MIDPOINT to the next sample -
  // steep segments render as diagonals instead of vertical drops.
  static uint8_t sm[256];

  for (int x = 0; x <= need; x++)
  {
    int i = t + x;
    int a = m_Raw[i > 0 ? i - 1 : 0];
    int b = m_Raw[i < m_RawN ? i : m_RawN - 1];
    int c = m_Raw[i + 1 < m_RawN ? i + 1 : m_RawN - 1];

    sm[x] = (uint8_t)((a + 2 * b + c) / 4);
  }

  m_DotN = need * 2;
  for (int x = 0; x < need; x++)
  {
    // 2x vertical gain centered on mid-scale: the synth's typical
    // swing (duty ~85..170) fills all four rows instead of hugging
    // the middle; anything outside drops off the graticule like a
    // trace leaving a real scope (0xFF = no dot)
    int v0 = sm[x];
    int v1 = (sm[x] + sm[x + 1]) / 2;
    int d0 = ((v0 - 128) * 48) / 256 + 8;
    int d1 = ((v1 - 128) * 48) / 256 + 8;

    m_Dots[x * 2] = (uint8_t)((d0 < 0 ||
                               d0 > PRISMWAVES_ROWS * 4 - 1) ? 0xFF : d0);
    m_Dots[x * 2 + 1] = (uint8_t)((d1 < 0 ||
                                   d1 > PRISMWAVES_ROWS * 4 - 1) ? 0xFF
                                                                 : d1);
  }
  m_Fresh = true;
  m_DrawStep = 0;
  return true;
}

static const uint8_t s_rowbit[2][4] =   /* braille dot bits, bottom-up */
{
  { 0x40, 0x04, 0x02, 0x01 },
  { 0x80, 0x20, 0x10, 0x08 },
};

// Top + both sides of the frame, in the tab's line-drawing set
void CPrismWaves::DrawBorder(WINDOW *pWnd, int top, int x0, int cells)
{
  init_pair(SCOPE_PAIR, COLOR_CYAN, COLOR_BLACK);
  wattron(pWnd, COLOR_PAIR(SCOPE_PAIR));
  mvwaddch(pWnd, top, x0, ACS_ULCORNER);
  for (int x = 0; x < cells; x++)
    waddch(pWnd, ACS_HLINE);
  waddch(pWnd, ACS_URCORNER);
  for (int y = 0; y < PRISMWAVES_ROWS; y++)
  {
    mvwaddch(pWnd, top + 1 + y, x0, ACS_VLINE);
    mvwaddch(pWnd, top + 1 + y, x0 + cells + 1, ACS_VLINE);
  }
  wattroff(pWnd, COLOR_PAIR(SCOPE_PAIR));
}

// Waveform rows [y0, y1) of the built frame
void CPrismWaves::DrawRows(WINDOW *pWnd, int top, int x0, int cells,
                           int y0, int y1)
{
  init_pair(SCOPE_PAIR, COLOR_CYAN, COLOR_BLACK);
  wattron(pWnd, COLOR_PAIR(SCOPE_PAIR));
  for (int y = y0; y < y1; y++)
  {
    for (int cell = 0; cell < cells; cell++)
    {
      unsigned bits = 0;

      for (int dx = 0; dx < 2; dx++)
      {
        int i = cell * 2 + dx;

        if (i >= m_DotN)
          continue;
        int dot = m_Dots[i];

        if (dot == 0xFF)
          continue;                         /* off the graticule */
        int prev = i ? m_Dots[i - 1] : dot;

        if (prev == 0xFF)
          prev = dot;                       /* no connect into the void */
        int a = dot < prev ? dot : prev;    /* connect to the neighbour */
        int b = dot < prev ? prev : dot;

        for (int d = a; d <= b; d++)
        {
          int line = (PRISMWAVES_ROWS - 1) - d / 4;

          if (line == y)
            bits |= s_rowbit[dx][d & 3];
        }
      }
      mvwaddch(pWnd, top + 1 + y, x0 + 1 + cell, (chtype)(0x2800 + bits));
    }
  }
  wattroff(pWnd, COLOR_PAIR(SCOPE_PAIR));
}

// Geometry: built-frame width and centered origin, clamped to window
static int scope_geom(int dotN, int cols, int *x0)
{
  int cells = dotN / 2;

  if (cells > cols - 2)
    cells = cols - 2;
  if (cells < 1)
  {
    *x0 = 0;
    return 0;
  }
  *x0 = (cols - (cells + 2)) / 2;
  if (*x0 < 0)
    *x0 = 0;
  return cells;
}

// One-shot full repaint (tab switch / resize - not in the playback loop)
void CPrismWaves::DrawFrame(WINDOW *pWnd, int top, int cols)
{
  int x0, cells;

  if (pWnd == NULL || !m_On)
    return;
  cells = scope_geom(m_DotN, cols, &x0);
  if (cells < 1)
    return;
  DrawBorder(pWnd, top, x0, cells);
  DrawRows(pWnd, top, x0, cells, 0, PRISMWAVES_ROWS);
}

// Chunked paint: one piece + its own wrefresh per call, so pwl_env_service
// runs between chunks (the single big wrefresh was the tempo hit on fast
// songs).  Returns true when the last chunk has been drawn.
bool CPrismWaves::DrawChunk(WINDOW *pWnd, int top, int cols)
{
  if (m_DrawStep == 0)
  {
    m_DrawCells = scope_geom(m_DotN, cols, &m_DrawX0);
    if (m_DrawCells < 1)
      return true;                      // nothing to draw
    DrawBorder(pWnd, top, m_DrawX0, m_DrawCells);
    wrefresh(pWnd);
    m_DrawStep = 1;
    return false;
  }

  int y0 = (m_DrawStep - 1) * 2;        // two rows per chunk
  int y1 = y0 + 2;

  if (y1 > PRISMWAVES_ROWS)
    y1 = PRISMWAVES_ROWS;
  DrawRows(pWnd, top, m_DrawX0, m_DrawCells, y0, y1);
  wrefresh(pWnd);
  m_DrawStep++;
  if (y1 >= PRISMWAVES_ROWS)
  {
    m_DrawStep = 0;
    return true;
  }
  return false;
}
bool CPrismWaves::Service(WINDOW *pWnd, int top, int cols)
{
  if (!m_On)
    return false;

  uint32_t now = read_time();

  if (m_Collect > 0)
  {
    int v = Sample();

    if (v >= 0 && m_RawN < (int)sizeof(m_Raw))
    {
      m_Raw[m_RawN++] = (uint8_t)v;
      if (--m_Collect == 0 &&
          !BuildFrame(cols > 1 ? (cols - 1) / 1 : 60))
      {
        // flat capture: retry soon rather than waiting a full frame gap
        m_NextFrame = read_time() + us2ticks(40000u);
      }
    }
    return false;
  }

  if (m_Fresh)
  {
    if (pWnd == NULL)
    {
      m_Fresh = false;              // no window: drop the frame
      m_NextFrame = now + us2ticks(FRAME_GAP_US);
      return false;
    }
    // One chunk (border / row-pair / row-pair) per call, each with its
    // own wrefresh; the caller runs pwl_env_service between calls
    if (DrawChunk(pWnd, top, cols))
    {
      m_Fresh = false;
      m_NextFrame = now + us2ticks(FRAME_GAP_US);
    }
    return true;
  }

  if ((int32_t)(now - m_NextFrame) >= 0)
  {
    // Dots per frame: braille packs two per cell.  Default draws a
    // compact half-width wave; 'scope full' spans the tab, capped at
    // prism-tui's 236 dots (118 cells).
    // A fixed ~3.7ms frame (75% of the old 5ms): one sample per cell,
    // centered by the draw.  Fewer cells x fewer rows keeps the paint
    // small enough that fast songs (Axel-F) hold their tempo at 64MHz.
    int cells = (5000 / SAMPLE_US) * 3 / 4;     // ~58
    int maxc = (cols > 4 ? cols : 65) - 4;      // frame sides + margin

    if (cells > maxc)
      cells = maxc;
    if (cells * 2 > (int)sizeof(m_Dots))
      cells = (int)sizeof(m_Dots) / 2;
    m_Want = cells * 2;                         // 2x trigger headroom
    if (m_Want > (int)sizeof(m_Raw))
      m_Want = (int)sizeof(m_Raw);
    m_RawN = 0;
    m_Collect = m_Want;
    m_NextSample = now;
  }
  else
  {
    (void)Sample();                 // keep the deltas / reload fresh
  }
  return false;
}
