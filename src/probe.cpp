// SWTDProbe: a ReShade add-on for Still Wakes the Deep (UE 5.3/5.4, DirectX 12) that captures
// the final frame the game presents, HDR or SDR, and measures it, so the stock UE ACES HDR
// output can be compared with the SDR image of the same scene.
//
// Press F9 (or use the overlay's button) to capture. The capture is taken from the back buffer
// at Present, before ReShade draws its own overlay, so the overlay is never in it. Take one
// capture with HDR on, switch HDR off in the game's menu, return to the same view and take
// another.
//
// What it writes, in SWTDProbe\ next to the game executable:
//   captures.csv                      one row per capture: mode, format, game HDR settings,
//                                     black level, mid grey, peak, clipping
//   <time>_<hdr|sdr>.txt              the full report, with a log10 nits histogram
//   <time>_<hdr|sdr>.dds              the raw back buffer (R10G10B10A2 PQ, RGBA16F scRGB or 8-bit)
//   <time>_<hdr|sdr>.bmp              a quarter-size preview to recognise the scene
//   SWTDProbe.log                     swap chain changes and capture errors
//
// Luminance is computed in the swap chain's own primaries (BT.2020 for PQ, BT.709 otherwise).
// HDR values are absolute nits. SDR values are relative to white; they are also given in nits
// for a 100-nit white, decoded both with the sRGB curve and with gamma 2.2 (what most displays
// do), because the two differ most in exactly the shadows we want to compare.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <nmmintrin.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = SWTD_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Captures " SWTD_GAME "'s final frame (HDR or SDR) and measures black level, mid grey and peak.";

namespace {

using namespace reshade::api;

constexpr int kHotkey = VK_F9;

// ---------------------------------------------------------------- files and logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
std::wstring g_dir;  // SWTDProbe\ next to the game executable, with a trailing backslash

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    g_dir = dir + L"SWTDProbe\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_log = _wfopen((g_dir + L"SWTDProbe.log").c_str(), L"a");
    Log("---- " SWTD_PRODUCT " " SWTD_VERSION " loaded");
}

// The game's own HDR settings, from the file the menu writes when a change is applied.
struct GameHdrSettings {
    std::string hdr = "?", nits = "?", uiNits = "?", gamma = "?";
};

GameHdrSettings ReadGameSettings() {
    GameHdrSettings s;
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return s;
    std::wstring ini = std::wstring(local) + L"\\Habitat\\Saved\\Config\\Windows\\GameUserSettings.ini";
    auto get = [&](const wchar_t* key) {
        wchar_t v[64];
        GetPrivateProfileStringW(L"/Script/Habitat.HabitatLocalSettings", key, L"?", v, 64, ini.c_str());
        char a[64];
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, sizeof(a), nullptr, nullptr);
        return std::string(a);
    };
    s.hdr = get(L"bUseHDRDisplayOutput");
    s.nits = get(L"HDRDisplayOutputNits");
    s.uiNits = get(L"HDRUIDisplayOutputNits");
    s.gamma = get(L"DisplayGamma");
    return s;
}

// ---------------------------------------------------------------- decoding

enum class Encoding { pq, scrgb, srgb8, srgb10, unknown };

const char* EncodingName(Encoding e) {
    switch (e) {
        case Encoding::pq: return "HDR10 PQ (BT.2020)";
        case Encoding::scrgb: return "scRGB (linear BT.709)";
        case Encoding::srgb8: return "SDR 8-bit";
        case Encoding::srgb10: return "SDR 10-bit";
        default: return "unsupported";
    }
}

const char* FormatName(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm: return "R8G8B8A8_UNORM";
        case format::r8g8b8a8_unorm_srgb: return "R8G8B8A8_UNORM_SRGB";
        case format::b8g8r8a8_unorm: return "B8G8R8A8_UNORM";
        case format::b8g8r8a8_unorm_srgb: return "B8G8R8A8_UNORM_SRGB";
        case format::r10g10b10a2_unorm: return "R10G10B10A2_UNORM";
        case format::r16g16b16a16_float: return "R16G16B16A16_FLOAT";
        default: return "other";
    }
}

const char* ColorSpaceName(color_space c) {
    switch (c) {
        case color_space::srgb: return "sRGB";
        case color_space::scrgb: return "scRGB";
        case color_space::hdr10_pq: return "HDR10 ST2084";
        case color_space::hdr10_hlg: return "HDR10 HLG";
        default: return "unknown";
    }
}

Encoding EncodingFor(format f, color_space c) {
    if (f == format::r10g10b10a2_unorm) return c == color_space::hdr10_pq ? Encoding::pq : Encoding::srgb10;
    if (f == format::r16g16b16a16_float) return Encoding::scrgb;
    if (f == format::r8g8b8a8_unorm || f == format::r8g8b8a8_unorm_srgb || f == format::b8g8r8a8_unorm ||
        f == format::b8g8r8a8_unorm_srgb)
        return Encoding::srgb8;
    return Encoding::unknown;
}

uint32_t BytesPerPixel(format f) { return f == format::r16g16b16a16_float ? 8 : 4; }

