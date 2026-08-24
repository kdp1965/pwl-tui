/************************************************************************************
 * tui/TuiMain.cxx
 *
 * Entry point for the 'tui' console command: builds the CPwlSynth source
 * and the CTui screen, runs the (single threaded) UI loop until the user
 * exits, then hands the terminal back to the plain CLI.
 ************************************************************************************/

#include <stdio.h>

#include "Tui.h"
#include "PwlSynth.h"

extern "C" {
#include "../pwl_test.h"
}

extern "C" void tui_run(void)
{
  CPwlSynth *pPwl = new CPwlSynth;
  CTui      *pTui = new CTui;

  if (pPwl == NULL || pTui == NULL)
  {
    printf("tui: out of memory\n");
    delete pTui;
    delete pPwl;
    return;
  }

  pTui->m_pPrompt = "pwl> ";
  pTui->AttachTuiSource(pPwl);

  // CLI command printf output -> TUI command window, note stream -> the
  // per-channel Notes tab
  pPwl->InstallStdoutHook();

  pTui->RunThread();

  pPwl->RemoveStdoutHook();

  delete pTui;
  delete pPwl;

  printf("\n(tui closed)\n");
}
