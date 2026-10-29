#include "ovr_hands.h"

#include <windows.h>

#include <cstddef>
#include <cstring>

namespace {

using ovrResult = int32_t;
using ovrSession = void*;

struct ovrInitParams {
    uint32_t Flags;
    uint32_t RequestedMinorVersion;
    void* LogCallback;
    uintptr_t UserData;
    uint32_t ConnectionTimeoutMS;
};
constexpr uint32_t ovrInit_Invisible = 0x10;

struct ovrErrorInfo {
    ovrResult Result;
    char ErrorString[512];
};

struct HandInputState {
    uint32_t unknown0;
    float pinch[4];
    float orientation[4];
    float position[3];
    uint32_t flags;
};
static_assert(sizeof(HandInputState) == 52);

// Public Oculus SDK types for the Touch controllers (only the parts read here).
struct ovrPosef {
    float orientation[4];
    float position[3];
};
struct alignas(8) ovrPoseStatef {
    ovrPosef pose;
    float velocities[12];
    char pad0[4];
    double timeInSeconds;
};
struct alignas(8) ovrTrackingState {
    ovrPoseStatef headPose;
    uint32_t statusFlags;
    char pad0[4];
    ovrPoseStatef handPoses[2];
    uint32_t handStatusFlags[2];
    ovrPosef calibratedOrigin;
    char slack[64];  // safety margin: the runtime writes the struct through a hidden pointer
};
static_assert(offsetof(ovrTrackingState, handPoses) == 96);
struct ovrInputStatePrefix {
    double timeInSeconds;
    uint32_t buttons;
    uint32_t touches;
    float indexTrigger[2];
    float handTrigger[2];  // grip
};
union ovrInputStateBuffer {
    ovrInputStatePrefix state;
    char raw[1024];  // the real struct is larger
};
constexpr uint32_t ovrControllerType_Touch = 0x3;
constexpr uint32_t ovrStatus_OrientationTracked = 0x1;
constexpr uint32_t ovrStatus_PositionTracked = 0x2;

} // namespace

struct OvrHands::Api {
    HMODULE shim = nullptr;
    ovrResult(__cdecl* Initialize)(const ovrInitParams*) = nullptr;
    void(__cdecl* Shutdown)() = nullptr;
    void(__cdecl* GetLastErrorInfo)(ovrErrorInfo*) = nullptr;
    ovrResult(__cdecl* Create)(ovrSession*, void* luid) = nullptr;
    void(__cdecl* Destroy)(ovrSession) = nullptr;
    ovrResult(__cdecl* GetCurrentHandInputState)(ovrSession, int, HandInputState*) = nullptr;
    double(__cdecl* GetTimeInSeconds)() = nullptr;
    ovrTrackingState(__cdecl* GetTrackingState)(ovrSession, double, char) = nullptr;
    uint32_t(__cdecl* GetConnectedControllerTypes)(ovrSession) = nullptr;
    ovrResult(__cdecl* GetInputState)(ovrSession, uint32_t, ovrInputStateBuffer*) = nullptr;
};

namespace {

template <typename T>
bool Load(HMODULE module, const char* name, T& function, std::string& error) {
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!function) error = std::string("missing export ") + name;
    return function != nullptr;
}

std::string LastError(void(__cdecl* getLastErrorInfo)(ovrErrorInfo*), const char* what, ovrResult result) {
    ovrErrorInfo info{};
    getLastErrorInfo(&info);
    return std::string(what) + " failed (" + std::to_string(result) + "): " + info.ErrorString;
}

} // namespace

OvrHands::~OvrHands() {
    Close();
    delete api_;
}

