#pragma once

#include <string>
#include <vector>

// Spoken words (Windows SAPI) and short click tones, both asynchronous.
class Audio {
public:
    Audio();
    ~Audio();

    // Interrupts whatever is being said: the newest information wins.
    void Say(const std::string& text);
    void ClickGrab();
    void ClickRelease();
    // Lever position feedback: a short tick whose pitch rises with the position (0..1), a
    // double beep at the centre, and a low buzz at either end of travel.
    void Tick(float position);
    void CentreMark();
    void EndStop();

private:
    void Play(const std::vector<char>& wav);

    struct ISpVoice* voice_ = nullptr;
    std::vector<char> grabTone_;
    std::vector<char> releaseTone_;
    std::vector<std::vector<char>> ticks_;  // one per 5 % of travel
    std::vector<char> centreMark_;
    std::vector<char> endStop_;
};