float PqToNits(float n) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    float p = std::pow(std::max(n, 0.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

float SrgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ffu, bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {  // subnormal
            exp = 127 - 15 + 1;
            while (!(man & 0x400u)) { man <<= 1; --exp; }
            bits = sign | (exp << 23) | ((man & 0x3ffu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// One pixel: the encoded channel values (0..1, or scRGB units), linear light per channel
// (nits for HDR; relative to white for SDR, using the sRGB curve), and SDR's gamma 2.2 decode.
struct Pixel {
    float code[3];
    float lin[3];
    float lin22[3];
};

Pixel Decode(Encoding e, format f, const uint8_t* p) {
    Pixel px{};
    switch (e) {
        case Encoding::pq:
        case Encoding::srgb10: {
            uint32_t v;
            memcpy(&v, p, 4);
            for (int i = 0; i < 3; ++i) px.code[i] = ((v >> (10 * i)) & 0x3ffu) / 1023.0f;
            break;
        }
        case Encoding::scrgb: {
            uint16_t h[3];
            memcpy(h, p, 6);
            for (int i = 0; i < 3; ++i) px.code[i] = HalfToFloat(h[i]);
            break;
        }
        case Encoding::srgb8: {
            bool bgr = f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb;
            px.code[0] = p[bgr ? 2 : 0] / 255.0f;
            px.code[1] = p[1] / 255.0f;
            px.code[2] = p[bgr ? 0 : 2] / 255.0f;
            break;
        }
        default: break;
    }
    for (int i = 0; i < 3; ++i) {
        float c = px.code[i];
        switch (e) {
            case Encoding::pq: px.lin[i] = PqToNits(c); break;
            case Encoding::scrgb: px.lin[i] = c * 80.0f; break;
            case Encoding::srgb8:
            case Encoding::srgb10:
                px.lin[i] = SrgbToLinear(c);
                px.lin22[i] = std::pow(c, 2.2f);
                break;
            default: break;
        }
    }
    return px;
}

// ---------------------------------------------------------------- measuring

constexpr float kHistLo = -4.0f, kHistHi = 4.5f, kHistStep = 0.25f;  // log10 nits
constexpr int kHistBins = static_cast<int>((kHistHi - kHistLo) / kHistStep);

struct Stats {
    float min = 0, p01 = 0, p1 = 0, p5 = 0, median = 0, logAverage = 0, mean = 0, p95 = 0, p99 = 0, p999 = 0,
          max = 0;
    float centreMean = 0;          // mean luminance of the centre 10% box (aim it at something)
    float maxChannel = 0;          // brightest single channel anywhere
    double fracBlack = 0;          // pixels below 0.01 nits (HDR) or 0.0001 of white (SDR)
    double fracAbove[4] = {};      // HDR: > 100, 203, 1000, 2000 nits
    double fracMaxCode = 0;        // pixels with any channel at the encoding's top code
    uint64_t hist[kHistBins] = {};
};

struct Capture {
    std::string name;  // file stem
    Encoding enc = Encoding::unknown;
    format fmt = format::unknown;
    color_space cs = color_space::unknown;
    uint32_t width = 0, height = 0;
    GameHdrSettings game;
    Stats stats;        // nits for HDR; relative to white x 100 (= nits at 100-nit white) for SDR, sRGB curve
    Stats stats22;      // SDR only: the same with gamma 2.2
    bool ok = false;
    std::string error;
};

float Percentile(std::vector<float>& v, double q) {
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// lum: per-pixel luminance in output units (nits, or nits at a 100-nit SDR white).
void Measure(std::vector<float>& lum, uint32_t width, uint32_t height, bool hdr, Stats& s) {
    size_t n = lum.size();
    if (!n) return;
    double sum = 0, logSum = 0;
    float mn = lum[0], mx = lum[0];
    size_t black = 0, above[4] = {}, centreCount = 0;
    double centreSum = 0;
    const float thresholds[4] = {100, 203, 1000, 2000};
    const float blackLevel = 0.01f;  // nits; SDR is scaled to a 100-nit white, so 0.0001 of white
    uint32_t cx0 = width * 45 / 100, cx1 = width * 55 / 100, cy0 = height * 45 / 100, cy1 = height * 55 / 100;
    for (size_t i = 0; i < n; ++i) {
        float y = lum[i];
        sum += y;
        logSum += std::log10(std::max(y, 1e-5f));
        mn = std::min(mn, y);
        mx = std::max(mx, y);
        if (y < blackLevel) ++black;
        for (int t = 0; t < 4; ++t) if (y > thresholds[t]) ++above[t];
        int b = static_cast<int>((std::log10(std::max(y, 1e-6f)) - kHistLo) / kHistStep);
        s.hist[std::clamp(b, 0, kHistBins - 1)]++;
        uint32_t x = static_cast<uint32_t>(i % width), yy = static_cast<uint32_t>(i / width);
        if (x >= cx0 && x < cx1 && yy >= cy0 && yy < cy1) { centreSum += y; ++centreCount; }
    }
    s.min = mn;
    s.max = mx;
    s.mean = static_cast<float>(sum / n);
    s.logAverage = static_cast<float>(std::pow(10.0, logSum / n));
    s.centreMean = centreCount ? static_cast<float>(centreSum / centreCount) : 0;
    s.fracBlack = static_cast<double>(black) / n;
    for (int t = 0; t < 4; ++t) s.fracAbove[t] = static_cast<double>(above[t]) / n;
    s.p01 = Percentile(lum, 0.001);
    s.p1 = Percentile(lum, 0.01);
    s.p5 = Percentile(lum, 0.05);
    s.median = Percentile(lum, 0.5);
    s.p95 = Percentile(lum, 0.95);
    s.p99 = Percentile(lum, 0.99);
    s.p999 = Percentile(lum, 0.999);
}

// ---------------------------------------------------------------- writing

void WriteDds(const std::wstring& path, format f, uint32_t width, uint32_t height, const std::vector<uint8_t>& px) {
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return;
    uint32_t dxgi = f == format::r10g10b10a2_unorm   ? 24
                    : f == format::r16g16b16a16_float ? 10
                    : f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb ? 87
                                                                                       : 28;
    uint32_t bpp = BytesPerPixel(f);
    uint32_t header[31] = {};
    header[0] = 124;                       // size
    header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8;  // caps, height, width, pixel format, pitch
    header[2] = height;
    header[3] = width;
    header[4] = width * bpp;  // pitch
    header[18] = 32;          // pixel format size
    header[19] = 0x4;         // DDPF_FOURCC
    header[20] = '0' << 24 | '1' << 16 | 'X' << 8 | 'D';  // "DX10"
    header[26] = 0x1000;      // DDSCAPS_TEXTURE
    uint32_t dx10[5] = {dxgi, 3 /* 2D */, 0, 1, 0};
    fwrite("DDS ", 1, 4, file);
    fwrite(header, 4, 31, file);
    fwrite(dx10, 4, 5, file);
    fwrite(px.data(), 1, px.size(), file);
    fclose(file);
}

// A quarter-size preview: HDR is shown with paper white at 203 nits and a soft shoulder, in
// BT.709; SDR as it was encoded.
void WriteBmp(const std::wstring& path, Encoding e, format f, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& px) {
    const uint32_t step = 4, w = width / step, h = height / step, bpp = BytesPerPixel(f);
    if (!w || !h) return;
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);  // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, file);
    fwrite(&ih, sizeof(ih), 1, file);
    std::vector<uint8_t> row(rowBytes);
    auto enc = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    };
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = px.data() + static_cast<size_t>((h - 1 - y) * step) * width * bpp;
        for (uint32_t x = 0; x < w; ++x) {
            Pixel p = Decode(e, f, src + static_cast<size_t>(x * step) * bpp);
            float rgb[3];
            if (e == Encoding::pq) {  // BT.2020 -> BT.709
                float r = p.lin[0], g = p.lin[1], b = p.lin[2];
                rgb[0] = 1.6605f * r - 0.5876f * g - 0.0728f * b;
                rgb[1] = -0.1246f * r + 1.1329f * g - 0.0083f * b;
                rgb[2] = -0.0182f * r - 0.1006f * g + 1.1187f * b;
            } else {
                for (int i = 0; i < 3; ++i) rgb[i] = p.lin[i];
            }
            for (int i = 0; i < 3; ++i) {
                float v = rgb[i];
                if (e == Encoding::pq || e == Encoding::scrgb) {
                    v = std::max(v, 0.0f) / 203.0f;
                    v = v / (1.0f + v * 0.5f) * 1.5f;  // keep 0..1 linear-ish, compress highlights
                }
                row[x * 3 + 2 - i] = enc(v);  // BGR
            }
        }
        fwrite(row.data(), 1, rowBytes, file);
    }
    fclose(file);
}

void AppendStats(std::string& out, const char* title, const Stats& s, bool hdr) {
    char buf[1600];
    const char* u = hdr ? "nits" : "nits @100-nit white";
    snprintf(buf, sizeof(buf),
             "%s (%s)\n"
             "  black level  min %.5f   0.1%% %.5f   1%% %.5f   5%% %.4f\n"
             "  mid grey     median %.3f   log-average %.3f   mean %.3f   centre box %.3f\n"
             "  peak         95%% %.1f   99%% %.1f   99.9%% %.1f   max %.1f   max channel %.1f\n"
             "  pixels       below 0.01: %.2f%%   >100: %.2f%%   >203: %.2f%%   >1000: %.3f%%   >2000: %.3f%%   "
             "at top code: %.3f%%\n",
             title, u, s.min, s.p01, s.p1, s.p5, s.median, s.logAverage, s.mean, s.centreMean, s.p95, s.p99, s.p999,
             s.max, s.maxChannel, s.fracBlack * 100, s.fracAbove[0] * 100, s.fracAbove[1] * 100,
             s.fracAbove[2] * 100, s.fracAbove[3] * 100, s.fracMaxCode * 100);
    out += buf;
}

void AppendHistogram(std::string& out, const Stats& s, size_t total) {
    out += "  histogram (log10 nits: share of pixels)\n";
    for (int b = 0; b < kHistBins; ++b) {
        if (!s.hist[b]) continue;
        double share = static_cast<double>(s.hist[b]) / total;
        char buf[160];
        int bar = static_cast<int>(share * 200 + 0.5);
        snprintf(buf, sizeof(buf), "    %6.2f..%6.2f  %7.3f%%  %s\n", kHistLo + b * kHistStep,
                 kHistLo + (b + 1) * kHistStep, share * 100, std::string(std::min(bar, 100), '#').c_str());
        out += buf;
    }
}

void WriteReports(const Capture& c, const std::vector<uint8_t>& px) {
    bool hdr = c.enc == Encoding::pq || c.enc == Encoding::scrgb;
    std::wstring stem = g_dir + std::wstring(c.name.begin(), c.name.end());
    WriteDds(stem + L".dds", c.fmt, c.width, c.height, px);
    WriteBmp(stem + L".bmp", c.enc, c.fmt, c.width, c.height, px);

    std::string text;
    char buf[512];
    snprintf(buf, sizeof(buf),
             SWTD_PRODUCT " " SWTD_VERSION " capture %s\n"
             "back buffer %ux%u %s, colour space %s -> %s\n"
             "game settings: bUseHDRDisplayOutput=%s HDRDisplayOutputNits=%s HDRUIDisplayOutputNits=%s "
             "DisplayGamma=%s\n\n",
             c.name.c_str(), c.width, c.height, FormatName(c.fmt), ColorSpaceName(c.cs), EncodingName(c.enc),
             c.game.hdr.c_str(), c.game.nits.c_str(), c.game.uiNits.c_str(), c.game.gamma.c_str());
    text += buf;
    if (hdr) {
        AppendStats(text, "luminance", c.stats, true);
        AppendHistogram(text, c.stats, static_cast<size_t>(c.width) * c.height);
    } else {
        AppendStats(text, "luminance, sRGB curve", c.stats, false);
        AppendStats(text, "luminance, gamma 2.2", c.stats22, false);
        AppendHistogram(text, c.stats22, static_cast<size_t>(c.width) * c.height);
    }
    if (FILE* f = _wfopen((stem + L".txt").c_str(), L"w")) {
        fputs(text.c_str(), f);
        fclose(f);
    }

    std::wstring csv = g_dir + L"captures.csv";
    bool fresh = GetFileAttributesW(csv.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (FILE* f = _wfopen(csv.c_str(), L"a")) {
        if (fresh)
            fputs("capture,mode,format,colour_space,width,height,game_hdr,game_peak_nits,game_ui_nits,"
                  "min,p0.1,p1,median,log_average,mean,centre,p99,p99.9,max,max_channel,pct_top_code,"
                  "g22_p0.1,g22_p1,g22_median\n",
                  f);
        const Stats& s = c.stats;
        fprintf(f, "%s,%s,%s,%s,%u,%u,%s,%s,%s,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.4f,%.5f,%.5f,%.4f\n",
                c.name.c_str(), hdr ? "HDR" : "SDR", FormatName(c.fmt), ColorSpaceName(c.cs), c.width, c.height,
                c.game.hdr.c_str(), c.game.nits.c_str(), c.game.uiNits.c_str(), s.min, s.p01, s.p1, s.median,
                s.logAverage, s.mean, s.centreMean, s.p99, s.p999, s.max, s.maxChannel, s.fracMaxCode * 100,
                hdr ? 0.0f : c.stats22.p01, hdr ? 0.0f : c.stats22.p1, hdr ? 0.0f : c.stats22.median);
        fclose(f);
    }
}

// ---------------------------------------------------------------- capture

std::mutex g_resultsMutex;
std::deque<Capture> g_results;     // newest first, for the overlay
std::atomic<bool> g_requested{false};
std::atomic<bool> g_busy{false};
std::atomic<int> g_countdown{0};   // frames until a delayed capture
bool g_keyWasDown = false;
format g_lastFormat = format::unknown;
color_space g_lastColorSpace = color_space::unknown;

void Analyse(Capture c, std::vector<uint8_t> px) {
    const bool hdr = c.enc == Encoding::pq || c.enc == Encoding::scrgb;
    const float wr = c.enc == Encoding::pq ? 0.2627f : 0.2126f, wg = c.enc == Encoding::pq ? 0.6780f : 0.7152f,
                wb = c.enc == Encoding::pq ? 0.0593f : 0.0722f;
    const float topCode = c.enc == Encoding::scrgb ? 1e30f : 1.0f;
    const size_t n = static_cast<size_t>(c.width) * c.height;
    const uint32_t bpp = BytesPerPixel(c.fmt);
    std::vector<float> lum(n), lum22(hdr ? 0 : n);
    float maxChannel = 0, maxChannel22 = 0;
    size_t atTop = 0;
    for (size_t i = 0; i < n; ++i) {
        Pixel p = Decode(c.enc, c.fmt, px.data() + i * bpp);
        float scale = hdr ? 1.0f : 100.0f;
        lum[i] = (wr * p.lin[0] + wg * p.lin[1] + wb * p.lin[2]) * scale;
        maxChannel = std::max({maxChannel, p.lin[0] * scale, p.lin[1] * scale, p.lin[2] * scale});
        if (!hdr) {
            lum22[i] = (wr * p.lin22[0] + wg * p.lin22[1] + wb * p.lin22[2]) * 100.0f;
            maxChannel22 = std::max({maxChannel22, p.lin22[0], p.lin22[1], p.lin22[2]}) ;
        }
        if (p.code[0] >= topCode || p.code[1] >= topCode || p.code[2] >= topCode) ++atTop;
    }
    Measure(lum, c.width, c.height, hdr, c.stats);
    c.stats.maxChannel = maxChannel;
    c.stats.fracMaxCode = static_cast<double>(atTop) / n;
    if (!hdr) {
        Measure(lum22, c.width, c.height, hdr, c.stats22);
        c.stats22.maxChannel = maxChannel22 * 100.0f;
        c.stats22.fracMaxCode = c.stats.fracMaxCode;
    }
    c.ok = true;
    WriteReports(c, px);
    Log("capture %s: %s, black (0.1%%) %.4f, median %.3f, peak (99.9%%) %.1f, max %.1f", c.name.c_str(),
        EncodingName(c.enc), c.stats.p01, c.stats.median, c.stats.p999, c.stats.max);
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.push_front(std::move(c));
    if (g_results.size() > 6) g_results.pop_back();
    g_busy = false;
}

void Fail(Capture c, const char* why) {
    c.error = why;
    Log("capture %s failed: %s", c.name.c_str(), why);
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.push_front(std::move(c));
    if (g_results.size() > 6) g_results.pop_back();
    g_busy = false;
}

void CaptureNow(command_queue* queue, swapchain* sc) {
    device* dev = sc->get_device();
    resource back = sc->get_current_back_buffer();
    resource_desc desc = dev->get_resource_desc(back);

    Capture c;
    c.fmt = desc.texture.format;
    c.cs = sc->get_color_space();
    c.enc = EncodingFor(c.fmt, c.cs);
    c.width = desc.texture.width;
    c.height = desc.texture.height;
    c.game = ReadGameSettings();
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    snprintf(name, sizeof(name), "%04u%02u%02u_%02u%02u%02u_%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, c.enc == Encoding::pq || c.enc == Encoding::scrgb ? "hdr" : "sdr");
    c.name = name;
    if (c.enc == Encoding::unknown) return Fail(std::move(c), "unsupported back buffer format");

    resource readback = {};
    resource_desc rd(c.width, c.height, 1, 1, c.fmt, 1, memory_heap::readback, resource_usage::copy_dest);
    if (!dev->create_resource(rd, nullptr, resource_usage::copy_dest, &readback))
        return Fail(std::move(c), "could not create the readback texture");

    command_list* cmd = queue->get_immediate_command_list();
    cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
    cmd->copy_texture_region(back, 0, nullptr, readback, 0, nullptr);
    cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
    queue->flush_immediate_command_list();
    queue->wait_idle();

    subresource_data mapped = {};
    if (!dev->map_texture_region(readback, 0, nullptr, map_access::read_only, &mapped) || !mapped.data) {
        dev->destroy_resource(readback);
        return Fail(std::move(c), "could not map the readback texture");
    }
    const uint32_t rowBytes = c.width * BytesPerPixel(c.fmt);
    std::vector<uint8_t> px(static_cast<size_t>(rowBytes) * c.height);
    for (uint32_t y = 0; y < c.height; ++y)
        memcpy(px.data() + static_cast<size_t>(y) * rowBytes,
               static_cast<const uint8_t*>(mapped.data) + static_cast<size_t>(y) * mapped.row_pitch, rowBytes);
    dev->unmap_texture_region(readback, 0);
    dev->destroy_resource(readback);

    // Decoding 8 million pixels takes a moment: do it off the render thread.
    std::thread(Analyse, std::move(c), std::move(px)).detach();
}

// ---------------------------------------------------------------- frame trace
//
// F10 records every draw and dispatch of the next whole frame (present to present) with the
// shaders bound and the render target, to SWTDProbe\trace_<time>.txt, and saves the bytecode
// of every shader used in it to SWTDProbe\shaders\<crc32c>.<stage>.cso (disassemble with
// dxc -dumpbin). The colour grading LUT pass is the one drawing into a 3D render target (or
// dispatching with a 32-deep grid); the trace lists those first.

constexpr int kTraceHotkey = VK_F10;

// CRC-32C with the SSE4.2 instruction: the game creates thousands of pipelines at start-up,
// so hashing has to be cheap.
uint32_t Crc32c(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t crc = 0xffffffffu;
    for (; size >= 8; size -= 8, p += 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        crc = _mm_crc32_u64(crc, v);
    }
    uint32_t c = static_cast<uint32_t>(crc);
    for (; size; --size, ++p) c = _mm_crc32_u8(c, *p);
    return ~c;
}

enum Stage { kVs, kGs, kPs, kCs, kStages };
const char* const kStageNames[kStages] = {"vs", "gs", "ps", "cs"};

struct PipelineInfo {
    uint32_t hash[kStages] = {};
    std::string rtFormats;
};

std::shared_mutex g_pipelineMutex;
std::unordered_map<uint64_t, PipelineInfo> g_pipelines;
// Pixel and compute shader bytecode by hash, kept for dumping (the LUT and tonemap passes
// are one or the other; vertex and geometry shaders are only hashed).
std::unordered_map<uint32_t, std::vector<uint8_t>> g_shaderCode;

// Per command list, in ReShade's private data: written only by the thread recording that list,
// so binds take no lock.
struct __declspec(uuid("6c1f0a4e-2b7d-4d61-9a43-5f2e8c1b7d90")) CmdState {
    uint64_t graphics = 0, compute = 0;
    resource_view rtv = {};
    uint32_t rtCount = 0;
};

std::atomic<int> g_traceState{0};  // 0 idle, 1 armed (starts at next present), 2 recording, 3 writing
std::mutex g_traceMutex;
std::vector<std::string> g_traceLines;
std::vector<std::string> g_traceLutLines;
std::set<std::pair<uint32_t, int>> g_traceShaders;  // (hash, stage)
bool g_traceKeyWasDown = false;
std::string g_traceStatus;

std::atomic<uint64_t> g_countInitCmd{0}, g_countBinds{0}, g_countWork{0}, g_countLazy{0}, g_countAllWork{0};
uint32_t g_tracePresents = 0;
ULONGLONG g_traceStart = 0;

void OnInitCommandList(command_list* cmd) {
    ++g_countInitCmd;
    cmd->create_private_data<CmdState>();
}

// Command lists created before the add-on loaded have no state yet: attach it on first use
// (by the thread recording that list).
CmdState* State(command_list* cmd) {
    CmdState* s = cmd->get_private_data<CmdState>();
    if (!s) {
        ++g_countLazy;
        s = cmd->create_private_data<CmdState>();
    }
    return s;
}
void OnDestroyCommandList(command_list* cmd) { cmd->destroy_private_data<CmdState>(); }

// ---------------------------------------------------------------- HDR: LUT builder replacement
//
// The game's colour grading LUT builder (UE CombineLUTs MainCS) is replaced with
// src/shaders/lutbuilder.hlsl, which builds HDR output from the game's SDR image (black floor,
// paper white, hue-preserving highlights; docs/HDR_PLAN.md). Its settings reach the shader as
// 8 root constants at b13 space50, a parameter appended to every root signature the game
// creates (where the 64-DWORD budget allows), and pushed before each LUT dispatch.
// F8 toggles it for quick A/B. The LUT is only rebuilt every frame with
// r.LUT.UpdateEveryFrame=1 in Engine.ini; otherwise changes show when grading next changes.

#include "lutbuilder_skiptemp.h"
#include "lutbuilder_temp.h"

constexpr int kToggleHotkey = VK_F8;
constexpr uint32_t kSettingsRegister = 13, kSettingsSpace = 50, kSettingsDwords = 8;
// The shader runs its stock path unless the last constant is exactly this, so a dispatch we
// missed (garbage root constants) can't turn the picture black.
constexpr float kSettingsMagic = 20261019.0f;

struct HdrSettings {
    bool enable = true;
    float paperWhite = 203.0f;
    float peak = 0.0f;  // 0: the game's menu value
    float blackFloor = 1.0f;
    float highlightSat = 0.0f;
    float knee = 0.5f;
};
HdrSettings g_hdr;
bool g_hdrLoaded = false;
bool g_toggleKeyWasDown = false;

std::shared_mutex g_lutMutex;
std::unordered_map<uint64_t, uint32_t> g_layoutParam;   // pipeline layout -> index of our constants
std::unordered_map<uint64_t, uint64_t> g_lutPipelines;  // replaced LUT pipeline -> its layout
std::atomic<uint32_t> g_layoutsExtended{0}, g_layoutsSkipped{0}, g_lutReplaced{0}, g_lutDispatches{0};
std::string g_lutStatus = "LUT shader not seen yet";

thread_local std::vector<pipeline_layout_param> t_params;

bool Contains(const void* data, size_t size, const char* needle) {
    const char* b = static_cast<const char*>(data);
    size_t n = strlen(needle);
    return std::search(b, b + size, needle, needle + n) != b + size;
}

uint32_t RootCost(const pipeline_layout_param& p) {
    switch (p.type) {
        case pipeline_layout_param_type::push_constants: return p.push_constants.count;
        case pipeline_layout_param_type::push_descriptors: return 2;
        default: return 1;
    }
}

bool OnCreatePipelineLayout(device* dev, uint32_t& count, pipeline_layout_param*& params) {
    if (dev->get_api() != device_api::d3d12) return false;
    uint32_t cost = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (params[i].type == pipeline_layout_param_type::push_constants &&
            params[i].push_constants.dx_register_space == kSettingsSpace)
            return false;  // already ours
        cost += RootCost(params[i]);
    }
    if (cost + kSettingsDwords > 64) {
        ++g_layoutsSkipped;
        return false;
    }
    t_params.assign(params, params + count);
    constant_range range;
    range.dx_register_index = kSettingsRegister;
    range.dx_register_space = kSettingsSpace;
    range.count = kSettingsDwords;
    range.visibility = shader_stage::all;
    t_params.emplace_back(range);
    params = t_params.data();
    count = static_cast<uint32_t>(t_params.size());
    ++g_layoutsExtended;
    return true;
}

void OnInitPipelineLayout(device*, uint32_t count, const pipeline_layout_param* params, pipeline_layout layout) {
    for (uint32_t i = 0; i < count; ++i) {
        if (params[i].type == pipeline_layout_param_type::push_constants &&
            params[i].push_constants.dx_register_space == kSettingsSpace) {
            std::unique_lock<std::shared_mutex> lock(g_lutMutex);
            g_layoutParam[layout.handle] = i;
            return;
        }
    }
}

void OnDestroyPipelineLayout(device*, pipeline_layout layout) {
    std::unique_lock<std::shared_mutex> lock(g_lutMutex);
    g_layoutParam.erase(layout.handle);
}

bool IsOurs(const void* code) { return code == k_lutbuilder_skiptemp || code == k_lutbuilder_temp; }

bool OnCreatePipeline(device*, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::compute_shader || !subobjects[i].data) continue;
        auto* sd = const_cast<shader_desc*>(static_cast<const shader_desc*>(subobjects[i].data));
        if (!sd->code || !sd->code_size) continue;
        // UE's CombineLUTs permutations we have a replacement for, by CRC-32C of the game's
        // DXIL. (Its reflected names are LLVM bitcode, not plain text, so they can't be matched.)
        // After a game update, find the new hash with an F10 trace (dispatch 4,4,4).
        uint32_t hash = Crc32c(sd->code, sd->code_size);
        bool temperature;
        if (hash == 0x129A2FB0u) temperature = false;  // SKIP_TEMPERATURE=1, BLENDCOUNT=1
        else continue;
        const char* skip = nullptr;
        bool layoutOk;
        {
            std::shared_lock<std::shared_mutex> lock(g_lutMutex);
            layoutOk = g_layoutParam.count(layout.handle) != 0;
        }
        if (!skip && !layoutOk) skip = "its root signature has no room for our constants";
        if (skip) {
            Log("LUT shader 0x%08X left alone: %s", hash, skip);
            continue;
        }
        sd->code = temperature ? k_lutbuilder_temp : k_lutbuilder_skiptemp;
        sd->code_size = temperature ? sizeof(k_lutbuilder_temp) : sizeof(k_lutbuilder_skiptemp);
        Log("LUT shader 0x%08X replaced (%s variant)", hash, temperature ? "white balance" : "no white balance");
        return true;
    }
    return false;
}

// Called from OnInitPipeline: remember pipelines built from our bytecode.
void LutOnInitPipeline(pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::compute_shader || !subobjects[i].data) continue;
        if (!IsOurs(static_cast<const shader_desc*>(subobjects[i].data)->code)) continue;
        std::unique_lock<std::shared_mutex> lock(g_lutMutex);
        g_lutPipelines[pipe.handle] = layout.handle;
        ++g_lutReplaced;
        Log("LUT pipeline 0x%llx created with layout 0x%llx (constants parameter %s)",
            static_cast<unsigned long long>(pipe.handle), static_cast<unsigned long long>(layout.handle),
            g_layoutParam.count(layout.handle) ? "present" : "MISSING");
    }
}