bool OvrHands::Open(const std::wstring& libovrPath, std::string& error) {
    Close();
    if (!api_) {
        api_ = new Api;
        api_->shim = LoadLibraryW(libovrPath.c_str());
        if (!api_->shim) {
            error = "cannot load LibOVR.dll (error " + std::to_string(GetLastError()) + ")";
            delete api_;
            api_ = nullptr;
            return false;
        }
        if (!Load(api_->shim, "ovr_Initialize", api_->Initialize, error) ||
            !Load(api_->shim, "ovr_Shutdown", api_->Shutdown, error) ||
            !Load(api_->shim, "ovr_GetLastErrorInfo", api_->GetLastErrorInfo, error) ||
            !Load(api_->shim, "ovr_Create", api_->Create, error) ||
            !Load(api_->shim, "ovr_Destroy", api_->Destroy, error) ||
            !Load(api_->shim, "ovr_GetTimeInSeconds", api_->GetTimeInSeconds, error) ||
            !Load(api_->shim, "ovr_GetTrackingState", api_->GetTrackingState, error) ||
            !Load(api_->shim, "ovr_GetConnectedControllerTypes", api_->GetConnectedControllerTypes, error) ||
            !Load(api_->shim, "ovr_GetInputState", api_->GetInputState, error)) {
            return false;
        }
    }

    ovrInitParams params{};
    params.Flags = ovrInit_Invisible;
    ovrResult result = api_->Initialize(&params);
    if (result < 0) {
        error = LastError(api_->GetLastErrorInfo, "ovr_Initialize", result);
        return false;
    }
    initialized_ = true;

    // The shim has loaded the Meta runtime now; the hand export lives there.
    HMODULE runtime = GetModuleHandleW(L"LibOVRRT32_1.dll");
    if (!runtime || !Load(runtime, "ovr_GetCurrentHandInputState", api_->GetCurrentHandInputState, error)) {
        if (!runtime) error = "Meta runtime LibOVRRT32_1.dll not loaded";
        Close();
        return false;
    }

    char luid[8] = {};
    result = api_->Create(&session_, luid);
    if (result < 0) {
        error = LastError(api_->GetLastErrorInfo, "ovr_Create", result);
        session_ = nullptr;
        Close();
        return false;
    }
    return true;
}

void OvrHands::Close() {
    if (!api_) return;
    if (session_) {
        api_->Destroy(session_);
        session_ = nullptr;
    }
    if (initialized_) {
        api_->Shutdown();
        initialized_ = false;
    }
}

bool OvrHands::Read(HandState (&state)[2]) {
    if (!session_) return false;
    if (api_->GetConnectedControllerTypes(session_) & ovrControllerType_Touch) return ReadControllers(state);
    return ReadHand(Hand::Left, state[0]) && ReadHand(Hand::Right, state[1]);
}

bool OvrHands::ReadControllers(HandState (&state)[2]) {
    const ovrTrackingState tracking = api_->GetTrackingState(session_, api_->GetTimeInSeconds(), 0);
    ovrInputStateBuffer input{};
    const bool haveInput = api_->GetInputState(session_, ovrControllerType_Touch, &input) >= 0;
    for (int h = 0; h < 2; ++h) {
        HandState& s = state[h];
        s = {};
        s.controller = true;
        const uint32_t status = tracking.handStatusFlags[h];
        s.tracked = (status & ovrStatus_OrientationTracked) && (status & ovrStatus_PositionTracked);
        if (!s.tracked) continue;
        const ovrPosef& pose = tracking.handPoses[h].pose;
        s.position = {pose.position[0], pose.position[1], pose.position[2]};
        s.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
        s.pinch[0] = haveInput ? input.state.handTrigger[h] : 0.0f;  // grip squeeze acts as the pinch
        // Oculus SDK buttons: A 0x1, B 0x2 (right); X 0x100, Y 0x200 (left).
        if (haveInput) s.buttons = (h == 1 ? input.state.buttons : input.state.buttons >> 8) & 0x3;
    }
    return true;
}

bool OvrHands::ReadHand(Hand hand, HandState& state) {
    state = {};
    if (!session_) return false;
    HandInputState raw{};
    const ovrResult result = api_->GetCurrentHandInputState(session_, static_cast<int>(hand), &raw);
    if (result < 0) return false;
    constexpr uint32_t kInputStateValid = 0x02;
    constexpr uint32_t kSystemGesture = 0x40;
    state.tracked = result == 0 && (raw.flags & (kInputStateValid | kSystemGesture)) != 0;
    state.palmFacing = state.tracked && (raw.flags & kSystemGesture) != 0;
    if (state.tracked) {
        state.position = {raw.position[0], raw.position[1], raw.position[2]};
        state.orientation = {raw.orientation[0], raw.orientation[1], raw.orientation[2], raw.orientation[3]};
        std::memcpy(state.pinch, raw.pinch, sizeof(state.pinch));
        state.flags = raw.flags;
    }
    return true;
}
