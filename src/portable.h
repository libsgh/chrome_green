#ifndef CHROME_GREEN_SRC_PORTABLE_H_
#define CHROME_GREEN_SRC_PORTABLE_H_

#include <windows.h>

#include <string>

void Portable(LPWSTR param);

// Inject the configured portable paths (--user-data-dir / --disk-cache-dir /
// config command line) into the current process's PEB command line when they
// are missing. Must run BEFORE Chrome's entry point (Loader → ChromeGreenCommand)
// so Chrome picks up the portable identity even when launched via a pinned /
// desktop shortcut that only carries --portable.
void ApplyPortableArgsToCommandLine();

#endif  // CHROME_GREEN_SRC_PORTABLE_H_
