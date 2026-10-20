// StillWakesTweaks: a ReShade add-on for Still Wakes the Deep (UE 5.4, DirectX 12) that replaces
// the game's ACES HDR output with one built from its SDR image (docs/HDR_PLAN.md):
//   - shadows and midtones as the SDR image looks on a gamma 2.2 display (black floor)
//   - SDR white at a chosen paper white (mid grey)
//   - highlights extended by luminance up to the peak, keeping SDR's hue
//
// How:
//   - The colour grading LUT builder (UE CombineLUTs MainCS, CRC-32C 0x129A2FB0) is swapped for
//     src/shaders/lutbuilder.hlsl when its pipeline is created.
//   - Its settings are 8 root constants at b13 space50. That parameter is appended only to root
//     signatures shaped like the LUT builder's (UAV table, CBV b0, CBV b1, samplers in space
//     1000); other shaders using them ignore it.
//   - Our LUT pipeline is recognised in init_pipeline, the bound compute pipeline is tracked
//     per command list, and the constants are pushed before each dispatch of ours (D3D12
//     guarantees its root signature is the current one then).
//   - The LUT is rebuilt every frame only with r.LUT.UpdateEveryFrame=1 in a read-only user
//     Engine.ini (the game deletes it otherwise).
//
// It deliberately doesn't listen for descriptor binds (bind_descriptor_tables/push_descriptors):
// with those registered, ReShade mirrors the game's descriptor heaps and start-up showed 20 s+
// of black screen. ReShade.ini [StillWakesTweaks] Events still allows them (bit 4) for testing.
//
// Writes StillWakesTweaks.log next to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <nmmintrin.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lutbuilder_skiptemp.h"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = SWTK_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replaces " SWTD_GAME "'s ACES HDR with one built from its SDR image: deep blacks, paper white, "
    "highlights that keep their colour.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;
constexpr uint32_t kGameLutHash = 0x129A2FB0u;  // CombineLUTs, SKIP_TEMPERATURE=1, BLENDCOUNT=1
constexpr uint32_t kSettingsRegister = 13, kSettingsSpace = 50, kSettingsDwords = 8;
// The shader runs its stock path unless the last constant is exactly this, so a dispatch we
// missed (garbage root constants) can't turn the picture black.
constexpr float kSettingsMagic = 20261019.0f;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring file(path, n);
    file.resize(file.find_last_of(L"\\/") + 1);
    file += L"StillWakesTweaks.log";
    g_log = _wfopen(file.c_str(), L"w");
    Log(SWTK_PRODUCT " " SWTD_VERSION " loaded");
}

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

// ---------------------------------------------------------------- settings

struct HdrSettings {
    bool enable = true;
    float paperWhite = 203.0f;
    float peak = 0.0f;  // 0: the game's HDR brightness
    float blackFloor = 1.0f;
    float highlightSat = 0.0f;
    float knee = 0.5f;
};
HdrSettings g_hdr;
bool g_loaded = false;
bool g_toggleKeyWasDown = false;

void LoadSettings(effect_runtime* rt) {
    auto get = [&](const char* key, float& v) {
        char buf[32];
        size_t n = sizeof(buf);
        if (reshade::get_config_value(rt, SWTK_PRODUCT, key, buf, &n)) v = static_cast<float>(atof(buf));
    };
    float enable = g_hdr.enable ? 1.0f : 0.0f;
    get("HdrEnable", enable);
    g_hdr.enable = enable != 0;
    get("PaperWhite", g_hdr.paperWhite);
    get("Peak", g_hdr.peak);
    get("BlackFloor", g_hdr.blackFloor);
    get("HighlightSaturation", g_hdr.highlightSat);
    get("Knee", g_hdr.knee);
    g_loaded = true;
}

void SaveSettings(effect_runtime* rt) {
    auto set = [&](const char* key, float v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v);
        reshade::set_config_value(rt, SWTK_PRODUCT, key, buf);
    };
    set("HdrEnable", g_hdr.enable ? 1.0f : 0.0f);
    set("PaperWhite", g_hdr.paperWhite);
    set("Peak", g_hdr.peak);
    set("BlackFloor", g_hdr.blackFloor);
    set("HighlightSaturation", g_hdr.highlightSat);
    set("Knee", g_hdr.knee);
}