// Diagnostics: what's bound when a LUT-shaped dispatch (4,4,4) happens, logged a few times.
void LutDiagnoseDispatch(command_list* cmd, uint64_t computePipeline, uint32_t x, uint32_t y, uint32_t z) {
    static std::atomic<int> logged{0};
    if (x != 4 || y != 4 || z != 4 || logged.load() >= 6) return;
    bool ours, param;
    {
        std::shared_lock<std::shared_mutex> lock(g_lutMutex);
        auto it = g_lutPipelines.find(computePipeline);
        ours = it != g_lutPipelines.end();
        param = ours && g_layoutParam.count(it->second);
    }
    uint32_t hash = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_pipelineMutex);
        auto it = g_pipelines.find(computePipeline);
        if (it != g_pipelines.end()) hash = it->second.hash[kCs];
    }
    ++logged;
    Log("dispatch 4,4,4 on cl %p: compute pipeline 0x%llx (cs 0x%08X), ours %d, constants parameter %d",
        static_cast<void*>(cmd), static_cast<unsigned long long>(computePipeline), hash, ours, param);
}

void LutOnDestroyPipeline(pipeline pipe) {
    std::unique_lock<std::shared_mutex> lock(g_lutMutex);
    g_lutPipelines.erase(pipe.handle);
}

