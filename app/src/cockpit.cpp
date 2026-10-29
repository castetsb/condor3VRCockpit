#include "cockpit.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace {

constexpr float kHoverExitFactor = 1.3f;     // leave a zone a bit further out than entering it
constexpr float kToggleThreshold = 0.6f;     // fraction of travel that flips a toggle
constexpr float kToggleResyncFactor = 0.5f;  // grab this much nearer the other end: take that state
constexpr double kButtonPulseSeconds = 0.15;
constexpr float kDetentHysteresis = 0.005f;  // metres closer another notch must be to switch
constexpr float kEndMargin = 0.02f;          // lever position this close to 0 or 1 counts as an end
constexpr double kAnnounceGap = 1.0;         // seconds between position announcements while moving

double Seconds() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}
constexpr double kStepPressSeconds = 0.06;   // step buttons: press length...
constexpr double kStepGapSeconds = 0.06;     // ...and pause before the next press

using Type = ControlConfig::Type;

} // namespace

Cockpit::Cockpit(const Profile& profile, VJoy& vjoy, Audio& audio, bool rudder)
    : profile_(profile), vjoy_(vjoy), audio_(audio), autorudder_(profile.autorudderOn), rudder_(rudder) {
    for (const ControlConfig& config : profile.controls) {
        Control c{&config};
        if (config.type == Type::Toggle || config.type == Type::Button) {
            c.state = config.initial >= 0.5f ? 1 : 0;
            c.value = static_cast<float>(c.state);
        } else if (config.detents.size() > 1) {
            c.detent = std::clamp(static_cast<int>(config.initial), 0, static_cast<int>(config.detents.size()) - 1);
            c.value = config.DetentPosition(c.detent);
            c.sentDetent = c.detent;  // assumes Condor starts there; reaching an end notch resyncs
        } else {
            c.value = std::clamp(config.initial, 0.0f, 1.0f);
        }
        if (config.type == Type::Stick && stick_ < 0) stick_ = static_cast<int>(controls_.size());
        controls_.push_back(c);
    }
    for (Control& c : controls_) {
        Output(c);
    }
}

bool Cockpit::Calibrated(const Control& c) const {
    if (c.config->type == Type::Stick) return c.config->from && c.config->to && c.config->right;
    if (c.config->HasDetentPoints()) return true;
    return c.config->from && (c.config->type == Type::Button || c.config->to);
}

Vec3 Cockpit::HandlePosition(const Control& c) const {
    if (c.config->type == Type::Button) return *c.config->from;
    if (c.config->type == Type::Stick) {
        const Vec3 centre = *c.config->from;
        return centre + (*c.config->to - centre) * c.pitch + (*c.config->right - centre) * c.roll;
    }
    if (c.config->HasDetentPoints()) return c.config->detentPoints[c.detent];
    return Lerp(*c.config->from, *c.config->to, c.value);
}

