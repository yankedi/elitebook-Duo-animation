// ---------------------------------------------------------------------------
//  FrameDump.h
//
//  Writes a D3D11 texture to a BMP file.
//
//  Added while bringing the fold effect up: screen captures taken through GDI
//  cannot see D3D-rendered content, so the only trustworthy way to inspect what
//  the shader actually produced is to have the program save its own textures.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace dragonfly {

// Copies `source` to a staging texture and writes it out as a 24-bit BMP.
// Returns false if the copy or the file write fails.
bool DumpTextureToBmp(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* source, const std::string& path);

} // namespace dragonfly