// Called from OnDispatch with the bound compute pipeline: push the settings before a LUT build.
void LutOnDispatch(command_list* cmd, uint64_t computePipeline) {
    uint64_t layout = 0;
    uint32_t param = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_lutMutex);
        auto it = g_lutPipelines.find(computePipeline);
        if (it == g_lutPipelines.end()) return;
        auto lp = g_layoutParam.find(it->second);
        if (lp == g_layoutParam.end()) return;
        layout = it->second;
        param = lp->second;
    }
    const HdrSettings s = g_hdr;
    float values[kSettingsDwords] = {s.enable ? 1.0f : 0.0f, s.paperWhite, s.peak, s.blackFloor, s.highlightSat,
                                     s.knee, 0, kSettingsMagic};
    cmd->push_constants(shader_stage::compute, pipeline_layout{layout}, param, 0, kSettingsDwords, values);
    ++g_lutDispatches;
}

void LoadHdrSettings(effect_runtime* rt) {
    auto get = [&](const char* key, float& v) {
        char buf[32];
        size_t n = sizeof(buf);
        if (reshade::get_config_value(rt, SWTD_PRODUCT, key, buf, &n)) v = static_cast<float>(atof(buf));
    };
    float enable = g_hdr.enable ? 1.0f : 0.0f;
    get("HdrEnable", enable);
    g_hdr.enable = enable != 0;
    get("PaperWhite", g_hdr.paperWhite);
    get("Peak", g_hdr.peak);
    get("BlackFloor", g_hdr.blackFloor);
    get("HighlightSaturation", g_hdr.highlightSat);
    get("Knee", g_hdr.knee);
    g_hdrLoaded = true;
}

