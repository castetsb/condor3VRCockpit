#pragma once

// Writes to %LOCALAPPDATA%\condor3VRCockpit\log.txt (truncated at each start) and OutputDebugString.
void Log(const char* fmt, ...);
