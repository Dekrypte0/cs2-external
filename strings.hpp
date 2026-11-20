#pragma once
#include <string>

/* Game-identifying literals never appear in the binary: every name is stored
 * XOR-encoded and decoded at runtime, so a static string scan of the image
 * matches nothing. Trivial key — the point is plaintext absence, not entropy. */
namespace strs {
    inline constexpr unsigned short KEY = 0x2A;

    inline std::wstring DecodeArr(const unsigned short* obf, size_t n) {
        std::wstring s(n, L'\0');
        for (size_t i = 0; i < n; ++i) s[i] = wchar_t(obf[i] ^ KEY);
        return s;
    }

    template <size_t N>
    inline std::wstring Decode(const unsigned short (&obf)[N]) {
        return DecodeArr(obf, N);
    }

    // "cs2.exe"
    inline constexpr unsigned short CS2EXE[]{ 0x49,0x59,0x18,0x04,0x4F,0x52,0x4F };
    // "\cs2.exe" (image path suffix)
    inline constexpr unsigned short PATH_CS2EXE[]{ 0x76,0x49,0x59,0x18,0x04,0x4F,0x52,0x4F };
    // "client.dll"
    inline constexpr unsigned short CLIENT_DLL[]{ 0x49,0x46,0x43,0x4F,0x44,0x5E,0x04,0x4E,0x46,0x46 };
    // "engine2.dll"
    inline constexpr unsigned short ENGINE2_DLL[]{ 0x4F,0x44,0x4D,0x43,0x44,0x4F,0x18,0x04,0x4E,0x46,0x46 };
}
