// cpoldcrit_core.h - portable logic for "Old sword crit": the crit heuristic and the bob curve.
// No OS calls, so it is unit-tested on any machine (tests/oldcrit_test.cpp).
#pragma once
#include <cmath>
#include <cstdint>

namespace cpcrit {

// ---- tunables --------------------------------------------------------------------------
constexpr double kBobSeconds   = 0.200;  // whole animation (down, then back up)
constexpr double kDownFraction = 0.35;   // share of the time spent going down
constexpr float  kAmplitude    = 0.030f; // peak shift as a fraction of the screen height
constexpr double kFallMinSec   = 0.20;   // a jump is "falling" from here ...
constexpr double kFallMaxSec   = 0.75;   // ... until here (after the Space press)
constexpr double kCooldownSec  = 0.35;   // at most one bob per attack cooldown

// Bob curve. p = 0..1 through the animation. Returns 0..1 (1 = lowest point).
inline float bobCurve(double p) {
    if (p <= 0.0 || p >= 1.0) return 0.0f;
    if (p < kDownFraction) {                       // fast ease-out down
        double u = p / kDownFraction;
        return (float)(1.0 - (1.0 - u) * (1.0 - u));
    }
    double u = (p - kDownFraction) / (1.0 - kDownFraction);   // smooth ease-in-out back up
    return (float)(1.0 - u * u * (3.0 - 2.0 * u));
}

// Crit detector. Feed it input edges with a timestamp in seconds; ask it what to do.
// It does NOT know whether a hit landed (that needs Bedrock's memory): it fires on an
// attack click that happens while the player is falling after a jump, which is exactly
// the situation where Minecraft awards a critical hit.
struct CritTracker {
    double jumpAt   = -1e9;   // time of the last Space press
    double lastBob  = -1e9;   // time the last bob started
    double bobStart = -1e9;   // start of the animation currently playing

    void onJump(double t)   { jumpAt = t; }
    // returns true if this attack click should start a bob
    bool onAttack(double t) {
        double since = t - jumpAt;
        if (since < kFallMinSec || since > kFallMaxSec) return false;
        if (t - lastBob < kCooldownSec) return false;
        lastBob = bobStart = t;
        return true;
    }
    // current shift as 0..1 of the amplitude
    float level(double now) const {
        double p = (now - bobStart) / kBobSeconds;
        return bobCurve(p);
    }
    bool active(double now) const { return (now - bobStart) < kBobSeconds && now >= bobStart; }
};

// pixels to shift the frame down for a screen of height h
inline int shiftPixels(float level, int h) {
    int px = (int)std::lround(level * kAmplitude * (float)h);
    if (px < 0) px = 0;
    if (px > h / 4) px = h / 4;
    return px;
}

} // namespace cpcrit
