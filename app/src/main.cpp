// condorhands: Quest hand tracking -> vJoy, for Condor 3's airbrake, flaps, gear and release.
//
//   condorhands monitor                     print live hand data (tuning, diagnostics)
//   condorhands calibrate <glider> [ctrl…]  voice-guided: pinch at each handle position
//   condorhands run <glider>                fly
//
// Profiles live in %LOCALAPPDATA%\condor3VRCockpit\profiles\<glider>.json and start as a copy
// of the template shipped next to the exe (profiles\<glider>.json).

#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <conio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "audio.h"
#include "cockpit.h"
#include "ovr_hands.h"
#include "profile.h"
#include "vjoy.h"

namespace fs = std::filesystem;

namespace {

const wchar_t* kDefaultLibOvr = L"C:\\Condor3\\LibOVR.dll";
const wchar_t* kDefaultVJoy = L"C:\\Program Files\\vJoy\\x86\\vJoyInterface.dll";
constexpr DWORD kFramePeriodMs = 11; // ~90 Hz

std::atomic<bool> gQuit = false;

BOOL WINAPI OnConsoleCtrl(DWORD) {
    gQuit = true;
    return TRUE;
}

double Seconds() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency.QuadPart;
}

fs::path ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path UserProfilePath(const std::string& glider) {
    PWSTR localAppData = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    fs::path dir = fs::path(localAppData) / L"condor3VRCockpit" / L"profiles";
    CoTaskMemFree(localAppData);
    fs::create_directories(dir);
    return dir / (glider + ".json");
}

std::optional<Profile> LoadProfile(const std::string& glider, fs::path& path) {
    path = UserProfilePath(glider);
    if (!fs::exists(path)) {
        const fs::path templ = ExeDir() / L"profiles" / (glider + ".json");
        std::error_code ec;
        if (!fs::copy_file(templ, path, ec)) {
            printf("No profile for '%s'. Available:", glider.c_str());
            for (const auto& entry : fs::directory_iterator(ExeDir() / L"profiles", ec)) {
                if (entry.path().extension() == L".json") printf(" %ls", entry.path().stem().c_str());
            }
            printf("\n");
            return std::nullopt;
        }
        printf("Created %ls from the template\n", path.c_str());
    }
    std::string error;
    auto profile = Profile::Load(path.wstring(), error);
    if (!profile) printf("Cannot read %ls: %s\n", path.c_str(), error.c_str());
    return profile;
}

// No glider on the command line: list the profiles (templates and the user's own) and ask for
// one by number. Returns an empty string if none is chosen.
std::string ChooseGlider() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const fs::path& dir : {ExeDir() / L"profiles", UserProfilePath("x").parent_path()}) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != L".json") continue;
            const std::string name = entry.path().stem().string();
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        printf("No glider profiles found.\n");
        return {};
    }
    printf("Gliders:\n");
    for (size_t i = 0; i < names.size(); ++i) {
        // Show the user's copy if there is one: it says whether it is calibrated.
        fs::path path = UserProfilePath(names[i]);
        if (!fs::exists(path)) path = ExeDir() / L"profiles" / (names[i] + ".json");
        std::string error;
        const auto profile = Profile::Load(path.wstring(), error);
        bool calibrated = false;
        if (profile) {
            for (const ControlConfig& c : profile->controls) calibrated = calibrated || c.from;
        }
        printf("  %zu  %-12s %s%s\n", i + 1, names[i].c_str(), profile ? profile->glider.c_str() : "",
               calibrated ? "" : "  (not calibrated)");
    }
    printf("Number: ");
    char line[32] = {};
    if (!fgets(line, sizeof(line), stdin)) return {};
    const char* digits = line;
    while (*digits && !isdigit(static_cast<unsigned char>(*digits))) ++digits;  // e.g. a piped BOM
    const int index = atoi(digits) - 1;
    if (index < 0 || index >= static_cast<int>(names.size())) {
        printf("No glider chosen.\n");
        return {};
    }
    printf("%s\n", names[index].c_str());
    return names[index];
}

