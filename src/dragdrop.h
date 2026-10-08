#ifndef CHROME_GREEN_SRC_DRAGDROP_H_
#define CHROME_GREEN_SRC_DRAGDROP_H_

#include <windows.h>

// Installs the OLE drop-target hook when drag_link_new_tab is on.
void DragLinkNewTab();

// Opens a URL parked by a completed drop. Driven by the mouse-move stream
// because Drop() runs inside OLE's modal drag loop and must not act inline.
bool FlushPendingDragDrop();

// Screen position of the last left-button press, i.e. where the drag started
// and therefore where the dragged link is. Invalid before the first click.
// Recorded by the mouse hook so the drop path can replay a Ctrl+click there.
POINT GetDragStartPoint();

// Ctrl+clicks `pt`: moves the pointer there, holds Ctrl, clicks and releases.
// This is the native gesture for "open a link in a background tab next to the
// current one", so its placement and focus behaviour come from Chrome itself.
void CtrlClickAt(POINT pt);

#endif  // CHROME_GREEN_SRC_DRAGDROP_H_
