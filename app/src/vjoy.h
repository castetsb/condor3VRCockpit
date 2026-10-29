#pragma once

#include <string>

// Minimal vJoy client, loading vJoyInterface.dll from the user's vJoy install at runtime.
class VJoy {
public:
    ~VJoy();

    bool Open(const std::wstring& dllPath, unsigned device, std::string& error);
    void Close();

    // axis: "X", "Y", "Z", "RX", "RY", "RZ", "SL0", "SL1". value: 0..1.
    bool SetAxis(const std::string& axis, float value);
    bool SetButton(unsigned button, bool pressed);

private:
    struct Api;
    Api* api_ = nullptr;
    unsigned device_ = 0;
    bool acquired_ = false;
};