// Condor only reads joysticks while its window is in front, so give it the focus. This works
// while our console is the foreground window, i.e. right after the user started us.
void FocusCondor() {
    struct Search {
        HWND window = nullptr;
    } search;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return TRUE;
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            const bool isCondor = QueryFullProcessImageNameW(process, 0, path, &size) &&
                                  _wcsicmp(fs::path(path).filename().c_str(), L"Condor.exe") == 0;
            CloseHandle(process);
            if (!isCondor) return TRUE;
            reinterpret_cast<Search*>(param)->window = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.window) {
        printf("Condor's window not found: click into Condor so it reads the vJoy controls.\n");
    } else if (!SetForegroundWindow(search.window)) {
        printf("Could not bring Condor to the front: click into Condor so it reads the vJoy controls.\n");
    } else {
        printf("Condor brought to the front.\n");
    }
}

// Opens the hand session, retrying until it works or the user quits.
bool OpenHands(OvrHands& hands, const std::wstring& libovr) {
    std::string lastError;
    while (!gQuit) {
        std::string error;
        if (hands.Open(libovr, error)) {
            printf("Hand tracking connected.\n");
            return true;
        }
        if (error != lastError) {
            printf("Waiting for the headset: %s\n", error.c_str());
            lastError = error;
        }
        Sleep(2000);
    }
    return false;
}

// Reads both hands; reopens the session if the runtime went away.
bool ReadHands(OvrHands& hands, const std::wstring& libovr, HandState (&state)[2]) {
    if (hands.Read(state)) return true;
    printf("Hand tracking lost, reconnecting...\n");
    hands.Close();
    state[0] = state[1] = {};
    return OpenHands(hands, libovr);
}

int Monitor(const std::wstring& libovr) {
    OvrHands hands;
    if (!OpenHands(hands, libovr)) return 1;
    while (!gQuit) {
        HandState state[2];
        if (!ReadHands(hands, libovr, state)) break;
        printf("%s ", state[0].controller ? "[controllers]" : "[hands]      ");
        for (int h = 0; h < 2; ++h) {
            const HandState& s = state[h];
            if (s.tracked) {
                printf("%s (%+.3f %+.3f %+.3f) pinch %.2f %.2f %.2f %.2f flags %04x   ", h ? "R" : "L", s.position.x,
                       s.position.y, s.position.z, s.pinch[0], s.pinch[1], s.pinch[2], s.pinch[3], s.flags);
            } else {
                printf("%s ---------------------------------------------------------------   ", h ? "R" : "L");
            }
        }
        printf("\n");
        Sleep(100);
    }
    return 0;
}

// Calibration hotkeys that nudge a lever's vJoy value; the hotkey id is the index.
struct AdjustKey {
    UINT key;
    float delta;
};
constexpr AdjustKey kAdjustKeys[] = {{VK_UP, 0.01f}, {VK_DOWN, -0.01f}, {VK_PRIOR, 0.05f}, {VK_NEXT, -0.05f}};

// Registers kAdjustKeys as global hotkeys for its lifetime: they reach us whichever window has
// the focus (Condor), and Condor doesn't see them.
struct AdjustHotkeys {
    AdjustHotkeys() {
        for (size_t i = 0; i < std::size(kAdjustKeys); ++i) {
            if (!RegisterHotKey(nullptr, static_cast<int>(i), 0, kAdjustKeys[i].key)) {
                printf("Warning: arrow/page key %zu is taken by another program, it won't adjust levers.\n", i);
            }
        }
    }
    ~AdjustHotkeys() {
        for (size_t i = 0; i < std::size(kAdjustKeys); ++i) UnregisterHotKey(nullptr, static_cast<int>(i));
    }
};

struct CalibrationStep {
    ControlConfig* control;
    bool to;                  // false: `from` point, true: `to` point
    bool pulseBefore = false; // toggles: press the button first so Condor's lever changes side
    bool pulseAfter = false;  // toggles: press it again to put the lever back
    int detent = -1;          // levers with notches: one step per notch
    bool right = false;       // stick: the `right` point (full right) instead of from / to
};

