#include "profile.h"

#include <fstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

const char* TypeName(ControlConfig::Type type) {
    switch (type) {
    case ControlConfig::Type::Toggle: return "toggle";
    case ControlConfig::Type::Button: return "button";
    case ControlConfig::Type::Stick: return "stick";
    default: return "lever";
    }
}

const char* HandName(ControlConfig::HandSide hand) {
    switch (hand) {
    case ControlConfig::HandSide::Left: return "left";
    case ControlConfig::HandSide::Right: return "right";
    default: return "any";
    }
}

ControlConfig::HandSide ParseHand(const std::string& name) {
    if (name == "left") return ControlConfig::HandSide::Left;
    if (name == "right") return ControlConfig::HandSide::Right;
    return ControlConfig::HandSide::Any;
}

const char* FeedbackName(ControlConfig::Feedback feedback) {
    switch (feedback) {
    case ControlConfig::Feedback::Announce: return "announce";
    case ControlConfig::Feedback::Ticks: return "ticks";
    default: return "none";
    }
}

ControlConfig::Feedback ParseFeedback(const std::string& name) {
    if (name == "announce") return ControlConfig::Feedback::Announce;
    if (name == "ticks") return ControlConfig::Feedback::Ticks;
    return ControlConfig::Feedback::None;
}

ControlConfig::Type ParseType(const std::string& name) {
    if (name == "toggle") return ControlConfig::Type::Toggle;
    if (name == "button") return ControlConfig::Type::Button;
    if (name == "stick") return ControlConfig::Type::Stick;
    return ControlConfig::Type::Lever;
}

std::optional<Vec3> ReadPoint(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return std::nullopt;
    return Vec3{j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>()};
}

json WritePoint(const std::optional<Vec3>& p) {
    return p ? json::array({p->x, p->y, p->z}) : json(nullptr);
}

} // namespace