void SaveHdrSettings(effect_runtime* rt) {
    auto set = [&](const char* key, float v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v);
        reshade::set_config_value(rt, SWTD_PRODUCT, key, buf);
    };
    set("HdrEnable", g_hdr.enable ? 1.0f : 0.0f);
    set("PaperWhite", g_hdr.paperWhite);
    set("Peak", g_hdr.peak);
    set("BlackFloor", g_hdr.blackFloor);
    set("HighlightSaturation", g_hdr.highlightSat);
    set("Knee", g_hdr.knee);
}

void HdrHotkeyOnPresent() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_toggleKeyWasDown) {
        g_hdr.enable = !g_hdr.enable;
        Log("HDR fix %s (F8)", g_hdr.enable ? "on" : "off");
    }
    g_toggleKeyWasDown = down;

    // Every 10 s: how often the LUT was rebuilt (with our constants). It should be every
    // rendered frame; if not, r.LUT.UpdateEveryFrame isn't set and setting changes won't show.
    static ULONGLONG last = GetTickCount64();
    static uint32_t lastBuilds = 0, presents = 0;
    ++presents;
    ULONGLONG now = GetTickCount64();
    if (now - last >= 10000) {
        uint32_t builds = g_lutDispatches.load();
        if (g_lutReplaced.load())
            Log("LUT builds in the last %.0f s: %u (%u presents)", (now - last) / 1000.0, builds - lastBuilds, presents);
        last = now;
        lastBuilds = builds;
        presents = 0;
    }
}

