// ovrprobe: checks what the Oculus PC SDK reports about hands while Condor runs.
//
// Opens an *invisible* LibOVR session (allowed next to a running VR app) through
// Condor's own LibOVR.dll and prints head/hand tracking and controller input twice
// per second. Question to answer: with controllers put down and hand tracking on,
// does the Quest report hand poses (and a pinch) to Oculus-SDK apps like Condor?
//
// Only the few LibOVR types we need are declared here, from the public ABI.

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

using ovrResult = int32_t;
using ovrBool = char;
using ovrSession = struct ovrHmdStruct*;

struct ovrInitParams {
    uint32_t Flags;
    uint32_t RequestedMinorVersion;
    void* LogCallback;
    uintptr_t UserData;
    uint32_t ConnectionTimeoutMS;
};
constexpr uint32_t ovrInit_Invisible = 0x10;

struct ovrGraphicsLuid {
    char Reserved[8];
};

struct ovrErrorInfo {
    ovrResult Result;
    char ErrorString[512];
};

struct ovrVector3f {
    float x, y, z;
};
struct ovrQuatf {
    float x, y, z, w;
};
struct ovrPosef {
    ovrQuatf Orientation;
    ovrVector3f Position;
};

struct alignas(8) ovrPoseStatef {
    ovrPosef ThePose;
    ovrVector3f AngularVelocity;
    ovrVector3f LinearVelocity;
    ovrVector3f AngularAcceleration;
    ovrVector3f LinearAcceleration;
    char pad0[4];
    double TimeInSeconds;
};

struct alignas(8) ovrTrackingState {
    ovrPoseStatef HeadPose;
    uint32_t StatusFlags;
    char pad0[4];
    ovrPoseStatef HandPoses[2];
    uint32_t HandStatusFlags[2];
    ovrPosef CalibratedOrigin;
    char slack[64]; // safety margin: the runtime writes the struct through a hidden pointer
};
static_assert(offsetof(ovrTrackingState, HandPoses) == 96);
static_assert(offsetof(ovrTrackingState, HandStatusFlags) == 272);

constexpr uint32_t ovrStatus_OrientationTracked = 0x1;
constexpr uint32_t ovrStatus_PositionTracked = 0x2;

// ovrInputState: only the leading fields are read; the buffer is oversized on purpose.
struct ovrInputStatePrefix {
    double TimeInSeconds;
    uint32_t Buttons;
    uint32_t Touches;
    float IndexTrigger[2];
    float HandTrigger[2];
    float Thumbstick[2][2];
    uint32_t ControllerType;
};
union ovrInputStateBuffer {
    ovrInputStatePrefix state;
    char raw[1024];
};

constexpr uint32_t ovrControllerType_Active = 0xffffffff;

using PFN_Initialize = ovrResult(__cdecl*)(const ovrInitParams*);
using PFN_Shutdown = void(__cdecl*)();
using PFN_GetLastErrorInfo = void(__cdecl*)(ovrErrorInfo*);
using PFN_Create = ovrResult(__cdecl*)(ovrSession*, ovrGraphicsLuid*);
using PFN_Destroy = void(__cdecl*)(ovrSession);
using PFN_GetTimeInSeconds = double(__cdecl*)();
using PFN_GetTrackingState = ovrTrackingState(__cdecl*)(ovrSession, double, ovrBool);
using PFN_GetConnectedControllerTypes = uint32_t(__cdecl*)(ovrSession);
using PFN_GetInputState = ovrResult(__cdecl*)(ovrSession, uint32_t, ovrInputStateBuffer*);

FILE* gLog = nullptr;

void Print(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    fputs(line, stdout);
    if (gLog) {
        fputs(line, gLog);
        fflush(gLog);
    }
}

const char* Tracked(uint32_t flags) {
    if ((flags & ovrStatus_PositionTracked) && (flags & ovrStatus_OrientationTracked)) return "pos+rot";
    if (flags & ovrStatus_OrientationTracked) return "rot only";
    if (flags & ovrStatus_PositionTracked) return "pos only";
    return "NOT tracked";
}