std::optional<Profile> Profile::Load(const std::wstring& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open profile";
        return std::nullopt;
    }
    try {
        const json j = json::parse(in, nullptr, true, /*ignore_comments=*/true);
        Profile p;
        p.glider = j.value("glider", "");
        p.vjoyDevice = j.value("vjoyDevice", 1u);
        p.zoneRadius = j.value("zoneRadius", 0.10f);
        p.pinchGrab = j.value("pinchGrab", 0.5f);
        p.pinchRelease = j.value("pinchRelease", 0.25f);
        p.controllerStickTravel = j.value("controllerStickTravel", 0.10f);
        p.controllerRudderAngle = j.value("controllerRudderAngle", 30.0f);
        p.autorudderKey = j.value("autorudderKey", "A");
        p.autorudderOn = j.value("autorudderOn", false);
        for (const json& c : j.at("controls")) {
            ControlConfig cc;
            cc.name = c.at("name").get<std::string>();
            cc.type = ParseType(c.value("type", "lever"));
            cc.say = c.value("say", cc.name);
            cc.hand = ParseHand(c.value("hand", "any"));
            cc.from = ReadPoint(c, "from");
            cc.to = ReadPoint(c, "to");
            cc.right = ReadPoint(c, "right");
            cc.fromPrompt = c.value("fromPrompt", cc.name);
            cc.toPrompt = c.value("toPrompt", cc.name);
            cc.rightPrompt = c.value("rightPrompt", cc.name);
            cc.vjoyAxis = c.value("vjoyAxis", "");
            if (c.contains("vjoyRange") && c["vjoyRange"].is_array() && c["vjoyRange"].size() == 2) {
                cc.vjoyRange[0] = c["vjoyRange"][0].get<float>();
                cc.vjoyRange[1] = c["vjoyRange"][1].get<float>();
            }
            cc.vjoyRollAxis = c.value("vjoyRollAxis", "");
            if (c.contains("vjoyRollRange") && c["vjoyRollRange"].is_array() && c["vjoyRollRange"].size() == 2) {
                cc.vjoyRollRange[0] = c["vjoyRollRange"][0].get<float>();
                cc.vjoyRollRange[1] = c["vjoyRollRange"][1].get<float>();
            }
            cc.vjoyRudderAxis = c.value("vjoyRudderAxis", "");
            if (c.contains("vjoyRudderRange") && c["vjoyRudderRange"].is_array() && c["vjoyRudderRange"].size() == 2) {
                cc.vjoyRudderRange[0] = c["vjoyRudderRange"][0].get<float>();
                cc.vjoyRudderRange[1] = c["vjoyRudderRange"][1].get<float>();
            }
            cc.vjoyButton = c.value("vjoyButton", 0u);
            cc.detents = c.value("detents", std::vector<std::string>{});
            if (c.contains("detentPoints") && c["detentPoints"].is_array()) {
                for (const json& point : c["detentPoints"]) {
                    if (point.is_array() && point.size() == 3) {
                        cc.detentPoints.push_back({point[0].get<float>(), point[1].get<float>(), point[2].get<float>()});
                    }
                }
            }
            cc.detentValues = c.value("detentValues", std::vector<float>{});
            if (c.contains("vjoyStepButtons") && c["vjoyStepButtons"].is_array() && c["vjoyStepButtons"].size() == 2) {
                cc.vjoyStepButtons[0] = c["vjoyStepButtons"][0].get<unsigned>();
                cc.vjoyStepButtons[1] = c["vjoyStepButtons"][1].get<unsigned>();
            }
            cc.states = c.value("states", std::vector<std::string>{});
            cc.initial = c.value("initial", 0.0f);
            cc.feedback = ParseFeedback(c.value("feedback", "none"));
            cc.announce = c.value("announce", cc.say);
            cc.announceEnds = c.value("announceEnds", cc.announceEnds);
            cc.announceEvery = c.value("announceEvery", cc.announceEvery);
            p.controls.push_back(cc);
        }
        return p;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

bool Profile::Save(const std::wstring& path, std::string& error) const {
    json j;
    j["glider"] = glider;
    j["vjoyDevice"] = vjoyDevice;
    j["zoneRadius"] = zoneRadius;
    j["pinchGrab"] = pinchGrab;
    j["pinchRelease"] = pinchRelease;
    j["controllerStickTravel"] = controllerStickTravel;
    j["controllerRudderAngle"] = controllerRudderAngle;
    j["autorudderKey"] = autorudderKey;
    j["autorudderOn"] = autorudderOn;
    j["controls"] = json::array();
    for (const ControlConfig& c : controls) {
        json cj;
        cj["name"] = c.name;
        cj["type"] = TypeName(c.type);
        cj["say"] = c.say;
        cj["hand"] = HandName(c.hand);
        cj["fromPrompt"] = c.fromPrompt;
        cj["from"] = WritePoint(c.from);
        if (c.type != ControlConfig::Type::Button) {
            cj["toPrompt"] = c.toPrompt;
            cj["to"] = WritePoint(c.to);
        }
        if (c.type == ControlConfig::Type::Stick) {
            cj["rightPrompt"] = c.rightPrompt;
            cj["right"] = WritePoint(c.right);
        }
        if (!c.vjoyAxis.empty()) {
            cj["vjoyAxis"] = c.vjoyAxis;
            cj["vjoyRange"] = json::array({c.vjoyRange[0], c.vjoyRange[1]});
        }
        if (!c.vjoyRollAxis.empty()) {
            cj["vjoyRollAxis"] = c.vjoyRollAxis;
            cj["vjoyRollRange"] = json::array({c.vjoyRollRange[0], c.vjoyRollRange[1]});
        }
        if (!c.vjoyRudderAxis.empty()) {
            cj["vjoyRudderAxis"] = c.vjoyRudderAxis;
            cj["vjoyRudderRange"] = json::array({c.vjoyRudderRange[0], c.vjoyRudderRange[1]});
        }
        if (c.vjoyButton) cj["vjoyButton"] = c.vjoyButton;
        if (!c.detents.empty()) cj["detents"] = c.detents;
        if (!c.detentPoints.empty()) {
            cj["detentPoints"] = json::array();
            for (const Vec3& p : c.detentPoints) cj["detentPoints"].push_back(WritePoint(p));
        }
        if (!c.detentValues.empty()) cj["detentValues"] = c.detentValues;
        if (c.vjoyStepButtons[0] || c.vjoyStepButtons[1]) {
            cj["vjoyStepButtons"] = json::array({c.vjoyStepButtons[0], c.vjoyStepButtons[1]});
        }
        if (!c.states.empty()) cj["states"] = c.states;
        cj["initial"] = c.initial;
        if (c.feedback != ControlConfig::Feedback::None) {
            cj["feedback"] = FeedbackName(c.feedback);
            if (c.feedback == ControlConfig::Feedback::Announce) {
                cj["announce"] = c.announce;
                cj["announceEnds"] = c.announceEnds;
                cj["announceEvery"] = c.announceEvery;
            }
        }
        j["controls"].push_back(cj);
    }
    std::ofstream out(path);
    if (!out) {
        error = "cannot write profile";
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}
