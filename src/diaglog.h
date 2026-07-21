#ifndef CHROME_GREEN_SRC_DIAGLOG_H_
#define CHROME_GREEN_SRC_DIAGLOG_H_

// Diagnostic logging has been REMOVED from all builds. It previously wrote
// <AppDir>\ChromeGreen_AUMID_Diag.log unconditionally (even release) and pulled
// in <format>/<fstream>/<filesystem> — huge STL machinery bloating the DLL.
//
// We keep a no-op macro so every existing call site still compiles unchanged,
// and (unlike an inline function) the arguments are NOT evaluated at all, so
// the macro generates zero code and zero runtime cost. Remove the call sites
// later if you want to drop the dead string literals too.
#define DiagLog(...) ((void)0)

#endif  // CHROME_GREEN_SRC_DIAGLOG_H_
