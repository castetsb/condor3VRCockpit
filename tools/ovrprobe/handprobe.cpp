// handprobe: tries the runtime's undocumented hand-tracking exports over Oculus Link.
//
// LibOVRRT32_1.dll (Meta runtime) exports ovr_GetHandPose, ovr_GetCurrentHandInputState and
// ovr_GetHandSkeleton, which the public Oculus SDK never declared. Parameter lists below come
// from inspecting the export stubs (cdecl):
//   ovr_GetHandPose(session, int hand, double absTime, void* out)
//   ovr_GetCurrentHandInputState(session, int hand, void* out)
//   ovr_GetHandSkeleton(session, int hand, void* out)
// The output layouts are unknown, so each call writes into a buffer pre-filled with 0xCD and
// we log how many bytes changed plus the content as floats/ints.

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using ovrResult = int32_t;
using ovrSession = struct ovrHmdStruct*;

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

using PFN_Initialize = ovrResult(__cdecl*)(const ovrInitParams*);
using PFN_Shutdown = void(__cdecl*)();
using PFN_GetLastErrorInfo = void(__cdecl*)(ovrErrorInfo*);
using PFN_Create = ovrResult(__cdecl*)(ovrSession*, void* luid);
using PFN_Destroy = void(__cdecl*)(ovrSession);
using PFN_GetTimeInSeconds = double(__cdecl*)();
using PFN_GetHandPose = ovrResult(__cdecl*)(ovrSession, int, double, void*);
using PFN_GetHandOut = ovrResult(__cdecl*)(ovrSession, int, void*);

constexpr size_t kBufferSize = 8192;
constexpr unsigned char kFill = 0xCD;
// Pose (0x1D8 bytes) and skeleton (0x53C bytes) outputs must start with this version tag.
constexpr uint32_t kHandStructVersion = 0xDF000001;

void PrepareVersioned(unsigned char* buffer) {
    memset(buffer, kFill, kBufferSize);
    memcpy(buffer, &kHandStructVersion, sizeof(kHandStructVersion));
}

FILE* gLog = nullptr;
PFN_GetLastErrorInfo gGetLastErrorInfo = nullptr;

void Print(const char* fmt, ...) {
    char line[4096];
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

size_t WrittenBytes(const unsigned char* buffer) {
    size_t last = kBufferSize;
    while (last > 0 && buffer[last - 1] == kFill) --last;
    return last;
}

// Dumps the written part as rows of 4 words: each word as float and as hex.
void Dump(const unsigned char* buffer, size_t bytes, size_t maxBytes) {
    const size_t n = (bytes < maxBytes ? bytes : maxBytes) / 4;
    for (size_t i = 0; i < n; i += 4) {
        Print("      %4zu:", i * 4);
        for (size_t j = i; j < i + 4 && j < n; ++j) {
            float f;
            uint32_t u;
            memcpy(&f, buffer + j * 4, 4);
            memcpy(&u, buffer + j * 4, 4);
            Print("  %+11.4f[%08x]", f, u);
        }
        Print("\n");
    }
}

void Report(const char* what, int hand, ovrResult result, const unsigned char* buffer, size_t maxDump) {
    const size_t written = WrittenBytes(buffer);
    if (result < 0) {
        ovrErrorInfo error{};
        gGetLastErrorInfo(&error);
        Print("   %s(%s) -> %d (%s), %zu bytes written\n", what, hand ? "R" : "L", result, error.ErrorString, written);
    } else {
        Print("   %s(%s) -> %d, %zu bytes written\n", what, hand ? "R" : "L", result, written);
    }
    Dump(buffer, written, maxDump);
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
    const int seconds = argc > 2 ? atoi(argv[2]) : 60;
    fopen_s(&gLog, "handprobe.log", "w");

    // Condor's shim loads the runtime (LibOVRRT32_1.dll); session handles are the runtime's own.
    HMODULE shim = LoadLibraryA(dllPath);
    if (!shim) {
        Print("Cannot load %s (error %lu)\n", dllPath, GetLastError());
        return 1;
    }
    PFN_Initialize Initialize;
    PFN_Shutdown Shutdown;
    PFN_Create Create;
    PFN_Destroy Destroy;
    PFN_GetTimeInSeconds GetTimeInSeconds;
    if (!Load(shim, "ovr_Initialize", Initialize) || !Load(shim, "ovr_Shutdown", Shutdown) ||
        !Load(shim, "ovr_GetLastErrorInfo", gGetLastErrorInfo) || !Load(shim, "ovr_Create", Create) ||
        !Load(shim, "ovr_Destroy", Destroy) || !Load(shim, "ovr_GetTimeInSeconds", GetTimeInSeconds)) {
        return 1;
    }

    ovrInitParams params{};
    params.Flags = ovrInit_Invisible;
    ovrResult result = Initialize(&params);
    if (result < 0) {
        ovrErrorInfo error{};
        gGetLastErrorInfo(&error);
        Print("ovr_Initialize failed: %d %s\n", result, error.ErrorString);
        return 1;
    }
    ovrSession session = nullptr;
    char luid[8] = {};
    result = Create(&session, luid);
    if (result < 0) {
        ovrErrorInfo error{};
        gGetLastErrorInfo(&error);
        Print("ovr_Create failed: %d %s\n", result, error.ErrorString);
        Shutdown();
        return 1;
    }

    HMODULE runtime = GetModuleHandleA("LibOVRRT32_1.dll");
    if (!runtime) {
        Print("Runtime LibOVRRT32_1.dll not loaded in this process\n");
        return 1;
    }
    PFN_GetHandPose GetHandPose;
    PFN_GetHandOut GetCurrentHandInputState;
    PFN_GetHandOut GetHandSkeleton;
    if (!Load(runtime, "ovr_GetHandPose", GetHandPose) ||
        !Load(runtime, "ovr_GetCurrentHandInputState", GetCurrentHandInputState) ||
        !Load(runtime, "ovr_GetHandSkeleton", GetHandSkeleton)) {
        return 1;
    }
    Print("Invisible session open. Logging hands for %d s (also to handprobe.log).\n", seconds);

    static unsigned char buffer[kBufferSize];
    for (int hand = 0; hand < 2; ++hand) {
        PrepareVersioned(buffer);
        Report("ovr_GetHandSkeleton", hand, GetHandSkeleton(session, hand, buffer), buffer, 2048);
    }

    for (int tick = 0; tick < seconds; ++tick) {
        const double now = GetTimeInSeconds();
        Print("t=%7.1f\n", now);
        for (int hand = 0; hand < 2; ++hand) {
            PrepareVersioned(buffer);
            Report("ovr_GetHandPose", hand, GetHandPose(session, hand, now, buffer), buffer, 512);
            memset(buffer, kFill, sizeof(buffer));
            Report("ovr_GetCurrentHandInputState", hand, GetCurrentHandInputState(session, hand, buffer), buffer, 256);
        }
        Sleep(1000);
    }

    Destroy(session);
    Shutdown();
    return 0;
}