void DrawHdrSection(effect_runtime* rt) {
    if (!g_hdrLoaded) LoadHdrSettings(rt);
    ImGui::SeparatorText("HDR fix (replaces the game's ACES HDR)");
    bool changed = false;
    changed |= ImGui::Checkbox("Enabled (F8 toggles in game)", &g_hdr.enable);
    changed |= ImGui::SliderFloat("Paper white (nits)", &g_hdr.paperWhite, 80.0f, 400.0f, "%.0f");
    bool useGamePeak = g_hdr.peak <= 0;
    if (ImGui::Checkbox("Peak from the game's HDR brightness", &useGamePeak)) {
        g_hdr.peak = useGamePeak ? 0.0f : 1000.0f;
        changed = true;
    }
    if (!useGamePeak) changed |= ImGui::SliderFloat("Peak (nits)", &g_hdr.peak, 400.0f, 10000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Black floor (0 = sRGB, 1 = gamma 2.2)", &g_hdr.blackFloor, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Highlight saturation", &g_hdr.highlightSat, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Highlight knee (SDR level)", &g_hdr.knee, 0.2f, 0.9f, "%.2f");
    if (ImGui::Button("Defaults")) {
        g_hdr = HdrSettings{};
        changed = true;
    }
    if (changed) SaveHdrSettings(rt);

    uint32_t replaced = g_lutReplaced.load();
    if (replaced)
        ImGui::TextDisabled("LUT shader replaced (%u pipeline%s); %u LUT builds; root signatures extended %u, "
                            "skipped %u",
                            replaced, replaced == 1 ? "" : "s", g_lutDispatches.load(), g_layoutsExtended.load(),
                            g_layoutsSkipped.load());
    else
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "LUT shader not replaced yet (see SWTDProbe.log)");
}

