#pragma once

#include <optional>
#include <string>
#include <vector>

#include "math.h"

// One glider's control layout. Positions are measured by calibration, in metres in the
// Oculus tracking origin, so they line up with Condor's cockpit after a recenter.
struct ControlConfig {
    enum class Type { Lever, Toggle, Button, Stick };
    enum class HandSide { Any, Left, Right };
    // Position feedback for a lever without notches:
    // Announce = voice, every 10 % while moving and periodically while not at `from` (airbrake);
    // Ticks = pitch ticks every 5 %, double beep at the centre (trim). Both buzz at each end.
    enum class Feedback { None, Announce, Ticks };

    std::string name;           // id used on the command line, e.g. "airbrake"
    Type type = Type::Lever;
    std::string say;            // spoken when a hand enters the zone
    HandSide hand = HandSide::Any;  // only this hand can use the control

    // Lever / toggle: handle position at both ends of travel. Button: `from` only.
    // Stick: `from` = centred, `to` = fully forward, `right` = fully right.
    std::optional<Vec3> from;
    std::optional<Vec3> to;
    std::optional<Vec3> right;
    std::string fromPrompt;     // calibration prompts, e.g. "airbrake closed"
    std::string toPrompt;
    std::string rightPrompt;

    std::string vjoyAxis;       // lever; stick: pitch
    float vjoyRange[2] = {0, 1};  // lever: vJoy axis value at `from` and at `to` (reversed if from > to);
                                  // stick: pitch value at full back and full forward
    std::string vjoyRollAxis;     // stick
    float vjoyRollRange[2] = {0, 1};  // stick: roll value at full left and full right
    std::string vjoyRudderAxis;   // stick, controller mode: rudder from twisting the controller
    float vjoyRudderRange[2] = {0, 1};  // stick: rudder value at full left and full right
    unsigned vjoyButton = 0;    // toggle (pulsed on each change) / button (held while grabbed)

    std::vector<std::string> detents;  // lever with notches (flaps): spoken labels, from -> to
    std::vector<Vec3> detentPoints;    // calibrated handle position at each notch (notches are not evenly spaced)
    std::vector<float> detentValues;   // optional lever position 0..1 sent for each notch (default: even split)
    // Notched lever driven like Condor's keys instead of an axis: buttons that move one notch
    // towards the first / the last notch. 0 = unused.
    unsigned vjoyStepButtons[2] = {0, 0};
    bool UsesStepButtons() const { return detents.size() > 1 && vjoyStepButtons[0] && vjoyStepButtons[1]; }
    std::vector<std::string> states;   // toggle, or button that is a switch: spoken labels for state 0 / 1

    Feedback feedback = Feedback::None;
    std::string announce;              // Announce: word before the position, e.g. "brakes"
    std::vector<std::string> announceEnds = {"closed", "full"};  // Announce: words at `from` / `to`
    float announceEvery = 8;           // Announce: seconds between reminders while not at `from`

    // Value at start-up: lever 0..1, detent index, or toggle state (0 = from, 1 = to).
    float initial = 0;

    // vJoy axis value for a lever position 0..1 (from -> to).
    float AxisValue(float position) const { return vjoyRange[0] + position * (vjoyRange[1] - vjoyRange[0]); }
    // Stick: vJoy roll value for a position 0..1 (left -> right).
    float RollAxisValue(float position) const {
        return vjoyRollRange[0] + position * (vjoyRollRange[1] - vjoyRollRange[0]);
    }
    // Stick: vJoy rudder value for a position 0..1 (left -> right).
    float RudderAxisValue(float position) const {
        return vjoyRudderRange[0] + position * (vjoyRudderRange[1] - vjoyRudderRange[0]);
    }
    // Lever position 0..1 sent for a notch.
    float DetentPosition(int index) const {
        if (detentValues.size() == detents.size()) return detentValues[index];
        return static_cast<float>(index) / (detents.size() - 1);
    }
    bool HasDetentPoints() const { return detents.size() > 1 && detentPoints.size() == detents.size(); }
    // Lever position 0..1 at start-up.
    float InitialPosition() const {
        return detents.size() > 1 ? DetentPosition(static_cast<int>(initial)) : initial;
    }
};

struct Profile {
    std::string glider;
    unsigned vjoyDevice = 1;
    float zoneRadius = 0.10f;   // metres around the handle that count as touching it
    float pinchGrab = 0.5f;     // index pinch strength that grabs...
    float pinchRelease = 0.25f; // ...and that lets go (with controllers: the grip squeeze)
    // Controller mode stick: hand travel (metres) for full pitch / roll, controller twist
    // (degrees) for full rudder.
    float controllerStickTravel = 0.10f;
    float controllerRudderAngle = 30;
    // Controller A button: key sent to Condor to toggle its autorudder (rudder twist is ignored
    // while autorudder is on), and whether autorudder is on at start.
    std::string autorudderKey = "A";
    bool autorudderOn = false;
    std::vector<ControlConfig> controls;

    static std::optional<Profile> Load(const std::wstring& path, std::string& error);
    bool Save(const std::wstring& path, std::string& error) const;
};
