#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <Windows.h>

namespace stealth {

// Written at runtime from process environment state, so the compiler cannot
// inspect it and fold the decoder back into plaintext.
extern volatile unsigned char g_decode_key;

template <std::size_t N>
constexpr std::array<char, N> Encrypt(const char (&text)[N]) noexcept {
    std::array<char, N> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<char>(text[i] ^ 0x5Au);
    return out;
}

inline std::string Decode(const char* data, std::size_t length) {
    const unsigned char key = g_decode_key;
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i)
        out.push_back(static_cast<char>(data[i] ^ key));
    return out;
}

} // namespace stealth

// The constexpr table is a local static, so only the encrypted bytes reach the
// binary; the source literal is consumed at compile time.
#define OBF(text) ([] { \
    static constexpr auto obf_table_ = ::stealth::Encrypt<sizeof(text)>(text); \
    return ::stealth::Decode(obf_table_.data(), sizeof(text) - 1); \
}())