// ---------------------------------------------------------------- root signatures

std::shared_mutex g_mutex;
std::unordered_set<uint64_t> g_extendedLayouts;  // layouts with our constants as their last parameter
std::atomic<uint32_t> g_layoutsSeen{0}, g_layoutsExtended{0}, g_lutReplaced{0}, g_pushes{0};
thread_local std::vector<pipeline_layout_param> t_params;
thread_local bool t_creatingLut = false;  // set by create_pipeline for the init_pipeline that follows

constexpr uint32_t kGameParams = 4;  // ours is appended as parameter 4

// A parameter's descriptor ranges, whichever form ReShade reports it in (UE's CBV and sampler
// tables come through as push_descriptors_with_ranges_and_flags).
uint32_t Ranges(const pipeline_layout_param& p, const descriptor_range*& first) {
    switch (p.type) {
        case pipeline_layout_param_type::descriptor_table:
        case pipeline_layout_param_type::push_descriptors_with_ranges:
            first = p.descriptor_table.ranges;
            return p.descriptor_table.count;
        case pipeline_layout_param_type::descriptor_table_with_flags:
        case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
            first = p.descriptor_table_with_flags.ranges;
            return p.descriptor_table_with_flags.count;
        default:
            first = nullptr;
            return 0;
    }
}

bool IsSingleRange(const pipeline_layout_param& p, descriptor_type type, uint32_t reg, uint32_t space, uint32_t count) {
    const descriptor_range* r;
    return Ranges(p, r) == 1 && r && r->type == type && r->dx_register_index == reg && r->dx_register_space == space &&
           r->count == count;
}

// The LUT builder's root signature (logged by 0.0.2): table{uav u0-15} | table{cbv b0} |
// table{cbv b1} | table{samplers s0-5, space 1000}.
bool IsLutShaped(uint32_t count, const pipeline_layout_param* params) {
    if (count != kGameParams) return false;
    if (!IsSingleRange(params[0], descriptor_type::unordered_access_view, 0, 0, 16)) return false;
    if (!IsSingleRange(params[1], descriptor_type::constant_buffer, 0, 0, 1)) return false;
    if (!IsSingleRange(params[2], descriptor_type::constant_buffer, 1, 0, 1)) return false;
    const descriptor_range* r;
    return Ranges(params[3], r) && r && r->type == descriptor_type::sampler && r->dx_register_space == 1000;
}

