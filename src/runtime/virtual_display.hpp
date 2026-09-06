#pragma once

namespace nfsheatvr {

// Presents a configurable desktop size to Frostbite only.  It never creates a
// Windows display, changes the user's monitor mode, or affects another process.
bool InstallHeatVirtualDisplay();

// Used by the DXGI mode shim after the process-local desktop facade has been
// armed. Returns false when the option was deliberately disabled in the panel.
bool GetHeatVirtualDisplayResolution(unsigned* width, unsigned* height);

} // namespace nfsheatvr