int Cockpit::FindHover(const HandContext& hand, int side, Vec3 position) const {
    // Stay on the current zone until clearly out of it.
    if (hand.hover >= 0 && !controls_[hand.hover].held &&
        Length(position - HandlePosition(controls_[hand.hover])) < profile_.zoneRadius * kHoverExitFactor) {
        return hand.hover;
    }
    int best = -1;
    float bestDistance = profile_.zoneRadius;
    for (size_t i = 0; i < controls_.size(); ++i) {
        if (!Calibrated(controls_[i]) || controls_[i].held || !ForHand(controls_[i], side)) continue;
        float distance = Length(position - HandlePosition(controls_[i]));
        // A toggle can also be found at its other end: Condor may show it there (see Grab).
        if (controls_[i].config->type == Type::Toggle) {
            const ControlConfig& config = *controls_[i].config;
            distance = std::min(Length(position - *config.from), Length(position - *config.to));
        }
        if (distance < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

void Cockpit::Grab(HandContext& hand, int index, Vec3 position) {
    Control& c = controls_[index];
    // Toggle grabbed clearly at its other end: Condor's lever is there (e.g. gear up after a
    // start in flight), so take that state without pressing the button.
    if (c.config->type == Type::Toggle) {
        const Vec3 current = c.state == 0 ? *c.config->from : *c.config->to;
        const Vec3 other = c.state == 0 ? *c.config->to : *c.config->from;
        if (Length(position - other) < kToggleResyncFactor * Length(position - current)) {
            c.state = 1 - c.state;
            c.value = static_cast<float>(c.state);
            if (c.config->states.size() == 2) audio_.Say(c.config->states[c.state]);
        }
    }
    c.held = true;
    hand.grabbed = index;
    hand.grabPosition = position;
    hand.grabValue = c.value;
    hand.grabDetent = c.detent;
    // Feedback reports changes from here on.
    c.feedbackStep = static_cast<int>(std::lround(c.value * (c.config->feedback == ControlConfig::Feedback::Ticks ? 20 : 10)));
    c.atEnd = c.value <= kEndMargin || c.value >= 1 - kEndMargin;
    hand.grabHandle = HandlePosition(c);
    audio_.ClickGrab();
    if (c.config->type == Type::Button) {
        vjoy_.SetButton(c.config->vjoyButton, true);
        // A switch with two named states (e.g. the gear switch) flips at each press.
        if (c.config->states.size() == 2) {
            c.state = 1 - c.state;
            audio_.Say(c.config->states[c.state]);
        } else {
            audio_.Say(c.config->say);  // the state is unknown (e.g. the gear switch): confirm the press
        }
    }
}

void Cockpit::Move(HandContext& hand, Vec3 position, double now) {
    Control& c = controls_[hand.grabbed];
    if (c.config->type == Type::Button) return;

    // Stick: the grip follows the hand; its offset from the centre along the calibrated forward
    // and right directions gives pitch and roll (-1..1).
    if (c.config->type == Type::Stick) {
        const Vec3 centre = *c.config->from;
        const Vec3 forward = *c.config->to - centre;
        const Vec3 right = *c.config->right - centre;
        const Vec3 grip = hand.grabHandle + (position - hand.grabPosition) - centre;
        c.pitch = std::clamp(Dot(grip, forward) / std::max(Dot(forward, forward), 1e-6f), -1.0f, 1.0f);
        c.roll = std::clamp(Dot(grip, right) / std::max(Dot(right, right), 1e-6f), -1.0f, 1.0f);
        Output(c);
        return;
    }

    // Calibrated notches: the handle goes to the notch nearest to where the hand has moved it,
    // changing only when clearly closer to another one.
    if (c.config->HasDetentPoints()) {
        const std::vector<Vec3>& points = c.config->detentPoints;
        const Vec3 handle = points[hand.grabDetent] + (position - hand.grabPosition);
        int nearest = c.detent;
        float nearestDistance = Length(handle - points[c.detent]) - kDetentHysteresis;
        for (size_t i = 0; i < points.size(); ++i) {
            const float distance = Length(handle - points[i]);
            if (distance < nearestDistance) {
                nearest = static_cast<int>(i);
                nearestDistance = distance;
            }
        }
        if (nearest != c.detent) {
            c.detent = nearest;
            c.value = c.config->DetentPosition(nearest);
            audio_.Say(c.config->detents[nearest]);
            Output(c);
        }
        return;
    }

    // Relative to where the handle was grabbed, so it never jumps under the hand.
    const Vec3 handle = Lerp(*c.config->from, *c.config->to, hand.grabValue) + (position - hand.grabPosition);
    const float raw = std::clamp(ProjectOnSegment(handle, *c.config->from, *c.config->to), 0.0f, 1.0f);

    if (c.config->type == Type::Toggle) {
        c.value = raw;
        const int wanted = c.state == 0 ? (raw > kToggleThreshold ? 1 : 0) : (raw < 1 - kToggleThreshold ? 0 : 1);
        if (wanted != c.state) {
            c.state = wanted;
            c.pulseUntil = now + kButtonPulseSeconds;
            vjoy_.SetButton(c.config->vjoyButton, true);
            // Without named states (the side Condor starts on is unknown), confirm with the name.
            audio_.Say(c.config->states.size() == 2 ? c.config->states[c.state] : c.config->say);
        }
        return;
    }

    if (c.config->detents.size() > 1) {
        const int last = static_cast<int>(c.config->detents.size()) - 1;
        const int detent = static_cast<int>(std::lround(raw * last));
        c.value = raw;
        if (detent != c.detent) {
            c.detent = detent;
            audio_.Say(c.config->detents[detent]);
            Output(c);
        }
        return;
    }

    c.value = raw;
    Output(c);
    PositionFeedback(c, now);
}

// While a lever without notches moves: buzz on reaching either end; then either pitch ticks
// every 5 % with a double beep at the centre (Ticks), or the position by voice every 10 %,
// at most once per kAnnounceGap (Announce).
void Cockpit::PositionFeedback(Control& c, double now) {
    const ControlConfig::Feedback feedback = c.config->feedback;
    if (feedback == ControlConfig::Feedback::None) return;
    const bool atEnd = c.value <= kEndMargin || c.value >= 1 - kEndMargin;
    const bool reachedEnd = atEnd && !c.atEnd;
    c.atEnd = atEnd;

    if (feedback == ControlConfig::Feedback::Ticks) {
        const int step = static_cast<int>(std::lround(c.value * 20));
        if (reachedEnd) {
            audio_.EndStop();
        } else if (step != c.feedbackStep) {
            if (step == 10) {
                audio_.CentreMark();
            } else if (!atEnd) {
                audio_.Tick(c.value);
            }
        }
        c.feedbackStep = step;
        return;
    }

    if (reachedEnd) audio_.EndStop();
    const int tenth = static_cast<int>(std::lround(c.value * 10));
    if (tenth != c.feedbackStep && now - c.spokenAt >= kAnnounceGap) AnnouncePosition(c, now);
}

// "brakes 40", or the end words: "brakes closed", "brakes full".
void Cockpit::AnnouncePosition(Control& c, double now) {
    const ControlConfig& config = *c.config;
    const int tenth = static_cast<int>(std::lround(c.value * 10));
    std::string words = config.announce + " ";
    if (tenth == 0 && !config.announceEnds.empty()) {
        words += config.announceEnds[0];
    } else if (tenth == 10 && config.announceEnds.size() > 1) {
        words += config.announceEnds[1];
    } else {
        words += std::to_string(tenth * 10);
    }
    audio_.Say(words);
    c.feedbackStep = tenth;
    c.spokenAt = now;
}

void Cockpit::Release(HandContext& hand) {
    Control& c = controls_[hand.grabbed];
    c.held = false;
    switch (c.config->type) {
    case Type::Button:
        vjoy_.SetButton(c.config->vjoyButton, false);
        break;
    case Type::Toggle:
        c.value = static_cast<float>(c.state);
        break;
    case Type::Lever:
        if (c.config->detents.size() > 1) c.value = c.config->DetentPosition(c.detent);
        // Final position, said even if just announced, so the last word is where it stopped.
        if (c.config->feedback == ControlConfig::Feedback::Announce) AnnouncePosition(c, Seconds());
        break;
    case Type::Stick:
        c.pitch = c.roll = c.rudder = 0;  // sprung: back to the centre when let go
        Output(c);
        break;
    }
    hand.grabbed = -1;
    audio_.ClickRelease();
}

bool Cockpit::ForHand(const Control& c, int side) const {
    switch (c.config->hand) {
    case ControlConfig::HandSide::Left: return side == 0;
    case ControlConfig::HandSide::Right: return side == 1;
    default: return true;
    }
}

void Cockpit::Output(Control& c) {
    if (c.config->type == Type::Stick) {
        if (!c.config->vjoyAxis.empty()) vjoy_.SetAxis(c.config->vjoyAxis, c.config->AxisValue((c.pitch + 1) / 2));
        if (!c.config->vjoyRollAxis.empty()) {
            vjoy_.SetAxis(c.config->vjoyRollAxis, c.config->RollAxisValue((c.roll + 1) / 2));
        }
        if (rudder_ && !c.config->vjoyRudderAxis.empty()) {
            vjoy_.SetAxis(c.config->vjoyRudderAxis, c.config->RudderAxisValue((c.rudder + 1) / 2));
        }
        return;
    }
    if (c.config->UsesStepButtons()) {
        c.pendingSteps += c.detent - c.sentDetent;
        c.sentDetent = c.detent;
        // At an end notch, extra presses push Condor against its end stop: both sides agree again.
        const int count = static_cast<int>(c.config->detents.size());
        if (c.detent == 0) c.pendingSteps -= count;
        if (c.detent == count - 1) c.pendingSteps += count;
        return;
    }
    if (c.config->type != Type::Lever || c.config->vjoyAxis.empty()) return;
    const float value = c.config->detents.size() > 1 ? c.config->DetentPosition(c.detent) : c.value;
    vjoy_.SetAxis(c.config->vjoyAxis, c.config->AxisValue(value));
}

void Cockpit::SendSteps(Control& c, double now) {
    if (now < c.stepTime) return;
    if (c.stepButtonDown) {
        vjoy_.SetButton(c.stepButtonDown, false);
        c.stepButtonDown = 0;
        c.stepTime = now + kStepGapSeconds;
    } else if (c.pendingSteps != 0) {
        c.stepButtonDown = c.config->vjoyStepButtons[c.pendingSteps < 0 ? 0 : 1];
        c.pendingSteps += c.pendingSteps < 0 ? 1 : -1;
        vjoy_.SetButton(c.stepButtonDown, true);
        c.stepTime = now + kStepPressSeconds;
    }
}

// Controller mode: since the grip was squeezed, moving the controller forward / back and left /
// right moves the stick (tracking space: -z is forward, +x is right), and twisting it about the
// vertical works the rudder, unless Condor's autorudder is on.
void Cockpit::StickFromController(Control& c, const HandContext& hand, const HandState& state) {
    const Vec3 moved = state.position - hand.neutralPosition;
    const float travel = std::max(profile_.controllerStickTravel, 0.01f);
    c.pitch = std::clamp(-moved.z / travel, -1.0f, 1.0f);
    c.roll = std::clamp(moved.x / travel, -1.0f, 1.0f);

    c.rudder = 0;
    if (rudder_ && !autorudder_) {
        // Signed angle between the controller's horizontal heading now and at the grab.
        const Vec3 before = Rotate(hand.neutral, {0, 0, -1});
        const Vec3 now = Rotate(state.orientation, {0, 0, -1});
        const float turnLeft = std::atan2(before.z * now.x - before.x * now.z, before.x * now.x + before.z * now.z);
        const float fullRudder = std::max(profile_.controllerRudderAngle, 1.0f) * 3.14159265f / 180;
        c.rudder = std::clamp(-turnLeft / fullRudder, -1.0f, 1.0f);
    }
    Output(c);
    // Live values in the console, to check what reaches vJoy.
    static double shownAt = 0;
    if (Seconds() - shownAt > 0.25) {
        shownAt = Seconds();
        printf("\r  stick: pitch %+.2f  roll %+.2f  rudder %+.2f%s   ", c.pitch, c.roll, c.rudder,
               !rudder_ ? " (--no-rudder)" : autorudder_ ? " (autorudder on)" : "");
        fflush(stdout);
    }
}

// Controller A button: Condor's autorudder key, sent as a keyboard press to the window in front.
void Cockpit::ToggleAutorudder() {
    if (!profile_.autorudderKey.empty()) {
        const SHORT vk = VkKeyScanA(profile_.autorudderKey[0]);
        INPUT keys[2] = {};
        for (INPUT& key : keys) {
            key.type = INPUT_KEYBOARD;
            key.ki.wScan = static_cast<WORD>(MapVirtualKeyA(LOBYTE(vk), MAPVK_VK_TO_VSC));
            key.ki.dwFlags = KEYEVENTF_SCANCODE;  // scan codes reach games reading the keyboard directly
        }
        keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, keys, sizeof(INPUT));
    }
    autorudder_ = !autorudder_;
    audio_.Say(autorudder_ ? "auto rudder on" : "auto rudder off");
}

// The headset switched between hands and controllers: positions jump, so let go of everything.
void Cockpit::SwitchMode(bool controllers) {
    controllerMode_ = controllers;
    for (HandContext& hand : hands_) {
        if (hand.grabbed >= 0) Release(hand);
        hand.hover = -1;
        hand.pinching = false;
    }
    audio_.Say(controllers ? "controllers" : "hands");
}

void Cockpit::Update(const HandState (&hands)[2], double now) {
    if (hands[0].controller != controllerMode_) SwitchMode(hands[0].controller);

    for (Control& c : controls_) {
        if (c.pulseUntil > 0 && now >= c.pulseUntil) {
            vjoy_.SetButton(c.config->vjoyButton, false);
            c.pulseUntil = 0;
        }
        if (c.config->UsesStepButtons()) SendSteps(c, now);
        // Reminder of a lever left away from its `from` end (open airbrakes).
        if (c.config->feedback == ControlConfig::Feedback::Announce && !c.held && c.value > kEndMargin &&
            now - c.spokenAt >= c.config->announceEvery) {
            AnnouncePosition(c, now);
        }
    }

    for (int h = 0; h < 2; ++h) {
        HandContext& hand = hands_[h];
        const HandState& state = hands[h];
        if (!state.tracked) {
            if (hand.grabbed >= 0) Release(hand);
            hand.hover = -1;
            hand.pinching = false;
            continue;
        }

        const float pinch = state.Pinch();
        const bool wasPinching = hand.pinching;
        hand.pinching = wasPinching ? pinch > profile_.pinchRelease : pinch >= profile_.pinchGrab;

        // Controllers: right A button toggles the autorudder.
        if (controllerMode_ && h == 1 && (state.buttons & kButtonPrimary) && !(hand.buttons & kButtonPrimary)) {
            ToggleAutorudder();
        }
        hand.buttons = state.buttons;

        if (hand.grabbed >= 0) {
            if (!hand.pinching) {
                Release(hand);
            } else if (controllerMode_ && hand.grabbed == stick_) {
                StickFromController(controls_[stick_], hand, state);
            } else {
                Move(hand, state.position, now);
            }
            continue;
        }

        const int hover = FindHover(hand, h, state.position);
        if (hover != hand.hover) {
            hand.hover = hover;
            if (hover >= 0) audio_.Say(controls_[hover].config->say);
        }
        // Only a pinch that starts inside a zone grabs, so moving a pinched hand past a handle doesn't.
        if (hand.pinching && !wasPinching) {
            if (hand.hover >= 0 && controllerMode_ && hand.hover == stick_) {
                // Controller stick: moves from where it was taken (see StickFromController).
                controls_[stick_].held = true;
                hand.grabbed = stick_;
                hand.neutralPosition = state.position;
                hand.neutral = state.orientation;
                audio_.ClickGrab();
            } else if (hand.hover >= 0) {
                Grab(hand, hand.hover, state.position);
            }
        }
    }
}