void OnInitPipeline(device*, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    LutOnInitPipeline(layout, count, subobjects, pipe);
    PipelineInfo info;
    const shader_desc* keep[2] = {};
    uint32_t keepHash[2] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& so = subobjects[i];
        int stage = -1;
        switch (so.type) {
            case pipeline_subobject_type::vertex_shader: stage = kVs; break;
            case pipeline_subobject_type::geometry_shader: stage = kGs; break;
            case pipeline_subobject_type::pixel_shader: stage = kPs; break;
            case pipeline_subobject_type::compute_shader: stage = kCs; break;
            case pipeline_subobject_type::render_target_formats:
                for (uint32_t f = 0; f < so.count && so.data; ++f) {
                    if (!info.rtFormats.empty()) info.rtFormats += '+';
                    info.rtFormats += FormatName(static_cast<const format*>(so.data)[f]);
                }
                break;
            default: break;
        }
        if (stage < 0 || !so.data) continue;
        const shader_desc* sd = static_cast<const shader_desc*>(so.data);
        if (!sd->code || !sd->code_size) continue;
        uint32_t h = Crc32c(sd->code, sd->code_size);
        info.hash[stage] = h;
        if (stage == kPs || stage == kCs) {
            keep[stage == kCs] = sd;
            keepHash[stage == kCs] = h;
        }
    }
    std::unique_lock<std::shared_mutex> lock(g_pipelineMutex);
    g_pipelines[pipe.handle] = std::move(info);
    for (int k = 0; k < 2; ++k) {
        if (!keep[k] || g_shaderCode.count(keepHash[k])) continue;
        const uint8_t* b = static_cast<const uint8_t*>(keep[k]->code);
        g_shaderCode.emplace(keepHash[k], std::vector<uint8_t>(b, b + keep[k]->code_size));
    }
}

void OnDestroyPipeline(device*, pipeline pipe) {
    LutOnDestroyPipeline(pipe);
    std::unique_lock<std::shared_mutex> lock(g_pipelineMutex);
    g_pipelines.erase(pipe.handle);
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe) {
    ++g_countBinds;
    CmdState* s = State(cmd);
    if ((stages & pipeline_stage::compute_shader) == pipeline_stage::compute_shader) s->compute = pipe.handle;
    if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader ||
        (stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader || stages == pipeline_stage::all)
        s->graphics = pipe.handle;
}

void OnBindRenderTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view) {
    CmdState* s = State(cmd);
    s->rtCount = count;
    s->rtv = count ? rtvs[0] : resource_view{};
}

// Only while tracing: what RT0 is.
std::string TargetDesc(command_list* cmd, const CmdState& s) {
    if (!s.rtCount || !s.rtv.handle) return "no RT";
    device* dev = cmd->get_device();
    resource r = dev->get_resource_from_view(s.rtv);
    if (!r.handle) return "RT0 unknown";
    resource_desc d = dev->get_resource_desc(r);
    char buf[160];
    const char* kind = d.type == resource_type::texture_3d ? "3D" : d.type == resource_type::texture_2d ? "2D" : "other";
    snprintf(buf, sizeof(buf), "RT0 %s %ux%ux%u %s (res 0x%llx)%s", kind, d.texture.width, d.texture.height,
             static_cast<unsigned>(d.texture.depth_or_layers), FormatName(d.texture.format),
             static_cast<unsigned long long>(r.handle), s.rtCount > 1 ? " +more" : "");
    return buf;
}

std::string ShaderList(uint64_t pipe, bool compute) {
    std::shared_lock<std::shared_mutex> lock(g_pipelineMutex);
    auto it = g_pipelines.find(pipe);
    if (it == g_pipelines.end()) return "pipeline unknown";
    std::string s;
    char buf[48];
    for (int st = 0; st < kStages; ++st) {
        if ((st == kCs) != compute || !it->second.hash[st]) continue;
        snprintf(buf, sizeof(buf), "%s 0x%08X ", kStageNames[st], it->second.hash[st]);
        s += buf;
        g_traceShaders.emplace(it->second.hash[st], st);
    }
    if (!compute && !it->second.rtFormats.empty()) s += "[" + it->second.rtFormats + "]";
    return s;
}

void TraceWork(command_list* cmd, const char* what, bool compute, uint32_t a, uint32_t b, uint32_t c) {
    if (g_traceState.load() != 2) return;
    if (++g_countWork > 300000) return;  // a runaway trace
    CmdState* s = State(cmd);
    std::string target = compute ? std::string() : TargetDesc(cmd, *s);
    std::lock_guard<std::mutex> lock(g_traceMutex);
    char buf[96];
    snprintf(buf, sizeof(buf), "%5zu cl %p %-13s %u,%u,%u  ", g_traceLines.size(), static_cast<void*>(cmd), what, a,
             b, c);
    std::string line = buf + ShaderList(compute ? s->compute : s->graphics, compute);
    if (!compute) line += "  " + target;
    bool lut = compute ? c == 32 || c == 33 : target.find("RT0 3D") != std::string::npos;
    if (lut) g_traceLutLines.push_back(line);
    g_traceLines.push_back(std::move(line));
}

bool OnDraw(command_list* cmd, uint32_t v, uint32_t i, uint32_t, uint32_t) {
    ++g_countAllWork;
    TraceWork(cmd, "draw", false, v, i, 0);
    return false;
}
bool OnDrawIndexed(command_list* cmd, uint32_t n, uint32_t i, uint32_t, int32_t, uint32_t) {
    ++g_countAllWork;
    TraceWork(cmd, "draw_indexed", false, n, i, 0);
    return false;
}
bool OnDispatch(command_list* cmd, uint32_t x, uint32_t y, uint32_t z) {
    if (CmdState* s = State(cmd)) {
        LutDiagnoseDispatch(cmd, s->compute, x, y, z);
        LutOnDispatch(cmd, s->compute);
    }
    ++g_countAllWork;
    TraceWork(cmd, "dispatch", true, x, y, z);
    return false;
}
bool OnIndirect(command_list* cmd, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    ++g_countAllWork;
    bool compute = type == indirect_command::dispatch;
    TraceWork(cmd, compute ? "dispatch_ind" : "draw_ind", compute, count, 0, 0);
    return false;
}

void WriteTrace() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    snprintf(name, sizeof(name), "trace_%04u%02u%02u_%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    std::wstring shaderDir = g_dir + L"shaders\\";
    CreateDirectoryW(shaderDir.c_str(), nullptr);
    size_t dumped = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_pipelineMutex);
        for (auto& [h, st] : g_traceShaders) {
            auto it = g_shaderCode.find(h);
            if (it == g_shaderCode.end()) continue;
            wchar_t file[64];
            swprintf(file, 64, L"0x%08X.%hs.cso", h, kStageNames[st]);
            std::wstring path = shaderDir + file;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                fwrite(it->second.data(), 1, it->second.size(), f);
                fclose(f);
                ++dumped;
            }
        }
    }
    std::wstring path = g_dir + std::wstring(name, name + strlen(name));
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        fprintf(f, "%s %s frame trace: %zu draws/dispatches, %zu shaders (%zu new files), %zu pipelines known\n"
                   "Order is per command list as recorded, not GPU order. Columns: index, command list, call, "
                   "args (vertices,instances / groups x,y,z), shaders, render target.\n\n",
                SWTD_PRODUCT, SWTD_VERSION, g_traceLines.size(), g_traceShaders.size(), dumped, g_pipelines.size());
        fprintf(f, "== LUT candidates (3D render target, or dispatch 32 deep)\n");
        for (auto& l : g_traceLutLines) fprintf(f, "%s\n", l.c_str());
        fprintf(f, "\n== all\n");
        for (auto& l : g_traceLines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
    char status[160];
    snprintf(status, sizeof(status), "%s: %zu calls, %zu LUT candidates", name, g_traceLines.size(),
             g_traceLutLines.size());
    g_traceStatus = status;
    Log("trace %s; command lists initialised %llu, attached late %llu, binds %llu, work calls while recording %llu (%llu ever), %u presents over %llu ms",
        status, (unsigned long long)g_countInitCmd.load(), (unsigned long long)g_countLazy.load(),
        (unsigned long long)g_countBinds.load(), (unsigned long long)g_countWork.load(),
        (unsigned long long)g_countAllWork.load(), g_tracePresents, GetTickCount64() - g_traceStart);
}

