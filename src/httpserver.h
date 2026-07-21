#ifndef CHROME_GREEN_SRC_HTTPSERVER_H_
#define CHROME_GREEN_SRC_HTTPSERVER_H_

#include <atomic>
#include <string>
#include <thread>

// Reserve a free per-install port for the config HTTP server.
// MUST be called BEFORE PakPatch() so the config-page links baked into the
// browser UI use the exact port the server binds. Picks the per-install base
// port (GetConfigPort, derived from the install dir) and, if already occupied
// by another local service, scans upward for a free port, binding it
// immediately so nothing else can grab it. Returns the chosen port.
int ReserveConfigServerPort();

// Port the config server will actually use. Equals the reserved port after
// ReserveConfigServerPort(), or falls back to the per-install base if
// reservation was never called (e.g. a renderer subprocess). Use this when
// baking links so they always match the live server.
int GetConfigServerPort();

// Start the localhost HTTP server on the reserved port (see above).
// Serves the embedded Vue3 config page and JSON API.
// Runs on a background thread; returns immediately.
void StartHttpServer();

// Stop the HTTP server.
void StopHttpServer();

// Check if the server is running.
bool IsHttpServerRunning();

#endif  // CHROME_GREEN_SRC_HTTPSERVER_H_
