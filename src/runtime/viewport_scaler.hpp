#pragma once

struct ID3D11DeviceContext;

namespace nfsheatvr {

// Expands only Frostbite viewports that still use the physical desktop size
// after their matching scene texture was expanded by the Heat VR runtime.
bool InstallFrostbiteViewportScaler(ID3D11DeviceContext* context);
void RemoveFrostbiteViewportScaler();

} // namespace nfsheatvr
