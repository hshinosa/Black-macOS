#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// the settings rows the game's own options menu shows, and the frame pacing
namespace app {

inline constexpr uint32_t kFpsCapRefresh = 1;  // Settings::fps_cap: just under the display's refresh rate
void PaceFrame();  // app::Pace, with kFpsCapRefresh; game thread, after the present

// the panel's rows, for the game's own options menu
namespace rows {

enum class Kind { Heading, Value, Bind };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
// 0 graphics, 1 controller, 2 general, 3 mouse and keyboard, 4 credits (the title menu's CREDITS)
const Row* Page(int page, size_t* count);
// the sticks' rows, on the game's control setup page and on the panel's controller page
const Row* Controller(size_t* count);
std::string Value(const Row& row, int step = 0);  // what the row shows, stepped first when step is not zero
void Defaults(int page);
bool Restorable(int page);  // the page ends with RESTORE DEFAULTS in the game's options
// false for a row shown greyed that takes no change: HDR output without an HDR display
bool Enabled(const Row& row);
// a row whose step opens something rather than changing a value: no arrows beside it in the game's options
bool Opens(const Row& row);

}  // namespace rows

}  // namespace app