void FrameTraceOnPresent() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kTraceHotkey) & 0x8000);
    if (down && !g_traceKeyWasDown && g_traceState.load() == 0) g_traceState = 1;
    g_traceKeyWasDown = down;

    int state = g_traceState.load();
    if (state == 1) {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        g_traceLines.clear();
        g_traceLutLines.clear();
        g_traceShaders.clear();
        g_tracePresents = 0;
        g_traceStart = GetTickCount64();
        g_traceState = 2;
    } else if (state == 2) {
        // With frame generation, presents come in back-to-back pairs, so one present-to-present
        // window can hold no game work at all: record at least 3 presents and 100 ms.
        if (++g_tracePresents < 3 || GetTickCount64() - g_traceStart < 100) return;
        g_traceState = 3;  // stop recording before writing
        std::lock_guard<std::mutex> lock(g_traceMutex);
        WriteTrace();
        g_traceState = 0;
    }
}

void OnPresent(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    resource_desc desc = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
    if (desc.texture.format != g_lastFormat || sc->get_color_space() != g_lastColorSpace) {
        g_lastFormat = desc.texture.format;
        g_lastColorSpace = sc->get_color_space();
        Log("swap chain: %ux%u %s, colour space %s", desc.texture.width, desc.texture.height,
            FormatName(g_lastFormat), ColorSpaceName(g_lastColorSpace));
    }

    // The hotkey, only while the game has focus.
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kHotkey) & 0x8000);
    if (down && !g_keyWasDown) g_requested = true;
    g_keyWasDown = down;

    int left = g_countdown.load();
    if (left > 0 && g_countdown.fetch_sub(1) == 1) g_requested = true;

    if (g_requested.exchange(false) && !g_busy.exchange(true)) CaptureNow(queue, sc);

    FrameTraceOnPresent();
    HdrHotkeyOnPresent();
}

// ---------------------------------------------------------------- overlay

void DrawStatsRow(const char* label, float hdrValue, float sdr, float sdr22, bool haveHdr, bool haveSdr,
                  const char* fmt) {
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    if (haveHdr) ImGui::Text(fmt, hdrValue); else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    if (haveSdr) ImGui::Text(fmt, sdr); else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    if (haveSdr) ImGui::Text(fmt, sdr22); else ImGui::TextDisabled("-");
}

void DrawOverlay(effect_runtime* rt) {
    DrawHdrSection(rt);
    ImGui::SeparatorText("Probe");
    ImGui::Text("Back buffer: %s, %s", FormatName(g_lastFormat), ColorSpaceName(g_lastColorSpace));
    bool busy = g_busy.load() || g_countdown.load() > 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("Capture now")) g_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Capture in 3 s")) g_countdown = 180;  // frames; close the overlay meanwhile
    if (busy) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("or press F9 in game");
    if (ImGui::Button("Trace next frame")) { if (g_traceState.load() == 0) g_traceState = 1; }
    ImGui::SameLine();
    ImGui::TextDisabled("or press F10 in game");
    if (!g_traceStatus.empty()) ImGui::TextDisabled("Last trace %s", g_traceStatus.c_str());
    ImGui::TextWrapped("Take one capture with HDR on, then turn HDR off in the game's menu, return to the same view "
                       "and take another. Files go to SWTDProbe\\ next to the game executable.");

    std::lock_guard<std::mutex> lock(g_resultsMutex);
    const Capture* hdr = nullptr;
    const Capture* sdr = nullptr;
    for (const Capture& c : g_results) {
        if (!c.ok) continue;
        bool isHdr = c.enc == Encoding::pq || c.enc == Encoding::scrgb;
        if (isHdr && !hdr) hdr = &c;
        if (!isHdr && !sdr) sdr = &c;
    }
    if (!g_results.empty() && !g_results.front().ok)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Last capture failed: %s", g_results.front().error.c_str());
    if (!hdr && !sdr) return;

    ImGui::SeparatorText("Latest HDR and SDR captures (nits; SDR at a 100-nit white)");
    if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn(hdr ? hdr->name.c_str() : "HDR");
        ImGui::TableSetupColumn(sdr ? (sdr->name + " sRGB").c_str() : "SDR sRGB");
        ImGui::TableSetupColumn("SDR gamma 2.2");
        ImGui::TableHeadersRow();
        Stats none;
        const Stats& h = hdr ? hdr->stats : none;
        const Stats& s = sdr ? sdr->stats : none;
        const Stats& g = sdr ? sdr->stats22 : none;
        DrawStatsRow("Black 0.1%", h.p01, s.p01, g.p01, hdr, sdr, "%.4f");
        DrawStatsRow("Black 1%", h.p1, s.p1, g.p1, hdr, sdr, "%.4f");
        DrawStatsRow("Median", h.median, s.median, g.median, hdr, sdr, "%.2f");
        DrawStatsRow("Log-average", h.logAverage, s.logAverage, g.logAverage, hdr, sdr, "%.2f");
        DrawStatsRow("Centre box", h.centreMean, s.centreMean, g.centreMean, hdr, sdr, "%.2f");
        DrawStatsRow("Peak 99.9%", h.p999, s.p999, g.p999, hdr, sdr, "%.1f");
        DrawStatsRow("Max", h.max, s.max, g.max, hdr, sdr, "%.1f");
        ImGui::EndTable();
    }
    if (hdr)
        ImGui::TextDisabled("HDR capture taken with game peak %s nits, UI %s nits", hdr->game.nits.c_str(),
                            hdr->game.uiNits.c_str());
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            OpenLog();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
            reshade::register_event<reshade::addon_event::create_pipeline_layout>(&OnCreatePipelineLayout);
            reshade::register_event<reshade::addon_event::init_pipeline_layout>(&OnInitPipelineLayout);
            reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(&OnDestroyPipelineLayout);
            reshade::register_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
            reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnIndirect);
            reshade::register_overlay(SWTD_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_overlay(SWTD_PRODUCT, &DrawOverlay);
            reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
            reshade::unregister_addon(module);
            if (g_log) {
                fclose(g_log);
                g_log = nullptr;
            }
            break;
    }
    return TRUE;
}
