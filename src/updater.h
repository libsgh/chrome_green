#ifndef CHROME_GREEN_SRC_UPDATER_H_
#define CHROME_GREEN_SRC_UPDATER_H_

#include <string>

// Initialize the updater module.
// Called from ChromeGreen() in the browser process.
// - Loads saved state
// - Starts the localhost HTTP server
// - If auto_check is enabled and enough time has passed, triggers a check
// - If state is Ready and user confirmed, applies the update
void InitUpdater();

// Trigger an update check (Omaha protocol) on a background thread.
void TriggerUpdateCheck();

// Trigger a download of the latest available update.
// If a local .7z package exists in the updates/ folder, it will be used
// instead of downloading from the internet (offline install mode).
void TriggerDownload();

// Check if a local Chrome update .7z exists in the updates/ folder.
// Returns true if a local package is available for offline install.
bool HasLocalPackage();

// Check if a pending update should be applied on this launch.
// If state is Ready, extracts the 7z and replaces Chrome files.
// Returns true if an update was applied (Chrome should restart).
bool ApplyPendingUpdate();

// Launch the standalone GUI update window. The window exe is embedded as an
// RCDATA resource inside version.dll; this extracts it to
// <install>\updates\chrome_green_updater.exe, writes a manifest next to it,
// and launches it detached (CREATE_BREAKAWAY_FROM_JOB) so it survives Chrome
// exiting. Returns false on failure.
bool LaunchUpdateWindow();

// Shutdown the updater (stop HTTP server, wait for threads).
void ShutdownUpdater();


#endif  // CHROME_GREEN_SRC_UPDATER_H_