int Calibrate(const std::string& glider, const std::vector<std::string>& only, const std::wstring& libovr,
              const std::wstring& vjoyDll) {
    fs::path path;
    auto profile = LoadProfile(glider, path);
    if (!profile) return 1;

    // Condor's levers are moved through vJoy to each position before it is asked for, so the
    // keyboard (and taking the headset off) isn't needed. A toggle starts on its current side.
    std::vector<CalibrationStep> steps;
    for (ControlConfig& c : profile->controls) {
        if (!only.empty() && std::find(only.begin(), only.end(), c.name) == only.end()) continue;
        if (c.type == ControlConfig::Type::Button) {
            steps.push_back({&c, false});
        } else if (c.type == ControlConfig::Type::Toggle) {
            const bool startTo = c.initial >= 0.5f;
            steps.push_back({&c, startTo});
            steps.push_back({&c, !startTo, true, true});
        } else if (c.type == ControlConfig::Type::Stick) {
            steps.push_back({&c, false});
            steps.push_back({&c, true});
            steps.push_back({&c, false, false, false, -1, true});
        } else if (c.detents.size() > 1) {
            c.detentPoints.assign(c.detents.size(), Vec3{});
            for (int i = 0; i < static_cast<int>(c.detents.size()); ++i) {
                steps.push_back({&c, false, false, false, i});
            }
        } else {
            steps.push_back({&c, false});
            steps.push_back({&c, true});
        }
    }
    if (steps.empty()) {
        printf("Nothing to calibrate.\n");
        return 1;
    }

    // Every frame goes to calibration-log.csv, to diagnose pinch detection.
    const fs::path logPath = path.parent_path().parent_path() / L"calibration-log.csv";
    FILE* log = _wfopen(logPath.c_str(), L"w");
    if (log) {
        fprintf(log, "t,step,phase,armed,candidate,pinching,"
                     "Ltracked,Lx,Ly,Lz,Lindex,Lmiddle,Lring,Lpinky,Lflags,"
                     "Rtracked,Rx,Ry,Rz,Rindex,Rmiddle,Rring,Rpinky,Rflags\n");
    }
    auto logFrame = [&](const char* step, const char* phase, bool armed, int candidate, int pinching,
                        const HandState (&state)[2]) {
        if (!log) return;
        fprintf(log, "%.3f,%s,%s,%d,%d,%d", Seconds(), step, phase, armed, candidate, pinching);
        for (const HandState& s : state) {
            fprintf(log, ",%d,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%04x", s.tracked, s.position.x, s.position.y,
                    s.position.z, s.pinch[0], s.pinch[1], s.pinch[2], s.pinch[3], s.flags);
        }
        fprintf(log, "\n");
    };
    struct LogCloser {
        FILE* f;
        ~LogCloser() {
            if (f) fclose(f);
        }
    } logCloser{log};
    printf("Logging hand data to %ls\n", logPath.c_str());

    // Calibration moves Condor's levers itself, so it needs vJoy (often busy because a `bind`
    // or `run` window is still open).
    VJoy vjoy;
    std::string vjoyError;
    if (!vjoy.Open(vjoyDll, profile->vjoyDevice, vjoyError)) {
        printf("vJoy: %s\nClose any other condorhands window (bind: press q, run: Ctrl+C) and retry.\n",
               vjoyError.c_str());
        return 1;
    }
    // Levers glide to a new value (like `bind`, which Condor follows); an instant jump from one
    // end to the other was not always picked up.
    std::map<std::string, float> axisValue;
    auto glide = [&](const std::string& axis, float target) {
        const auto known = axisValue.find(axis);
        const float start = known != axisValue.end() ? known->second : target;
        bool ok = true;
        for (int step = 1; step <= 50; ++step) {  // 1 s
            ok = vjoy.SetAxis(axis, start + (target - start) * step / 50.0f) && ok;
            Sleep(20);
        }
        axisValue[axis] = target;
        return ok;
    };
    auto pulse = [&](const ControlConfig& c) {
        vjoy.SetButton(c.vjoyButton, true);
        Sleep(200);
        vjoy.SetButton(c.vjoyButton, false);
    };
    // Step-button levers (flaps like Condor's keys): notch Condor is at, -1 = unknown.
    std::map<const ControlConfig*, int> stepNotch;
    auto stepTo = [&](const ControlConfig& c, int target) {
        int& current = stepNotch.try_emplace(&c, -1).first->second;
        int presses = current < 0 ? static_cast<int>(c.detents.size()) : target - current;
        if (current < 0) {
            // Unknown: push against the first notch, then count from there.
            for (int i = 0; i < presses; ++i) {
                vjoy.SetButton(c.vjoyStepButtons[0], true);
                Sleep(80);
                vjoy.SetButton(c.vjoyStepButtons[0], false);
                Sleep(80);
            }
            current = 0;
            presses = target;
        }
        const unsigned button = c.vjoyStepButtons[presses < 0 ? 0 : 1];
        for (int i = 0; i < std::abs(presses); ++i) {
            vjoy.SetButton(button, true);
            Sleep(80);
            vjoy.SetButton(button, false);
            Sleep(80);
        }
        current = target;
    };

    FocusCondor();
    Sleep(300);
    // Levers at their start-up value, as `run` will set them.
    for (const ControlConfig& c : profile->controls) {
        if (c.UsesStepButtons()) {
            stepTo(c, static_cast<int>(c.initial));
        } else if (c.type == ControlConfig::Type::Stick) {
            if (!c.vjoyAxis.empty()) glide(c.vjoyAxis, c.AxisValue(0.5f));
            if (!c.vjoyRollAxis.empty()) glide(c.vjoyRollAxis, c.RollAxisValue(0.5f));
        } else if (c.type == ControlConfig::Type::Lever && !c.vjoyAxis.empty()) {
            glide(c.vjoyAxis, c.AxisValue(c.InitialPosition()));
        }
    }

    Audio audio;
    OvrHands hands;
    if (!OpenHands(hands, libovr)) return 1;
    const AdjustHotkeys adjustHotkeys;

    // Hands are invisible in Condor, but the Meta system icon over a palm turned towards the face
    // is drawn on top of it: that hand is the pointer, and a pinch of the *other* hand records it
    // (pinching the pointer hand itself would open the Meta menu).
    audio.Say("Calibration. Turn one palm towards you and put its Meta icon on the handle. "
              "Then pinch with the other hand.");
    printf("Calibration: turn one palm towards you, put its Meta icon on the handle,\n"
           "then pinch with the other hand (palm down). The icon hand's position is recorded.\n");
    Sleep(7000);

    for (const CalibrationStep& step : steps) {
        const ControlConfig& c = *step.control;
        if (c.UsesStepButtons() && step.detent >= 0) {
            stepTo(c, step.detent);
            printf("  [vJoy step buttons: notch %d]\n", step.detent);
        }
        // Axis levers and the stick: the position sent on the axis being calibrated, which the
        // keyboard can correct (see below). The stick's centre is fixed; its other axis stays centred.
        const bool stick = c.type == ControlConfig::Type::Stick;
        const bool rollStep = stick && step.right;
        const std::string& adjustAxis = rollStep ? c.vjoyRollAxis : c.vjoyAxis;
        auto valueOf = [&](float p) { return rollStep ? c.RollAxisValue(p) : c.AxisValue(p); };
        const bool adjustable = !adjustAxis.empty() && ((stick && (step.to || step.right)) ||
                                                        (!c.UsesStepButtons() && c.type == ControlConfig::Type::Lever));
        float position = step.detent >= 0 ? c.DetentPosition(step.detent) : (step.to || step.right) ? 1.0f : 0.0f;
        if (stick) {
            if (!c.vjoyAxis.empty()) glide(c.vjoyAxis, c.AxisValue(step.to ? 1.0f : 0.5f));
            if (!c.vjoyRollAxis.empty()) glide(c.vjoyRollAxis, c.RollAxisValue(step.right ? 1.0f : 0.5f));
        }
        if (adjustable) {
            const float value = valueOf(position);
            const bool ok = stick || glide(adjustAxis, value);
            printf("  [vJoy %s = %.2f%s]  Up/Down +-0.01, PgUp/PgDn +-0.05 if Condor is not at the right place\n",
                   adjustAxis.c_str(), value, ok ? "" : " FAILED");
        } else if (step.pulseBefore) {
            pulse(c);
            printf("  [vJoy button %u pressed]\n", c.vjoyButton);
        }
        const std::string& prompt = step.detent >= 0 ? c.detents[step.detent]
                                    : step.right     ? c.rightPrompt
                                    : step.to        ? c.toPrompt
                                                     : c.fromPrompt;
        printf("  %s ... ", prompt.c_str());
        audio.Say(prompt);

        // A pinch only counts with both hands tracked (a half-hidden palm-down hand reports false
        // pinches) and once held for kPinchConfirmSeconds. Then the other hand's position is
        // averaged while the pinch is held for kCaptureSeconds; letting go earlier starts over.
        constexpr double kPinchConfirmSeconds = 0.2;
        constexpr double kCaptureSeconds = 0.5;
        bool armed = false;
        int candidateHand = -1;
        double candidateStart = 0;
        int pinchingHand = -1;
        Vec3 sum;
        int samples = 0;
        double captureStart = 0;
        auto reset = [&] {
            candidateHand = pinchingHand = -1;
            sum = {};
            samples = 0;
        };
        const std::string stepName = step.control->name + (step.detent >= 0 ? "-" + std::to_string(step.detent)
                                                           : step.right             ? "-right"
                                                           : step.to                ? "-to"
                                                                                    : "-from");
        while (!gQuit) {
            // Keyboard corrections when Condor's lever isn't where the prompt says (global hotkeys,
            // registered below, so they work while Condor has the focus).
            MSG message;
            while (PeekMessageW(&message, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
                if (!adjustable) continue;
                const float delta = message.wParam < std::size(kAdjustKeys) ? kAdjustKeys[message.wParam].delta : 0;
                if (delta == 0) continue;
                position = std::clamp(position + delta, 0.0f, 1.0f);
                const float value = valueOf(position);
                vjoy.SetAxis(adjustAxis, value);
                axisValue[adjustAxis] = value;
                printf("\n    adjusted: vJoy %s = %.2f ", adjustAxis.c_str(), value);
            }

            HandState state[2];
            if (!ReadHands(hands, libovr, state)) return 1;
            const double now = Seconds();
            logFrame(stepName.c_str(), "frame", armed, candidateHand, pinchingHand, state);
            Sleep(kFramePeriodMs);

            if (!state[0].tracked || !state[1].tracked) {
                reset();
                continue;
            }
            if (pinchingHand < 0) {
                if (state[0].Pinch() < profile->pinchRelease && state[1].Pinch() < profile->pinchRelease) {
                    armed = true;  // both hands open: ready for the next pinch
                    candidateHand = -1;
                }
                if (!armed) continue;
                if (candidateHand >= 0 && state[candidateHand].Pinch() < profile->pinchGrab) {
                    candidateHand = -1;
                }
                for (int h = 0; candidateHand < 0 && h < 2; ++h) {
                    if (state[h].Pinch() >= profile->pinchGrab) {
                        candidateHand = h;
                        candidateStart = now;
                    }
                }
                if (candidateHand >= 0 && now - candidateStart >= kPinchConfirmSeconds) {
                    pinchingHand = candidateHand;
                    captureStart = now;
                    audio.ClickGrab();
                    logFrame(stepName.c_str(), "PINCH-CONFIRMED", armed, candidateHand, pinchingHand, state);
                }
                continue;
            }

            if (state[pinchingHand].Pinch() <= profile->pinchRelease) {
                logFrame(stepName.c_str(), "RELEASED-EARLY", armed, candidateHand, pinchingHand, state);
                reset();  // let go too early
                armed = false;
                continue;
            }
            sum = sum + state[1 - pinchingHand].position;
            ++samples;
            if (now - captureStart >= kCaptureSeconds) {
                logFrame(stepName.c_str(), "RECORDED", armed, candidateHand, pinchingHand, state);
                break;
            }
        }
        if (gQuit) {
            printf("aborted, profile not changed.\n");
            return 1;
        }
        const Vec3 point = sum * (1.0f / samples);
        ControlConfig& control = *step.control;
        if (step.detent >= 0) {
            control.detentPoints[step.detent] = point;
            if (step.detent == 0) control.from = point;
            if (step.detent + 1 == static_cast<int>(c.detents.size())) control.to = point;
        } else {
            (step.right ? control.right : step.to ? control.to : control.from) = point;
        }
        // Keep the vJoy value that put Condor's lever at this position.
        if (adjustable) {
            if (step.detent >= 0) {
                if (control.detentValues.size() != control.detents.size()) {
                    std::vector<float> values;
                    for (int i = 0; i < static_cast<int>(control.detents.size()); ++i) {
                        values.push_back(control.DetentPosition(i));
                    }
                    control.detentValues = values;
                }
                control.detentValues[step.detent] = position;
            } else if (rollStep) {
                control.vjoyRollRange[1] = c.RollAxisValue(position);
            } else {
                control.vjoyRange[step.to ? 1 : 0] = c.AxisValue(position);
            }
        }
        printf("(%+.3f %+.3f %+.3f) %s hand", point.x, point.y, point.z, pinchingHand ? "left" : "right");
        if (adjustable) printf(", vJoy %s = %.2f", adjustAxis.c_str(), valueOf(position));
        printf("\n");
        audio.ClickRelease();
        if (step.pulseAfter) pulse(c);
        Sleep(600);
    }

    // Levers back to their start-up value.
    for (const ControlConfig& c : profile->controls) {
        if (c.UsesStepButtons()) {
            stepTo(c, static_cast<int>(c.initial));
        } else if (c.type == ControlConfig::Type::Stick) {
            if (!c.vjoyAxis.empty()) glide(c.vjoyAxis, c.AxisValue(0.5f));
            if (!c.vjoyRollAxis.empty()) glide(c.vjoyRollAxis, c.RollAxisValue(0.5f));
        } else if (c.type == ControlConfig::Type::Lever && !c.vjoyAxis.empty()) {
            glide(c.vjoyAxis, c.AxisValue(c.InitialPosition()));
        }
    }

    std::string error;
    if (!profile->Save(path.wstring(), error)) {
        printf("Cannot save %ls: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    printf("Saved %ls\n", path.c_str());
    audio.Say("Calibration saved.");
    Sleep(2000);
    return 0;
}

int Run(const std::string& glider, const std::wstring& libovr, const std::wstring& vjoyDll, bool rudder) {
    fs::path path;
    auto profile = LoadProfile(glider, path);
    if (!profile) return 1;

    for (const ControlConfig& c : profile->controls) {
        if (!c.from || (c.type != ControlConfig::Type::Button && !c.to) ||
            (c.type == ControlConfig::Type::Stick && !c.right)) {
            printf("Warning: '%s' is not calibrated and will be ignored (condorhands calibrate %s %s)\n",
                   c.name.c_str(), glider.c_str(), c.name.c_str());
        }
    }

    VJoy vjoy;
    std::string error;
    if (!vjoy.Open(vjoyDll, profile->vjoyDevice, error)) {
        printf("vJoy: %s\n", error.c_str());
        return 1;
    }
    FocusCondor();
    Sleep(300);
    Audio audio;
    Cockpit cockpit(*profile, vjoy, audio, rudder);
    if (!rudder) printf("Rudder: not controlled (--no-rudder), use your own rudder controller.\n");

    OvrHands hands;
    if (!OpenHands(hands, libovr)) return 1;
    audio.Say("Hands ready.");
    printf("Running. Ctrl+C to stop.\n");

    while (!gQuit) {
        HandState state[2];
        if (!ReadHands(hands, libovr, state)) break;
        cockpit.Update(state, Seconds());
        Sleep(kFramePeriodMs);
    }
    return 0;
}

// Moves vJoy outputs on demand so Condor's input setup can detect them before calibration.
int Bind(const std::string& glider, const std::wstring& vjoyDll) {
    fs::path path;
    auto profile = LoadProfile(glider, path);
    if (!profile) return 1;
    VJoy vjoy;
    std::string error;
    if (!vjoy.Open(vjoyDll, profile->vjoyDevice, error)) {
        printf("vJoy: %s\n", error.c_str());
        return 1;
    }
    Audio audio;

    // One menu entry per vJoy output; a step-button lever has two (one per direction).
    struct Entry {
        std::string label;
        unsigned button = 0;  // 0: an axis
        std::string axis;
        float rest = 0;       // axis value to leave it at afterwards
    };
    std::vector<Entry> entries;
    for (const ControlConfig& c : profile->controls) {
        if (c.UsesStepButtons()) {
            entries.push_back({c.name + " one notch towards " + c.detents.front(), c.vjoyStepButtons[0]});
            entries.push_back({c.name + " one notch towards " + c.detents.back(), c.vjoyStepButtons[1]});
        } else if (c.type == ControlConfig::Type::Stick) {
            entries.push_back({c.name + " pitch (elevator)", 0, c.vjoyAxis, c.AxisValue(0.5f)});
            entries.push_back({c.name + " roll (ailerons)", 0, c.vjoyRollAxis, c.RollAxisValue(0.5f)});
            if (!c.vjoyRudderAxis.empty()) {
                entries.push_back({"rudder (controller twist)", 0, c.vjoyRudderAxis, c.RudderAxisValue(0.5f)});
            }
        } else if (c.type == ControlConfig::Type::Lever) {
            entries.push_back({c.name, 0, c.vjoyAxis, c.AxisValue(c.InitialPosition())});
        } else {
            entries.push_back({c.name, c.vjoyButton});
        }
    }

    for (;;) {
        printf("\nPress a number, then within 5 s select the control in Condor's input setup:\n");
        for (size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (e.button == 0) {
                printf("  %zu  %-32s moves vJoy axis %s to zero, half, full (announced)\n", i + 1, e.label.c_str(),
                       e.axis.c_str());
            } else {
                printf("  %zu  %-32s presses vJoy button %u\n", i + 1, e.label.c_str(), e.button);
            }
        }
        printf("  q  quit\n> ");
        const int key = _getwch();
        printf("%lc\n", key);
        if (key == L'q' || key == L'Q' || gQuit) return 0;
        const size_t index = static_cast<size_t>(key - L'1');
        if (index >= entries.size()) continue;
        const Entry& e = entries[index];
        for (int s = 5; s > 0 && !gQuit; --s) {
            printf("  %d...\n", s);
            Sleep(1000);
        }
        if (e.button == 0) {
            // Raw vJoy values, announced, so Condor's response can be watched: zero, half, full.
            float value = e.rest;
            const std::pair<float, const char*> holds[] = {{0.0f, "zero"}, {0.5f, "half"}, {1.0f, "full"}};
            for (const auto& [target, word] : holds) {
                for (int step = 1; step <= 50; ++step) {  // glide there in 1 s
                    vjoy.SetAxis(e.axis, value + (target - value) * step / 50.0f);
                    Sleep(20);
                }
                value = target;
                printf("  vJoy %s = %.1f (%s)\n", e.axis.c_str(), target, word);
                audio.Say(word);
                Sleep(2000);
            }
            vjoy.SetAxis(e.axis, e.rest);
        } else {
            vjoy.SetButton(e.button, true);
            Sleep(500);
            vjoy.SetButton(e.button, false);
        }
        printf("  done (%s)\n", e.label.c_str());
    }
}

void Usage() {
    printf("usage:\n"
           "  condorhands monitor\n"
           "  condorhands calibrate [<glider> [control ...]]\n"
           "  condorhands run [<glider>] [--no-rudder]\n"
           "  condorhands bind [<glider>]         move vJoy outputs for Condor's input setup\n"
           "  without <glider>, the gliders are listed to choose one by number\n"
           "options:\n"
           "  --libovr <path>   Condor's LibOVR.dll (default C:\\Condor3\\LibOVR.dll)\n"
           "  --vjoy <path>     32-bit vJoyInterface.dll (default C:\\Program Files\\vJoy\\x86\\...)\n"
           "  --no-rudder       run: leave the rudder to your own rudder controller (no controller twist)\n");
}

std::string Narrow(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring libovr = kDefaultLibOvr;
    std::wstring vjoyDll = kDefaultVJoy;
    bool rudder = true;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--libovr" && i + 1 < argc) {
            libovr = argv[++i];
        } else if (arg == L"--vjoy" && i + 1 < argc) {
            vjoyDll = argv[++i];
        } else if (arg == L"--no-rudder") {
            rudder = false;
        } else {
            args.push_back(Narrow(argv[i]));
        }
    }
    if (args.empty()) {
        Usage();
        return 1;
    }

    setvbuf(stdout, nullptr, _IONBF, 0); // keep output when redirected to a file
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    // run / calibrate / bind without a glider: choose one from the list.
    if (args.size() == 1 && (args[0] == "run" || args[0] == "calibrate" || args[0] == "bind")) {
        const std::string glider = ChooseGlider();
        if (glider.empty()) return 1;
        args.push_back(glider);
    }

    int result = 1;
    if (args[0] == "monitor") {
        result = Monitor(libovr);
    } else if (args[0] == "calibrate" && args.size() >= 2) {
        result = Calibrate(args[1], std::vector<std::string>(args.begin() + 2, args.end()), libovr, vjoyDll);
    } else if (args[0] == "run" && args.size() >= 2) {
        result = Run(args[1], libovr, vjoyDll, rudder);
    } else if (args[0] == "bind" && args.size() >= 2) {
        result = Bind(args[1], vjoyDll);
    } else {
        Usage();
    }

    timeEndPeriod(1);
    CoUninitialize();
    return result;
}
