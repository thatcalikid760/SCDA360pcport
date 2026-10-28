// Title lifecycle for in-process relaunch (XamLoaderLaunchTitle): every title thread (main +
// ExCreateThread) is registered; a stop makes each one exit at its next import entry or HLE
// wait, and terminates the ones spinning in guest code. The interrupt and audio threads are
// title threads too (restarted for each instance on persistent service-region contexts).
#pragma once
#include <windows.h>
#include <cstdint>

// The current host thread is (no longer) a title thread with this guest thread id.
void TitleThreadRegister(uint32_t id);
// A title thread just created (maybe suspended): stopped with the title even if it never runs.
void TitleThreadAdopt(HANDLE thread, uint32_t id);
void TitleThreadUnregister();

// True from a stop request until TitleRestarted.
bool TitleStopping();
// A stopping title thread exits here (no-op for other threads or a running title).
void TitleCheckExit();
// Ends the current thread (ExTerminateThread, stops): unregisters it first.
[[noreturn]] void TitleThreadExit();

// TitleImportScope (declared in runtime.h): every SCDA_IMPORT body runs inside one. It exits a
// stopping title thread at entry and counts the thread's import depth (a thread is terminated
// only outside imports).

// WaitForMultipleObjectsEx / Sleep for HLE waits: a title thread exits when a stop arrives.
DWORD TitleWait(DWORD count, const HANDLE* handles, bool waitAll, DWORD ms, bool alertable);
void TitleSleep(DWORD ms);

// Relaunch: called on a title thread (XamLoaderLaunchTitle); never returns.
[[noreturn]] void TitleRequestRelaunch();
// Manager side: signalled by TitleRequestRelaunch (or SCDA_FORCE_RELAUNCH).
HANDLE TitleRelaunchEvent();
void TitleForceRelaunch();
// Manager side: stops every title thread. Logs what happened.
void TitleStopAll();
// Manager side, after the reset: title threads may run again.
void TitleRestarted();
