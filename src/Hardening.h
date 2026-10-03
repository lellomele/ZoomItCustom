#pragma once
#include <bit>
#include <cstdint>
inline WPARAM EncodeZoomLevel(float level) noexcept { return std::bit_cast<uint32_t>(level); }
inline float DecodeZoomLevel(WPARAM value) noexcept { return std::bit_cast<float>(static_cast<uint32_t>(value)); }
inline void DeleteSelectedFont(HDC dc, HFONT& font) noexcept {
    if(font && dc && GetCurrentObject(dc,OBJ_FONT)==font) SelectObject(dc,GetStockObject(SYSTEM_FONT));
    if(font) { DeleteObject(font); font=nullptr; }
}
inline void DeleteSelectedPen(HDC dc, HPEN& pen) noexcept {
    if(pen && dc && GetCurrentObject(dc,OBJ_PEN)==pen) SelectObject(dc,GetStockObject(BLACK_PEN));
    if(pen) { DeleteObject(pen); pen=nullptr; }
}

