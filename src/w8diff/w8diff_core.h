/* Differential test harness (mod, 2026-10-13). Not part of the original game.

   The same core is built into Wiz8Runtime.exe and into w8diff.dll, which the
   widescreen patch (version.dll) loads into the retail Wiz8.exe. Both run one
   plan of calls - function address, arguments, memory set up before the call -
   and write what came back, so a script can compare the rebuilt functions with
   the retail ones on identical inputs. See wiz8-widescreen docs/w8diff.md. */
#ifndef W8DIFF_CORE_H
#define W8DIFF_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct W8DiffHost {
    /* "retail" or "port": picks w8diff_<name>.req / _plan_ / _out_ / _done_. */
    const char* name;
    /* Game folder with a trailing backslash. */
    const char* dir;
    /* srand of the CRT the game code calls rand() from. */
    void(__cdecl* seed)(unsigned int);
} W8DiffHost;

/* Call once per frame on the game thread. Idle: checks for the request file
   every quarter second. Running: executes cases for up to ~150 ms per call.
   Returns nonzero while a run is in progress. */
int W8DiffPoll(const W8DiffHost* host);

#ifdef __cplusplus
}
#endif

#endif
