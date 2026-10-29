#pragma once

#include <cstdint>
#include <string>

#include "math.h"

enum class Hand { Left = 0, Right = 1 };

constexpr uint32_t kButtonPrimary = 0x1;    // A on the right controller, X on the left
constexpr uint32_t kButtonSecondary = 0x2;  // B / Y

struct HandState {
    bool controller = false;  // the headset is in controller mode: this "hand" is a Touch controller
    bool tracked = false;     // fresh data (the runtime repeats the last frame for ~15 s after losing a hand)
    bool palmFacing = false;  // palm towards the face: Meta shows its system icon, pinches open the menu
    Vec3 position;            // metres, in the Oculus tracking origin (follows recentering)
    Quat orientation;
    float pinch[4] = {};      // index, middle, ring, pinky pinch strength, 0..1; controller: [0] = grip
    uint32_t buttons = 0;     // controller: kButtonPrimary (A / X), kButtonSecondary (B / Y)
    uint32_t flags = 0;

    // A pinch that should count: fresh data and not the Meta system gesture.
    float Pinch() const { return tracked && !palmFacing ? pinch[0] : 0.0f; }
};

// Reads Quest hand tracking over Link next to a running Oculus-SDK app (Condor), or the Touch
// controllers when the headset has switched to them (public ovr_GetTrackingState / InputState).
//
// Uses an invisible LibOVR session plus the runtime's undocumented
// ovr_GetCurrentHandInputState(session, hand, out) export. Layout of the 52-byte output,
// observed on Meta Horizon Link runtime 2026-10:
//   [0] unknown  [1..4] pinch strengths  [5..8] orientation xyzw  [9..11] position  [12] flags
// Returns 0 when the hand is known and 1002 when it isn't. Flags match OVRPlugin's HandStatus:
// 0x02 input state valid, 0x40 system gesture in progress (palm facing), 0x80 dominant hand.
// With neither 0x02 nor 0x40 the data is a stale repeat.
class OvrHands {
public:
    ~OvrHands();

    // Loads LibOVR through Condor's own shim DLL and opens an invisible session.
    bool Open(const std::wstring& libovrPath, std::string& error);
    void Close();
    bool IsOpen() const { return session_ != nullptr; }

    // Both hands, from hand tracking or from the Touch controllers, whichever the headset is
    // using (it never tracks both at once). False means the session broke (headset or runtime
    // gone); the caller should reopen.
    bool Read(HandState (&state)[2]);

private:
    bool ReadHand(Hand hand, HandState& state);
    bool ReadControllers(HandState (&state)[2]);

    struct Api;
    Api* api_ = nullptr;
    void* session_ = nullptr;
    bool initialized_ = false;
};
