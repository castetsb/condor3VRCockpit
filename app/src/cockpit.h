#pragma once

#include <vector>

#include "audio.h"
#include "ovr_hands.h"
#include "profile.h"
#include "vjoy.h"

// Hand-driven cockpit controls: hover a handle to hear its name, pinch to grab it,
// move the hand to move it, open the fingers to let go. Output goes to vJoy.
class Cockpit {
public:
    // rudder = false: the rudder axis is never written, so a real rudder controller can be used.
    Cockpit(const Profile& profile, VJoy& vjoy, Audio& audio, bool rudder = true);

    void Update(const HandState (&hands)[2], double now);

private:
    struct Control {
        const ControlConfig* config;
        float value = 0;        // lever position 0..1 (from -> to)
        int detent = 0;         // levers with detents
        int state = 0;          // toggles: 0 = from, 1 = to
        bool held = false;
        double pulseUntil = 0;  // toggles: button released at this time
        float pitch = 0;        // stick deflection -1..1: back -> forward
        float roll = 0;         // stick deflection -1..1: left -> right
        float rudder = 0;       // stick, controller mode: rudder -1..1, left -> right
        // Step-button levers: notch Condor was last told about, presses still to send
        // (negative = towards the first notch), and the press in progress.
        int sentDetent = 0;
        int pendingSteps = 0;
        unsigned stepButtonDown = 0;
        double stepTime = 0;
        // Position feedback: last 5 % / 10 % step reported, whether at an end, last announcement.
        int feedbackStep = 0;
        bool atEnd = true;
        double spokenAt = 0;
    };
    struct HandContext {
        int hover = -1;
        int grabbed = -1;
        bool pinching = false;
        Vec3 grabPosition;
        float grabValue = 0;
        int grabDetent = 0;
        Vec3 grabHandle;
        // Controller mode, stick: controller position and orientation when the grip was squeezed.
        Vec3 neutralPosition;
        Quat neutral;
        uint32_t buttons = 0;  // controller buttons last frame, to see presses
    };

    bool Calibrated(const Control& c) const;
    Vec3 HandlePosition(const Control& c) const;
    int FindHover(const HandContext& hand, int side, Vec3 position) const;  // side: 0 left, 1 right
    bool ForHand(const Control& c, int side) const;
    void Grab(HandContext& hand, int index, Vec3 position);
    void Move(HandContext& hand, Vec3 position, double now);
    void Release(HandContext& hand);
    void Output(Control& c);
    void SendSteps(Control& c, double now);
    void StickFromController(Control& c, const HandContext& hand, const HandState& state);
    void ToggleAutorudder();
    void PositionFeedback(Control& c, double now);
    void AnnouncePosition(Control& c, double now);
    void SwitchMode(bool controllers);

    const Profile& profile_;
    VJoy& vjoy_;
    Audio& audio_;
    std::vector<Control> controls_;
    HandContext hands_[2];
    bool controllerMode_ = false;
    bool autorudder_ = false;  // Condor's autorudder, as toggled with the controller A button
    bool rudder_ = true;       // controller twist drives the rudder axis
    int stick_ = -1;  // index of the stick control, if any
};
