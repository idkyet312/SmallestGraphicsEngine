#ifndef VIRTUAL_INPUT_H
#define VIRTUAL_INPUT_H

// Shared state for the on-screen movement pad, so the scene can be driven with
// the mouse alone (useful when testing without touching the keyboard).
// RenderUI sets the flags while a button is held; ProcessInput consumes them
// once per frame and clears them.
struct VirtualInput {
    bool forward = false;
    bool back    = false;
    bool left    = false;
    bool right   = false;
    bool down    = false;
    bool jump    = false;
    bool shoot   = false;

    // Look deltas in the same units as raw mouse movement, scaled by lookSpeed
    // and deltaTime when applied.
    float lookX = 0.0f;
    float lookY = 0.0f;

    float lookSpeed = 250.0f;
    bool  showPad   = true;
};

extern VirtualInput virtualInput;

#endif // VIRTUAL_INPUT_H
