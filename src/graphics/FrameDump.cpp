// ---------------------------------------------------------------------------
//  FrameDump.cpp
// ---------------------------------------------------------------------------
#include "FrameDump.h"

#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace dragonfly {

namespace {

void WriteU16(std::ofstream& file, uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 2);
}

void WriteU32(std::ofstream& file, uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value & 0xFF),
                              static_cast<uint8_t>((value >> 8) & 0xFF),
                              static_cast<uint8_t>((value >> 16) & 0xFF),
                              static_cast<uint8_t>((value >> 24) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
}

// Decodes one pixel to 8-bit RGB, handling the two formats this program sees:
// the BGRA desktop format and the RGBA back buffer.
void DecodePixel(DXGI_FORMAT format, const uint8_t* source, uint8_t& r, uint8_t& g,
                 uint8_t& b) {
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        b = source[0];
        g = source[1];
        r = source[2];
    } else {
        r = source[0];
        g = source[1];
        b = source[2];
    }
}

} // namespace

bool DumpTextureToBmp(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* source, const std::string& path) {
    if (!device || !context || !source) {
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);

    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        return false;  // formats this simple dumper does not handle
    }

    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
        return false;
    }

    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const uint32_t width = description.Width;
    const uint32_t height = description.Height;
    const uint32_t rowSize = width * 3;
    const uint32_t padding = (4 - (rowSize % 4)) % 4;
    const uint32_t imageSize = (rowSize + padding) * height;
    const uint32_t fileSize = 14 + 40 + imageSize;

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        context->Unmap(staging.Get(), 0);
        return false;
    }

    // BITMAPFILEHEADER
    file.write("BM", 2);
    WriteU32(file, fileSize);
    WriteU16(file, 0);
    WriteU16(file, 0);
    WriteU32(file, 14 + 40);

    // BITMAPINFOHEADER
    WriteU32(file, 40);
    WriteU32(file, width);
    WriteU32(file, height);
    WriteU16(file, 1);
    WriteU16(file, 24);
    WriteU32(file, 0);
    WriteU32(file, imageSize);
    WriteU32(file, 2835);
    WriteU32(file, 2835);
    WriteU32(file, 0);
    WriteU32(file, 0);

    // Pixels, bottom-up, BGR: BMP row 0 is the bottom of the image, so the
    // source rows are walked in reverse.
    std::vector<uint8_t> row(rowSize + padding, 0);
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t index = 0; index < height; ++index) {
        const uint32_t y = height - 1 - index;
        const uint8_t* source_row = base + static_cast<size_t>(y) * mapped.RowPitch;
        std::memset(row.data(), 0, row.size());
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            DecodePixel(description.Format, source_row + static_cast<size_t>(x) * 4, r, g, b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        file.write(reinterpret_cast<const char*>(row.data()),
                   static_cast<std::streamsize>(row.size()));
    }

    context->Unmap(staging.Get(), 0);
    return file.good();
}

} // namespace dragonfly
