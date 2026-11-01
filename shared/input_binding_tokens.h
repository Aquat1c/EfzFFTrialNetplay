#pragma once

#include <string>
#include <string_view>

namespace trialnet::input
{

enum class Source
{
    Keyboard = 0,
    Controller = 1,
};

inline std::wstring NormalizeAsciiToken(const std::wstring_view raw)
{
    std::wstring token;
    token.reserve(raw.size());
    for (wchar_t ch : raw)
    {
        if (ch >= L'a' && ch <= L'z')
        {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        if (ch == L'-' || ch == L' ')
        {
            ch = L'_';
        }
        token.push_back(ch);
    }
    return token;
}

inline Source ParseSource(const std::wstring_view raw)
{
    const std::wstring token = NormalizeAsciiToken(raw);
    if (token == L"CONTROLLER" || token == L"GAMEPAD" || token == L"PAD"
        || token == L"JOYSTICK" || token == L"XINPUT")
    {
        return Source::Controller;
    }
    return Source::Keyboard;
}

inline const wchar_t* SourceIniName(const Source source)
{
    return source == Source::Controller ? L"controller" : L"keyboard";
}

inline const char* SourceDisplayName(const Source source)
{
    return source == Source::Controller ? "Controller" : "Keyboard";
}

enum class StandardButton
{
    Invalid = -1,
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    RightPaddle1,
    LeftPaddle1,
    RightPaddle2,
    LeftPaddle2,
    Touchpad,
    Misc2,
    Misc3,
    Misc4,
    Misc5,
    Misc6,
};

inline StandardButton ParseStandardButton(const std::wstring_view raw)
{
    const std::wstring token = NormalizeAsciiToken(raw);
    if (token == L"A" || token == L"CROSS" || token == L"SOUTH") return StandardButton::South;
    if (token == L"B" || token == L"CIRCLE" || token == L"EAST") return StandardButton::East;
    if (token == L"X" || token == L"SQUARE" || token == L"WEST") return StandardButton::West;
    if (token == L"Y" || token == L"TRIANGLE" || token == L"NORTH") return StandardButton::North;
    if (token == L"BACK" || token == L"SELECT" || token == L"SHARE") return StandardButton::Back;
    if (token == L"GUIDE" || token == L"HOME") return StandardButton::Guide;
    if (token == L"START" || token == L"OPTIONS") return StandardButton::Start;
    if (token == L"LEFT_STICK" || token == L"LEFTSTICK" || token == L"L3") return StandardButton::LeftStick;
    if (token == L"RIGHT_STICK" || token == L"RIGHTSTICK" || token == L"R3") return StandardButton::RightStick;
    if (token == L"LEFT_SHOULDER" || token == L"LEFTSHOULDER" || token == L"LB" || token == L"L1") return StandardButton::LeftShoulder;
    if (token == L"RIGHT_SHOULDER" || token == L"RIGHTSHOULDER" || token == L"RB" || token == L"R1") return StandardButton::RightShoulder;
    if (token == L"DPAD_UP" || token == L"D_PAD_UP" || token == L"DPUP") return StandardButton::DpadUp;
    if (token == L"DPAD_DOWN" || token == L"D_PAD_DOWN" || token == L"DPDOWN") return StandardButton::DpadDown;
    if (token == L"DPAD_LEFT" || token == L"D_PAD_LEFT" || token == L"DPLEFT") return StandardButton::DpadLeft;
    if (token == L"DPAD_RIGHT" || token == L"D_PAD_RIGHT" || token == L"DPRIGHT") return StandardButton::DpadRight;
    if (token == L"MISC1" || token == L"MISC_1") return StandardButton::Misc1;
    if (token == L"RIGHT_PADDLE1" || token == L"RIGHTPADDLE1" || token == L"PADDLE1") return StandardButton::RightPaddle1;
    if (token == L"LEFT_PADDLE1" || token == L"LEFTPADDLE1" || token == L"PADDLE3") return StandardButton::LeftPaddle1;
    if (token == L"RIGHT_PADDLE2" || token == L"RIGHTPADDLE2" || token == L"PADDLE2") return StandardButton::RightPaddle2;
    if (token == L"LEFT_PADDLE2" || token == L"LEFTPADDLE2" || token == L"PADDLE4") return StandardButton::LeftPaddle2;
    if (token == L"TOUCHPAD") return StandardButton::Touchpad;
    if (token == L"MISC2" || token == L"MISC_2") return StandardButton::Misc2;
    if (token == L"MISC3" || token == L"MISC_3") return StandardButton::Misc3;
    if (token == L"MISC4" || token == L"MISC_4") return StandardButton::Misc4;
    if (token == L"MISC5" || token == L"MISC_5") return StandardButton::Misc5;
    if (token == L"MISC6" || token == L"MISC_6") return StandardButton::Misc6;
    return StandardButton::Invalid;
}

inline const wchar_t* CanonicalButtonToken(const StandardButton button)
{
    switch (button)
    {
    case StandardButton::South: return L"SOUTH";
    case StandardButton::East: return L"EAST";
    case StandardButton::West: return L"WEST";
    case StandardButton::North: return L"NORTH";
    case StandardButton::Back: return L"BACK";
    case StandardButton::Guide: return L"GUIDE";
    case StandardButton::Start: return L"START";
    case StandardButton::LeftStick: return L"LEFT_STICK";
    case StandardButton::RightStick: return L"RIGHT_STICK";
    case StandardButton::LeftShoulder: return L"LEFT_SHOULDER";
    case StandardButton::RightShoulder: return L"RIGHT_SHOULDER";
    case StandardButton::DpadUp: return L"DPAD_UP";
    case StandardButton::DpadDown: return L"DPAD_DOWN";
    case StandardButton::DpadLeft: return L"DPAD_LEFT";
    case StandardButton::DpadRight: return L"DPAD_RIGHT";
    case StandardButton::Misc1: return L"MISC1";
    case StandardButton::RightPaddle1: return L"RIGHT_PADDLE1";
    case StandardButton::LeftPaddle1: return L"LEFT_PADDLE1";
    case StandardButton::RightPaddle2: return L"RIGHT_PADDLE2";
    case StandardButton::LeftPaddle2: return L"LEFT_PADDLE2";
    case StandardButton::Touchpad: return L"TOUCHPAD";
    case StandardButton::Misc2: return L"MISC2";
    case StandardButton::Misc3: return L"MISC3";
    case StandardButton::Misc4: return L"MISC4";
    case StandardButton::Misc5: return L"MISC5";
    case StandardButton::Misc6: return L"MISC6";
    default: return L"";
    }
}

inline std::wstring CanonicalizePadToken(const std::wstring_view raw)
{
    const StandardButton button = ParseStandardButton(raw);
    return button == StandardButton::Invalid
        ? NormalizeAsciiToken(raw)
        : std::wstring(CanonicalButtonToken(button));
}

}
