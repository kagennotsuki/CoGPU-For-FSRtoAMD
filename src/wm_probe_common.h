#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

namespace wmtest {
constexpr unsigned limit = 300, width = 1024, height = 32, inset = 32;
constexpr unsigned screen_w = 3840, screen_h = 2160;
constexpr unsigned x0 = screen_w - width - inset, y0 = screen_h - height - inset;
constexpr ULONGLONG duration = 30000;
constexpr char arm_env[] = "COGPU_WMTEST_20261111_START";
constexpr char stop_env[] = "COGPU_WMTEST_20261111_STOP";
constexpr char switch_file[] = "xess_proxy.watermark-test.txt";

inline bool Enabled(const char* dir) {
    char path[MAX_PATH];
    const size_t len = strlen(dir);
    const char* sep = len && (dir[len - 1] == '\\' || dir[len - 1] == '/') ? "" : "\\";
    const int n = snprintf(path, sizeof(path), "%s%s%s", dir, sep, switch_file);
    if (n <= 0 || n >= static_cast<int>(sizeof(path))) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    const int first = fgetc(f), next = fgetc(f);
    fclose(f);
    return first == '1' && next == EOF;
}
inline ULONGLONG ReadStart() {
    char s[32]{};
    const DWORD n = GetEnvironmentVariableA(arm_env, s, sizeof(s));
    if (n == 0 || n >= sizeof(s)) return 0;
    ULONGLONG start = 0;
    char tail = 0;
    return sscanf_s(s, "%llu%c", &start, &tail, 1u) == 1 ? start : 0;
}
inline bool WindowOpen(ULONGLONG start) {
    const auto now = GetTickCount64();
    return start && now >= start && now - start < duration && !GetEnvironmentVariableA(stop_env, nullptr, 0);
}
inline uint32_t Nonce(ULONGLONG start) {
    uint64_t v = start ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    v ^= v >> 33; v *= 0xff51afd7ed558ccdull; v ^= v >> 33;
    return static_cast<uint32_t>(v) & 0xffffffu;
}
inline uint16_t Crc(uint64_t body) {
    uint16_t c = 0xffff;
    for (int bit = 47; bit >= 0; --bit) {
        const bool mix = ((c >> 15) ^ (body >> bit)) & 1u;
        c = static_cast<uint16_t>(c << 1);
        if (mix) c ^= 0x1021u;
    }
    return c;
}
inline uint64_t Encode(ULONGLONG start, unsigned seq) {
    const uint64_t body = (0xa7ull << 40) | (static_cast<uint64_t>(Nonce(start)) << 16) | seq;
    return (body << 16) | Crc(body);
}
inline bool Decode(const uint8_t* pixels, unsigned pitch, ULONGLONG start, unsigned& seq, float& min_score) {
    uint64_t code = 0;
    min_score = 255.0f;
    for (unsigned b = 0; b < 64; ++b) {
        int sum = 0;
        for (unsigned y = 0; y < height; ++y) {
            const auto row = pixels + y * pitch;
            const int row_sign = ((y / 8) & 1u) ? -1 : 1;
            for (unsigned j = 0; j < 8; ++j) {
                const auto a = row + (b * 16 + j) * 4;
                const auto z = a + 8 * 4;
                sum += row_sign * (int(a[0]) + a[1] + a[2] - z[0] - z[1] - z[2]);
            }
        }
        const float score = float(sum) / (8 * height * 3);
        const float magnitude = score < 0 ? -score : score;
        if (magnitude < min_score) min_score = magnitude;
        code = (code << 1) | (score > 0 ? 1u : 0u);
    }
    seq = static_cast<unsigned>((code >> 16) & 0xffffu);
    return min_score >= 0.5f && seq >= 1 && seq <= limit && code == Encode(start, seq);
}
}
