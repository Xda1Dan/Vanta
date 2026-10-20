//
//  input.hpp
//  vanta-base
//
//  Synthetic keyboard and mouse input. The UNC surface is written against
//  Windows virtual-key codes, so the numbers scripts pass in are translated to
//  macOS virtual keycodes before CGEvent sees them; a one-character string is
//  accepted too, because that is what most scripts actually pass.
//
//  Events are posted to the HID event tap rather than to Roblox directly, which
//  is what makes the game react to them at all - it reads the window server's
//  input queue, not its own.
//

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <ApplicationServices/ApplicationServices.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace input {

    // The reference gates every one of these on Roblox being the frontmost
    // app, and it is right to: a script that presses W while the user has
    // alt-tabbed away would otherwise type into whatever they are actually
    // using. Same NSApplication check isrbxactive answers with.
    inline bool active()
    {
        Class applicationClass = objc_getClass("NSApplication");
        if (!applicationClass)
            return false;

        using SendId = id (*)(id, SEL);
        using SendBool = signed char (*)(id, SEL);
        const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
        const auto sendBool = reinterpret_cast<SendBool>(objc_msgSend);

        id application = sendId(reinterpret_cast<id>(applicationClass), sel_registerName("sharedApplication"));
        return application && sendBool(application, sel_registerName("isActive"));
    }

    // Windows VK -> macOS kVK. Only the keys a script can plausibly name; a
    // code with no macOS equivalent is left out and reported rather than
    // silently pressing something else.
    inline const std::unordered_map<int, CGKeyCode>& keymap()
    {
        static const std::unordered_map<int, CGKeyCode> map = {
            {0x08, 51}, {0x09, 48}, {0x0D, 36}, {0x10, 56}, {0x11, 59},
            {0x12, 58}, {0x14, 57}, {0x1B, 53}, {0x20, 49},
            // arrows, navigation
            {0x21, 116}, {0x22, 121}, {0x23, 119}, {0x24, 115},
            {0x25, 123}, {0x26, 126}, {0x27, 124}, {0x28, 125},
            {0x2E, 117},
            // digits
            {0x30, 29}, {0x31, 18}, {0x32, 19}, {0x33, 20}, {0x34, 21},
            {0x35, 23}, {0x36, 22}, {0x37, 26}, {0x38, 28}, {0x39, 25},
            // letters
            {0x41, 0},  {0x42, 11}, {0x43, 8},  {0x44, 2},  {0x45, 14},
            {0x46, 3},  {0x47, 5},  {0x48, 4},  {0x49, 34}, {0x4A, 38},
            {0x4B, 40}, {0x4C, 37}, {0x4D, 46}, {0x4E, 45}, {0x4F, 31},
            {0x50, 35}, {0x51, 12}, {0x52, 15}, {0x53, 1},  {0x54, 17},
            {0x55, 32}, {0x56, 9},  {0x57, 13}, {0x58, 7},  {0x59, 16},
            {0x5A, 6},
            // numpad
            {0x60, 82}, {0x61, 83}, {0x62, 84}, {0x63, 85}, {0x64, 86},
            {0x65, 87}, {0x66, 88}, {0x67, 89}, {0x68, 91}, {0x69, 92},
            {0x6A, 67}, {0x6B, 69}, {0x6D, 78}, {0x6E, 65}, {0x6F, 75},
            // function keys
            {0x70, 122}, {0x71, 120}, {0x72, 99},  {0x73, 118}, {0x74, 96},
            {0x75, 97},  {0x76, 98},  {0x77, 100}, {0x78, 101}, {0x79, 109},
            {0x7A, 103}, {0x7B, 111},
            // punctuation
            {0xBA, 41}, {0xBB, 24}, {0xBC, 43}, {0xBD, 27}, {0xBE, 47},
            {0xBF, 44}, {0xC0, 50}, {0xDB, 33}, {0xDC, 42}, {0xDD, 30},
            {0xDE, 39},
            // modifier variants Roblox reports separately
            {0xA0, 56}, {0xA1, 60}, {0xA2, 59}, {0xA3, 62}, {0xA4, 58},
            {0xA5, 61}, {0x5B, 55}, {0x5C, 54},
        };
        return map;
    }

    // A script may pass "e", "E", or 0x45 for the same key. Strings are folded
    // to their VK first so there is only one lookup path.
    inline bool translate(const std::string& name, int code, CGKeyCode& out)
    {
        int vk = code;
        if (!name.empty()) {
            if (name.size() == 1) {
                unsigned char c = static_cast<unsigned char>(name[0]);
                if (c >= 'a' && c <= 'z')
                    vk = c - 'a' + 0x41;
                else if (c >= 'A' && c <= 'Z')
                    vk = c;
                else if (c >= '0' && c <= '9')
                    vk = c;
                else
                    return false;
            } else if (name == "space" || name == "Space") {
                vk = 0x20;
            } else if (name == "enter" || name == "return" || name == "Return") {
                vk = 0x0D;
            } else if (name == "escape" || name == "Escape") {
                vk = 0x1B;
            } else if (name == "tab" || name == "Tab") {
                vk = 0x09;
            } else if (name == "backspace" || name == "Backspace") {
                vk = 0x08;
            } else {
                return false;
            }
        }

        const auto& map = keymap();
        auto found = map.find(vk);
        if (found == map.end())
            return false;
        out = found->second;
        return true;
    }

    inline void postKey(CGKeyCode key, bool down)
    {
        if (!active())
            return;
        CGEventRef event = CGEventCreateKeyboardEvent(nullptr, key, down);
        if (!event)
            return;
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    inline CGPoint cursor()
    {
        CGEventRef probe = CGEventCreate(nullptr);
        CGPoint point = probe ? CGEventGetLocation(probe) : CGPointMake(0, 0);
        if (probe)
            CFRelease(probe);
        return point;
    }

    inline void postMouse(CGEventType type, CGPoint at, CGMouseButton button)
    {
        if (!active())
            return;
        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, at, button);
        if (!event)
            return;
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    inline void mouseButton(bool right, bool down)
    {
        const CGPoint at = cursor();
        const CGMouseButton button = right ? kCGMouseButtonRight : kCGMouseButtonLeft;
        CGEventType type;
        if (right)
            type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        else
            type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        postMouse(type, at, button);
    }

    inline void moveTo(double x, double y)
    {
        postMouse(kCGEventMouseMoved, CGPointMake(x, y), kCGMouseButtonLeft);
    }

    inline void moveBy(double dx, double dy)
    {
        const CGPoint at = cursor();
        moveTo(at.x + dx, at.y + dy);
    }

    // Pixels rather than lines, matching the reference: a script asking to
    // scroll by 5 means five pixels of wheel travel, not five text lines.
    inline void scroll(int amount)
    {
        if (!active())
            return;
        CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, amount);
        if (!event)
            return;
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}