// Diagnostics: each 4-parameter root signature starting with a UAV table, raw.
void LogCandidate(const pipeline_layout_param* params) {
    std::string s;
    for (uint32_t i = 0; i < kGameParams; ++i) {
        const pipeline_layout_param& p = params[i];
        const descriptor_range* r;
        uint32_t n = Ranges(p, r);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s[type %u, %u ranges", i ? " " : "", static_cast<uint32_t>(p.type), n);
        s += buf;
        if (r) {
            snprintf(buf, sizeof(buf), ": desc %u reg %u space %u count %u array %u", static_cast<uint32_t>(r->type),
                     r->dx_register_index, r->dx_register_space, r->count, r->array_size);
            s += buf;
        }
        s += "]";
    }
    if (s.find("desc 3 reg 0") == std::string::npos && s.find("desc 4 reg 0") == std::string::npos &&
        s.find("desc 5 reg 0") == std::string::npos)
        return;
    Log("4-parameter root signature: %s -> %s", s.c_str(), IsLutShaped(kGameParams, params) ? "LUT-shaped" : "no");
}

bool OnCreatePipelineLayout(device* dev, uint32_t& count, pipeline_layout_param*& params) {
    if (dev->get_api() != device_api::d3d12) return false;
    if (count == kGameParams) LogCandidate(params);
    if (!IsLutShaped(count, params)) return false;
    t_params.assign(params, params + count);
    constant_range range;
    range.dx_register_index = kSettingsRegister;
    range.dx_register_space = kSettingsSpace;
    range.count = kSettingsDwords;
    range.visibility = shader_stage::all;
    t_params.emplace_back(range);
    params = t_params.data();
    count = static_cast<uint32_t>(t_params.size());
    return true;
}

void OnInitPipelineLayout(device*, uint32_t count, const pipeline_layout_param* params, pipeline_layout layout) {
    ++g_layoutsSeen;
    if (count != kGameParams + 1) return;
    const pipeline_layout_param& last = params[kGameParams];
    if (last.type != pipeline_layout_param_type::push_constants || last.push_constants.dx_register_space != kSettingsSpace)
        return;
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_extendedLayouts.insert(layout.handle);
    uint32_t n = ++g_layoutsExtended;
    Log("root signature 0x%llx extended (%u so far)", static_cast<unsigned long long>(layout.handle), n);
}

void OnDestroyPipelineLayout(device*, pipeline_layout layout) {
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_extendedLayouts.erase(layout.handle);
}

bool IsExtended(uint64_t layout) {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    return g_extendedLayouts.count(layout) != 0;
}

// ---------------------------------------------------------------- the LUT shader

bool OnCreatePipeline(device*, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::compute_shader || !subobjects[i].data) continue;
        auto* sd = const_cast<shader_desc*>(static_cast<const shader_desc*>(subobjects[i].data));
        if (!sd->code || sd->code_size < 16 || Crc32c(sd->code, sd->code_size) != kGameLutHash) return false;
        if (!IsExtended(layout.handle)) {
            Log("LUT shader found, but its root signature 0x%llx wasn't extended: left alone",
                static_cast<unsigned long long>(layout.handle));
            return false;
        }
        sd->code = k_lutbuilder_skiptemp;
        sd->code_size = sizeof(k_lutbuilder_skiptemp);
        t_creatingLut = true;
        ++g_lutReplaced;
        Log("LUT shader replaced");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- pushing the settings

// The compute root signature in use, per command list, in ReShade's private data (written only
// by the thread recording that list, so no lock).
struct __declspec(uuid("3f0b8f5e-7a3c-4b0e-9d61-2c8f4a6e1b37")) CmdState {
    uint64_t computeLayout = 0;    // from descriptor binds (Events bit 4)
    uint64_t computePipeline = 0;  // from pipeline binds (Events bit 16)
};

// Our LUT pipelines and their (extended) root signature, from init_pipeline (Events bit 8).
std::unordered_map<uint64_t, uint64_t> g_lutPipelines;

void OnInitPipeline(device*, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects,
                    pipeline pipe) {
    bool ours = t_creatingLut;
    for (uint32_t i = 0; i < count && !ours; ++i)
        ours = subobjects[i].type == pipeline_subobject_type::compute_shader && subobjects[i].data &&
               static_cast<const shader_desc*>(subobjects[i].data)->code == k_lutbuilder_skiptemp;
    t_creatingLut = false;
    if (!ours) return;
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_lutPipelines[pipe.handle] = layout.handle;
    Log("LUT pipeline 0x%llx tracked", static_cast<unsigned long long>(pipe.handle));
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_lutPipelines.erase(pipe.handle);
}

uint64_t LutPipelineLayout(uint64_t pipe) {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    auto it = g_lutPipelines.find(pipe);
    return it == g_lutPipelines.end() ? 0 : it->second;
}

CmdState* State(command_list* cmd) {
    CmdState* s = cmd->get_private_data<CmdState>();
    return s ? s : cmd->create_private_data<CmdState>();
}

void OnInitCommandList(command_list* cmd) { cmd->create_private_data<CmdState>(); }
void OnDestroyCommandList(command_list* cmd) { cmd->destroy_private_data<CmdState>(); }

void NoteLayout(command_list* cmd, shader_stage stages, pipeline_layout layout) {
    if ((stages & shader_stage::compute) == shader_stage::compute) State(cmd)->computeLayout = layout.handle;
}

void OnBindDescriptorTables(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t, uint32_t,
                            const descriptor_table*, uint32_t, const uint32_t*) {
    NoteLayout(cmd, stages, layout);
}

void OnPushDescriptors(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t,
                       const descriptor_table_update&) {
    NoteLayout(cmd, stages, layout);
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe) {
    if ((stages & pipeline_stage::compute_shader) == pipeline_stage::compute_shader)
        State(cmd)->computePipeline = pipe.handle;
}

bool OnDispatch(command_list* cmd, uint32_t, uint32_t, uint32_t) {
    CmdState* st = State(cmd);
    // Our LUT pipeline bound: D3D12 requires its root signature to be the current one.
    uint64_t layout = st->computePipeline ? LutPipelineLayout(st->computePipeline) : 0;
    if (!layout && st->computeLayout && IsExtended(st->computeLayout)) layout = st->computeLayout;
    if (!layout) return false;
    const HdrSettings s = g_hdr;
    float values[kSettingsDwords] = {s.enable ? 1.0f : 0.0f, s.paperWhite, s.peak, s.blackFloor, s.highlightSat,
                                     s.knee, 0, kSettingsMagic};
    cmd->push_constants(shader_stage::compute, pipeline_layout{layout}, kGameParams, 0, kSettingsDwords, values);
    if (g_pushes.fetch_add(1) == 0) Log("first settings push");
    return false;
}

// ---------------------------------------------------------------- present and overlay

bool g_firstPresent = false;
color_space g_lastColorSpace = color_space::unknown;

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (!g_firstPresent) {
        g_firstPresent = true;
        Log("first present (%u root signatures so far, %u extended)", g_layoutsSeen.load(), g_layoutsExtended.load());
    }
    if (sc->get_color_space() != g_lastColorSpace) {
        g_lastColorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_lastColorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_lastColorSpace == color_space::srgb  ? "sRGB"
                                                                                   : "other");
    }

    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_toggleKeyWasDown) {
        g_hdr.enable = !g_hdr.enable;
        Log("HDR fix %s (F8)", g_hdr.enable ? "on" : "off");
    }
    g_toggleKeyWasDown = down;
}

