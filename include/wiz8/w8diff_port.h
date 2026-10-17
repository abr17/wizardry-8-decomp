/* Differential harness hook (mod, 2026-10-13): RenderFrame calls this once
   per frame. Does nothing unless wiz8wide.ini has [Debug] DiffHarness=1. */
#ifndef WIZ8_W8DIFF_PORT_H
#define WIZ8_W8DIFF_PORT_H

void W8DiffPortPoll(void);

#endif
