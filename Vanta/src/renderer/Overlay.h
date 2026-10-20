//
//  overlay.hpp
//  vanta-base
//
//  The shape list the Drawing library hands to the native overlay. See
//  overlay.mm for what draws it and why it is a separate window rather than a
//  hook on Roblox's renderer.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace overlay {

    enum class Kind : int {
        Line = 0,
        Text = 1,
        Image = 2,
        Circle = 3,
        Square = 4,
        Quad = 5,
        Triangle = 6,
    };

    struct Point {
        double x = 0;
        double y = 0;
    };

    struct Shape {
        uint64_t id = 0;
        Kind kind = Kind::Line;
        bool visible = false;
        int zindex = 0;
        double transparency = 0;
        double r = 0, g = 0, b = 0;

        // Line uses [0] and [1]; Triangle [0..2]; Quad [0..3]; everything else
        // treats [0] as its position (a Circle's centre, a Square's corner).
        Point points[4];
        Point size;

        double thickness = 1;
        bool filled = false;
        double radius = 0;
        int numsides = 0;

        std::string text;
        double textsize = 13;
        int font = 0;
        bool center = false;
        bool outline = false;
        double outline_r = 0, outline_g = 0, outline_b = 0;

        std::string data;  // raw image bytes for Kind::Image
    };

    // Brings the window up if it is not already. Safe to call from any thread:
    // the AppKit work is bounced to the main queue.
    void start();

    // Whether the overlay came up. False means the Drawing library should keep
    // to its GuiObject fallback rather than silently drawing nothing.
    bool available();

    void set(const Shape& shape);
    void remove(uint64_t id);
    void clear();

    void textBounds(const std::string& text, double size, int font, double* width, double* height);

    // What the overlay is actually showing, for verification: the window's own
    // frame, the game window it is tracking, and a count of pixels the view
    // painted. Screen capture needs a permission this process does not have, so
    // the only way to prove the thing draws is to ask it to draw into a bitmap.
    struct Snapshot {
        bool built = false;
        double x = 0, y = 0, w = 0, h = 0;
        double gamex = 0, gamey = 0, gamew = 0, gameh = 0;
        long painted = 0;
        double r = 0, g = 0, b = 0;
        int shapes = 0;
    };
    Snapshot inspect();

}  // namespace overlay
