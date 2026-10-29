#include "vjoy.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace {

// HID usages, as in vJoy's public.h.
const std::map<std::string, UINT> kAxisUsage = {
    {"X", 0x30}, {"Y", 0x31}, {"Z", 0x32}, {"RX", 0x33}, {"RY", 0x34}, {"RZ", 0x35}, {"SL0", 0x36}, {"SL1", 0x37},
};

enum VjdStat { VJD_STAT_OWN, VJD_STAT_FREE, VJD_STAT_BUSY, VJD_STAT_MISS, VJD_STAT_UNKN };

} // namespace

struct VJoy::Api {
    HMODULE dll = nullptr;
    BOOL(__cdecl* vJoyEnabled)() = nullptr;
    int(__cdecl* GetVJDStatus)(UINT) = nullptr;
    BOOL(__cdecl* AcquireVJD)(UINT) = nullptr;
    VOID(__cdecl* RelinquishVJD)(UINT) = nullptr;
    BOOL(__cdecl* ResetVJD)(UINT) = nullptr;
    BOOL(__cdecl* GetVJDAxisExist)(UINT, UINT) = nullptr;
    BOOL(__cdecl* GetVJDAxisMin)(UINT, UINT, LONG*) = nullptr;
    BOOL(__cdecl* GetVJDAxisMax)(UINT, UINT, LONG*) = nullptr;
    BOOL(__cdecl* SetAxis)(LONG, UINT, UINT) = nullptr;
    BOOL(__cdecl* SetBtn)(BOOL, UINT, UCHAR) = nullptr;
};

namespace {

template <typename T>
bool Load(HMODULE module, const char* name, T& function, std::string& error) {
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!function) error = std::string("vJoyInterface.dll: missing export ") + name;
    return function != nullptr;
}

} // namespace

VJoy::~VJoy() {
    Close();
}

bool VJoy::Open(const std::wstring& dllPath, unsigned device, std::string& error) {
    Close();
    api_ = new Api;
    // vJoyInterface.dll depends on the VC++ runtime DLLs shipped in its own folder.
    api_->dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!api_->dll) {
        error = "cannot load vJoyInterface.dll (Windows error " + std::to_string(GetLastError()) +
                "; is the 32-bit vJoy installed?)";
        return false;
    }
    if (!Load(api_->dll, "vJoyEnabled", api_->vJoyEnabled, error) ||
        !Load(api_->dll, "GetVJDStatus", api_->GetVJDStatus, error) ||
        !Load(api_->dll, "AcquireVJD", api_->AcquireVJD, error) ||
        !Load(api_->dll, "RelinquishVJD", api_->RelinquishVJD, error) ||
        !Load(api_->dll, "ResetVJD", api_->ResetVJD, error) ||
        !Load(api_->dll, "GetVJDAxisExist", api_->GetVJDAxisExist, error) ||
        !Load(api_->dll, "GetVJDAxisMin", api_->GetVJDAxisMin, error) ||
        !Load(api_->dll, "GetVJDAxisMax", api_->GetVJDAxisMax, error) ||
        !Load(api_->dll, "SetAxis", api_->SetAxis, error) || !Load(api_->dll, "SetBtn", api_->SetBtn, error)) {
        return false;
    }
    if (!api_->vJoyEnabled()) {
        error = "vJoy driver is not enabled";
        return false;
    }
    const int status = api_->GetVJDStatus(device);
    if (status == VJD_STAT_BUSY) {
        error = "vJoy device " + std::to_string(device) + " is used by another program";
        return false;
    }
    if (status == VJD_STAT_MISS) {
        error = "vJoy device " + std::to_string(device) + " is not configured (use 'Configure vJoy')";
        return false;
    }
    if (!api_->AcquireVJD(device)) {
        error = "cannot acquire vJoy device " + std::to_string(device);
        return false;
    }
    device_ = device;
    acquired_ = true;
    api_->ResetVJD(device_);
    return true;
}

void VJoy::Close() {
    // No reset here: it would centre the axes, i.e. half-open airbrakes in Condor.
    if (api_ && acquired_) api_->RelinquishVJD(device_);
    acquired_ = false;
    delete api_;
    api_ = nullptr;
}

bool VJoy::SetAxis(const std::string& axis, float value) {
    if (!acquired_) return false;
    const auto usage = kAxisUsage.find(axis);
    if (usage == kAxisUsage.end() || !api_->GetVJDAxisExist(device_, usage->second)) return false;
    LONG min = 0, max = 0x8000;
    api_->GetVJDAxisMin(device_, usage->second, &min);
    api_->GetVJDAxisMax(device_, usage->second, &max);
    value = std::clamp(value, 0.0f, 1.0f);
    return api_->SetAxis(min + static_cast<LONG>(value * static_cast<float>(max - min)), device_, usage->second);
}

bool VJoy::SetButton(unsigned button, bool pressed) {
    if (!acquired_) return false;
    return api_->SetBtn(pressed, device_, static_cast<UCHAR>(button));
}
