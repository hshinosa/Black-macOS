#pragma once
// What the host's parts share: the window, the app's lifecycle and the event routing
#include <cstdint>
#include <functional>
#include <vector>

union SDL_Event;
struct SDL_Window;

namespace input {
struct Pad;
}

namespace host {

SDL_Window* Window();
// the app is in front with a window to draw to; the renderer and the game wait while it is not
bool Foreground();
// counts the native windows the app was given: the renderer makes its surface again when this changes
uint32_t SurfaceEpoch();
// the game thread, before a present: returns once the app is in front again
void WaitForeground();
// the window's size in pixels
void WindowPixels(int& w, int& h);
// a job on the main thread, which runs it on its next turn and the caller waits: macOS makes windows and their Metal
// views there only, and the renderer's surface is one of them
void OnMainThread(const std::function<void()>& job);
// the same, without waiting: for a change nothing reads back, like a window's size
void PostMainThread(const std::function<void()>& job);
// main thread, each turn of the event loop: the jobs asked for
void RunMainThreadJobs();
// true on the main thread: the jobs run at once there
bool MainThreadIsCurrent();

}  // namespace host

// the render resolution following the frame rate (dynamic_resolution.cpp)
namespace dynres {
uint32_t Percent();  // the render resolution in use, percent of the screen's: RENDER RESOLUTION's while it is off
void Frame();        // vulkan thread, each present
}  // namespace dynres

// controllers through SDL (gamepads.cpp): the main thread hands over their plug events
namespace input {
void OnPadEvent(const SDL_Event& e);
}

// the keyboard and the mouse (input.cpp)
namespace input {
// main thread, every input event; true when the game's own handling took it
bool OnEvent(const SDL_Event& e);
// main thread, each turn of the event loop: the mouse is captured in play, and a name being typed takes text input
void Pump();
// the view turns as the mouse turns it, in mouse counts, while the game is in play
void AddLook(float dx, float dy);
bool LookActive();  // in play, in front, no menu or panel up
bool Firing();      // the left button, or a controller's right trigger, is held
// a controller was used: the game's hints name its buttons, not the keys
void PadUsed();
// a controller is plugged in: its buttons are the prompts
bool Shown();
}  // namespace input