void DrawOverlay(effect_runtime* rt) {
    if (!g_loaded) LoadSettings(rt);
    bool changed = false;
    changed |= ImGui::Checkbox("HDR fix (F8 toggles in game)", &g_hdr.enable);
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
    if (changed) SaveSettings(rt);

    if (g_lutReplaced.load())
        ImGui::TextDisabled("LUT shader replaced; root signatures extended %u of %u; %u settings pushes",
                            g_layoutsExtended.load(), g_layoutsSeen.load(), g_pushes.load());
    else
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "LUT shader not replaced yet (see StillWakesTweaks.log)");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
        {
            // ReShade.ini [StillWakesTweaks] Events: which event groups to listen for, for
            // finding what slows the game's start: 1 root signatures, 2 pipeline creation,
            // 4 descriptor binds (slow: ReShade then mirrors the descriptor heaps),
            // 8 pipeline init/destroy, 16 pipeline binds and dispatches. Default 27 (no 4).
            int events = 27;
            reshade::get_config_value(nullptr, SWTK_PRODUCT, "Events", events);
            Log("Events=%d", events);
            if (events & 1) {
                reshade::register_event<reshade::addon_event::create_pipeline_layout>(&OnCreatePipelineLayout);
                reshade::register_event<reshade::addon_event::init_pipeline_layout>(&OnInitPipelineLayout);
                reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(&OnDestroyPipelineLayout);
            }
            if (events & 2) reshade::register_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
            if (events & 4) {
                reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindDescriptorTables);
                reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
            }
            if (events & 8) {
                reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
                reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            }
            if (events & 16) reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            if (events & (4 | 16)) {
                reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCommandList);
                reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
                reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            }
        }
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(SWTK_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_addon(module);
            if (g_log) {
                fclose(g_log);
                g_log = nullptr;
            }
            break;
    }
    return TRUE;
}
