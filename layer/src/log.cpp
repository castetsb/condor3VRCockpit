#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {

std::mutex gLogMutex;
FILE* gLogFile = nullptr;

FILE* OpenLogFile() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        return nullptr;
    }
    std::filesystem::path dir = std::filesystem::path(localAppData) / L"condor3VRCockpit";
    CoTaskMemFree(localAppData);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    FILE* file = nullptr;
    _wfopen_s(&file, (dir / L"log.txt").c_str(), L"w");
    return file;
}

} // namespace

void Log(const char* fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);

    std::lock_guard lock(gLogMutex);
    OutputDebugStringA(line);
    if (!gLogFile) {
        gLogFile = OpenLogFile();
    }
    if (gLogFile) {
        fputs(line, gLogFile);
        fflush(gLogFile);
    }
}