template <typename T>
bool Load(HMODULE module, const char* name, T& function) {
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!function) Print("Missing export %s\n", name);
    return function != nullptr;
}

} // namespace

int main(int argc, char** argv) {
    const char* dllPath = argc > 1 ? argv[1] : "C:\\Condor3\\LibOVR.dll";
    const int seconds = argc > 2 ? atoi(argv[2]) : 120;
    fopen_s(&gLog, "ovrprobe.log", "w");

    HMODULE ovr = LoadLibraryA(dllPath);
    if (!ovr) {
        Print("Cannot load %s (error %lu)\n", dllPath, GetLastError());
        return 1;
    }
    PFN_Initialize Initialize;
    PFN_Shutdown Shutdown;
    PFN_GetLastErrorInfo GetLastErrorInfo;
    PFN_Create Create;
    PFN_Destroy Destroy;
    PFN_GetTimeInSeconds GetTimeInSeconds;
    PFN_GetTrackingState GetTrackingState;
    PFN_GetConnectedControllerTypes GetConnectedControllerTypes;
    PFN_GetInputState GetInputState;
    if (!Load(ovr, "ovr_Initialize", Initialize) || !Load(ovr, "ovr_Shutdown", Shutdown) ||
        !Load(ovr, "ovr_GetLastErrorInfo", GetLastErrorInfo) || !Load(ovr, "ovr_Create", Create) ||
        !Load(ovr, "ovr_Destroy", Destroy) || !Load(ovr, "ovr_GetTimeInSeconds", GetTimeInSeconds) ||
        !Load(ovr, "ovr_GetTrackingState", GetTrackingState) ||
        !Load(ovr, "ovr_GetConnectedControllerTypes", GetConnectedControllerTypes) ||
        !Load(ovr, "ovr_GetInputState", GetInputState)) {
        return 1;
    }

    ovrInitParams params{};
    params.Flags = ovrInit_Invisible;
    ovrResult result = Initialize(&params);
    if (result < 0) {
        ovrErrorInfo error{};
        GetLastErrorInfo(&error);
        Print("ovr_Initialize failed: %d %s\n", result, error.ErrorString);
        return 1;
    }
    ovrSession session = nullptr;
    ovrGraphicsLuid luid{};
    result = Create(&session, &luid);
    if (result < 0) {
        ovrErrorInfo error{};
        GetLastErrorInfo(&error);
        Print("ovr_Create failed: %d %s\n", result, error.ErrorString);
        Shutdown();
        return 1;
    }
    Print("Invisible LibOVR session open. Logging for %d s (also to ovrprobe.log).\n", seconds);

    const char* sides[2] = {"L", "R"};
    for (int tick = 0; tick < seconds * 2; ++tick) {
        const double now = GetTimeInSeconds();
        const ovrTrackingState tracking = GetTrackingState(session, now, 0);
        const uint32_t controllers = GetConnectedControllerTypes(session);

        Print("t=%7.1f head %-11s controllers=0x%03x\n", now, Tracked(tracking.StatusFlags), controllers);
        for (int h = 0; h < 2; ++h) {
            const ovrVector3f& p = tracking.HandPoses[h].ThePose.Position;
            Print("   hand %s %-11s pos (%+.3f %+.3f %+.3f)\n", sides[h], Tracked(tracking.HandStatusFlags[h]),
                  p.x, p.y, p.z);
        }
        ovrInputStateBuffer input{};
        if (GetInputState(session, ovrControllerType_Active, &input) >= 0) {
            const ovrInputStatePrefix& s = input.state;
            Print("   input type=0x%x buttons=0x%08x touches=0x%08x index=%.2f/%.2f grip=%.2f/%.2f\n",
                  s.ControllerType, s.Buttons, s.Touches, s.IndexTrigger[0], s.IndexTrigger[1], s.HandTrigger[0],
                  s.HandTrigger[1]);
        }
        Sleep(500);
    }

    Destroy(session);
    Shutdown();
    return 0;
}
