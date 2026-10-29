#include "audio.h"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// A short tone as an in-memory WAV, playable with PlaySound(SND_MEMORY). `beeps` > 1 splits it
// into that many beeps with gaps; `square` gives a buzz instead of a sine.
std::vector<char> MakeTone(float frequency, float seconds, int beeps = 1, bool square = false) {
    constexpr uint32_t kRate = 22050;
    const uint32_t samples = static_cast<uint32_t>(kRate * seconds);
    const uint32_t dataBytes = samples * 2;
    std::vector<char> wav(44 + dataBytes);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 1); // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, dataBytes);
    const uint32_t beepLength = samples / beeps;
    for (uint32_t i = 0; i < samples; ++i) {
        const uint32_t inBeep = i % beepLength;
        const bool gap = beeps > 1 && inBeep > beepLength * 6 / 10;
        const float fade = 1.0f - static_cast<float>(inBeep) / beepLength;
        float wave = std::sin(6.2831853f * frequency * i / kRate);
        if (square) wave = wave >= 0 ? 0.6f : -0.6f;
        const float s = gap ? 0.0f : wave * fade * 0.5f;
        put16(44 + i * 2, static_cast<uint16_t>(static_cast<int16_t>(s * 32767)));
    }
    return wav;
}

std::wstring Widen(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
    return wide;
}

} // namespace

Audio::Audio()
    : grabTone_(MakeTone(1320, 0.06f)),
      releaseTone_(MakeTone(660, 0.06f)),
      centreMark_(MakeTone(1000, 0.16f, 2)),
      endStop_(MakeTone(180, 0.15f, 1, true)) {
    // Ticks from 400 Hz at one end to 1600 Hz at the other (two octaves).
    for (int i = 0; i <= 20; ++i) ticks_.push_back(MakeTone(400.0f * std::pow(4.0f, i / 20.0f), 0.03f));
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice_));
}

Audio::~Audio() {
    if (voice_) voice_->Release();
}

void Audio::Say(const std::string& text) {
    if (voice_) voice_->Speak(Widen(text).c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

void Audio::Play(const std::vector<char>& wav) {
    PlaySoundA(wav.data(), nullptr, SND_MEMORY | SND_ASYNC);
}

void Audio::ClickGrab() {
    Play(grabTone_);
}

void Audio::ClickRelease() {
    Play(releaseTone_);
}

void Audio::Tick(float position) {
    const int index = static_cast<int>(std::lround(std::clamp(position, 0.0f, 1.0f) * 20));
    Play(ticks_[index]);
}

void Audio::CentreMark() {
    Play(centreMark_);
}

void Audio::EndStop() {
    Play(endStop_);
}
