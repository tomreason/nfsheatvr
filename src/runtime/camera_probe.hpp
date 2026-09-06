#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace nfsheatvr {

bool InstallCameraDrawProbe(ID3D11Device* device, ID3D11DeviceContext* context);
void RemoveCameraDrawProbe();
void RequestCameraDrawProbe();

} // namespace nfsheatvr
