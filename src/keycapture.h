#ifndef CHROME_GREEN_SRC_KEYCAPTURE_H_
#define CHROME_GREEN_SRC_KEYCAPTURE_H_

#include <string>

// Backend-assisted shortcut capture.
//
// While the config page is recording a hotkey, Chrome's own accelerators
// (Ctrl+T/W, F5, F11, F12, ...) would fire before the page can read them,
// ruining the capture. To give recording priority we install a highest
// priority keyboard hook that, while capture mode is active, swallows every
// key so Chrome never acts on it, and reports the captured combination back
// to the frontend over the existing SSE channel.

// Begin capturing the next key combination. Any subsequent key press is
// swallowed (Chrome sees nothing) and reported via the SSE "key_capture"
// event. Capture is one-shot: it ends after a successful capture or a cancel.
void StartKeyCapture();

// Abort an in-progress capture (e.g. when the user clicks away).
void StopKeyCapture();

// Register the capture keyboard handler. Must be called once at startup,
// before InstallInputHooks() so the handler is in the list.
void InitKeyCapture();

// Returns true if a capture event is pending and writes its JSON payload
// (e.g. {"value":"Ctrl+T"} or {"cancel":true}) into `out`, clearing it.
bool ConsumeKeyCaptureEvent(std::string& out);

#endif  // CHROME_GREEN_SRC_KEYCAPTURE_H_
