#pragma once
/* The installer's window: docs/PLAN-installer-ui.md §3.

   One window, two columns, no dialogs.  The left column lists the locations
   found (or the empty state), the right column shows the snapshot of the
   selected one plus exactly one main action.  Scanning, hashing and every write
   happen on a worker thread; the UI thread only paints and posts. */

#include <windows.h>

#include <string>

namespace tc::setup {

/* `initial` is the directory given on the command line, if any; it becomes the
   first candidate and is labeled as such.  Returns the process exit code. */
int run_window(HINSTANCE instance, const std::wstring& initial);

}  // namespace tc::setup
