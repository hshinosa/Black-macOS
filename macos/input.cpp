// The keyboard and the mouse: the game's bindings, the menu cursor, the mouse look and the name entry, which is
// src/common/mouse_keyboard.h. SDL's keys arrive as the virtual keys of common/keys.h; a pad takes port 0 over from
// the keys the moment it is used (gamepads.cpp).
#include "common/mouse_keyboard.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/aspect.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/options.h"
#include "common/translations.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<uint64_t> g_take_ms{0};     // TakeMouse, asked every tick in play
std::atomic<uint64_t> g_capture_ms{0};  // PressedKey
std::atomic<bool> g_settle{false};
std::atomic<uint64_t> g_confirm_until{0};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_keyboard{true};  // the keyboard and mouse are the way in; a pad takes over when it moves
std::atomic<bool> g_text{false};
std::atomic<bool> g_text_shown{false};  // the on-screen keyboard, main thread
std::mutex g_chars_lock;
std::u16string g_chars;  // typed, not yet taken
constexpr size_t kMaxChars = 64;
std::atomic<int> g_wheel{0};
constexpr uint64_t kPlayMs = 250;
constexpr uint64_t kTapMs = 80;
constexpr uint64_t kCaptureMs = 250;  // PressedKey asked this recently: a key capture is up
// The wheel binds as two keys, codes no key has. A notch holds what is bound to it for one tap.
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down
// the keys and mouse buttons held, by virtual key
std::array<std::atomic<bool>, 256> g_down{};

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys,
// 20-23 the look keys
struct Target {
    Action action;
    int button;
};
constexpr int kHostKey = -2;
constexpr int kWalk = -3;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kFirstLookKey = 20;
constexpr int kStickMax = 32767;
// the game's default controller layout, as src/common/mouse_keyboard.cpp has it
constexpr Target kActions[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, 16},
    {{"move_back", "MOVE BACK", "S"}, 17},
    {{"strafe_left", "STRAFE LEFT", "A"}, 18},
    {{"strafe_right", "STRAFE RIGHT", "D"}, 19},
    {{"walk", "WALK", "Alt"}, kWalk},
    {{"crouch", "CROUCH", "C"}, 14},
    {{"fire", "FIRE", "LMB"}, 7},
    {{"aim", "AIM", "RMB"}, 15},
    {{"melee", "MELEE", "V"}, 1},
    {{"reload", "RELOAD", "R"}, 0},
    {{"interact", "INTERACT", "F"}, 2},
    {{"grenade", "THROW GRENADE", "G"}, 6},
    {{"weapon_up", "CHANGE WEAPON UP", "E,Wheel Up"}, 11},
    {{"weapon_down", "CHANGE WEAPON DOWN", "Q,Wheel Down"}, 10},
    {{"suppressor", "TOGGLE SUPPRESSOR", "T"}, 3},
    {{"firing_mode", "FIRING MODE / SCOPE ZOOM", "X"}, 8},
    {{"health_pack", "HEALTH PACK", "H"}, 9},
    {{"next_track", "NEXT MUSIC TRACK", "N"}, 5},
    {{"back", "OBJECTIVES", "Tab"}, 13},
    {{"pause", "PAUSE MENU", "Escape"}, 12},
    {{"hide_hud", "TOGGLE HUD", ""}, kHostKey},
    {{"look_up", "LOOK UP", "Up"}, 20},
    {{"look_down", "LOOK DOWN", "Down"}, 21},
    {{"look_left", "LOOK LEFT", "Left"}, 22},
    {{"look_right", "LOOK RIGHT", "Right"}, 23},
};
constexpr size_t kCount = std::size(kActions);
constexpr size_t HostAction(std::string_view id) {
    size_t i = 0;
    while (kActions[i].action.id != id) i++;
    return i;
}
constexpr size_t kHudAction = HostAction("hide_hud");
struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// not bindable: in the game's menus the arrows are the dpad, Enter accepts, Backspace backs out
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (strcasecmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

std::string Join(const std::vector<int>& vks) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += NameOf(vk);
    }
    return s;
}

bool Active() { return host::Foreground(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
    return vk > 0 && vk < 256 && g_down[size_t(vk)];
}
bool Mouse(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || Wheel(vk);
}
bool Play() { return NowMs() - g_take_ms < kPlayMs; }
bool GameDown(int vk) { return Down(vk) && !(g_menu && vk == VK_DELETE); }
bool KeyCaptureUp() { return NowMs() - g_capture_ms <= kCaptureMs; }

bool HostKeyBound(size_t action, int vk) {
    if (KeyCaptureUp()) return false;
    std::lock_guard lock(g_bind_lock);
    const auto& b = g_binds[action];
    return std::find(b.begin(), b.end(), vk) != b.end();
}

// keys held from before the game had the input wait for release; true once none is held
bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

struct Held {
    bool used = false;
    bool walk = false;
    int stick[8] = {};  // move forward back left right, look up down left right
};

Held PressActions(const Binds& binds, bool play, Pad& p) {
    Held held;
    auto pressed = [play](int vk) { return GameDown(vk) && (play || !Wheel(vk)); };
    static bool pause_held;
    static bool pause_backs;
    bool pause = false;
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (b == kHostKey || !std::any_of(binds[i].begin(), binds[i].end(), pressed)) continue;
        held.used = true;
        if (g_text && b != kStart) continue;
        if (b == kWalk) {
            held.walk = true;
        } else if (b >= kFirstLookKey && !play) {
            continue;
        } else if (b >= kFirstMoveKey) {
            held.stick[b - kFirstMoveKey] = 1;
        } else if (b < kFirstButtonBit) {
            p.analog[b] = 0xFF;
        } else if (b == kStart) {
            if (!pause_held) pause_backs = g_menu;
            pause = true;
            if (!pause_backs) p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
            else if (g_menu) p.analog[1] = 0xFF;
        } else {
            p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
        }
    }
    pause_held = pause;
    return held;
}

// SDL's scancodes as the virtual keys (common/keys.h) the bindings name, by position as on a US keyboard
int VkOf(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        default: return 0;
    }
}

int VkOfButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
        case SDL_BUTTON_X1: return VK_XBUTTON1;
        case SDL_BUTTON_X2: return VK_XBUTTON2;
        default: return 0;
    }
}

}  // namespace

void ReleaseCursor() { g_settle = true; }

bool TakeMouse(float& dx, float& dy) {
    uint64_t now = NowMs();
    bool stale = now - g_take_ms.exchange(now) >= kPlayMs;
    int x = g_dx.exchange(0);
    int y = g_dy.exchange(0);
    if ((!x && !y) || stale || !Active()) return false;
    const game::Options& o = game::Opt();
    dx = float(x) * o.mouse_sensitivity;
    dy = float(y) * o.mouse_sensitivity;
    if (o.invert_mouse) dy = -dy;
    return true;
}

void Keyboard(Pad& p) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    Binds binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) return;
    bool play = Play();
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, play, p);
    bool confirm = !play && !g_text && (Down(VK_RETURN) || Down(VK_SPACE));
    if (confirm || NowMs() < g_confirm_until) p.analog[0] = 0xFF;
    if ((!play && !g_text && Down(VK_BACK)) || (g_menu && Down(VK_RBUTTON))) p.analog[1] = 0xFF;
    if (!play) {
        for (int i = 0; i < 4; i++) p.buttons |= uint16_t(Down(kFixed[i]) << i);
    }
    const int* stick = held.stick;
    float speed = held.walk ? float(game::Opt().walk_speed) / 100.0f : 1.0f;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(float(x) * speed * float(kStickMax));
    if (int y = stick[0] - stick[1]) p.ly = int16_t(float(y) * speed * float(kStickMax));
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) g_keyboard = true;
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) g_keyboard = false;
}

void SetMenu(bool menu) { g_menu = menu; }

bool KeyboardActive() { return g_keyboard; }

void TapConfirm() { g_confirm_until = NowMs() + kTapMs; }

void SetTextEntry(bool on) {
    if (g_text.exchange(on) == on) return;
    std::lock_guard lock(g_chars_lock);
    g_chars.clear();
}

void OnChar(unsigned c) {
    if (!g_text || !Active() || c > 0xFFFF) return;
    std::lock_guard lock(g_chars_lock);
    if (g_chars.size() < kMaxChars) g_chars += char16_t(c);
}

std::u16string TakeChars() {
    std::lock_guard lock(g_chars_lock);
    return std::exchange(g_chars, {});
}

// a notch a call: notches that came together (a swipe's) step one menu tick each
int TakeWheel() {
    int w = g_wheel.load();
    while (w && !g_wheel.compare_exchange_weak(w, w > 0 ? w - 1 : w + 1)) {
    }
    return w > 0 ? 1 : w < 0 ? -1 : 0;
}

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action]) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    if (kActions[action].button == kHostKey && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        if (!slots[0] && !slots[1] && action == kHudAction) return;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void Unbind(size_t action) {
    if (action >= kCount || action == kHudAction) return;
    {
        std::lock_guard lock(g_bind_lock);
        g_binds[action].clear();
        game::Opt().keys[kActions[action].action.id].clear();
    }
    app::Save();
}

bool TakeDeletePress() {
    static bool held = false;
    bool down = Down(VK_DELETE);
    bool fresh = down && !held;
    held = down;
    return fresh && Active();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
}

std::string ButtonKey(int button) {
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].button != button || g_binds[i].empty()) continue;
        std::string name = g_binds[i][0] == VK_ESCAPE ? "Esc" : NameOf(g_binds[i][0]);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return name;
    }
    return {};
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

bool HudKey(int vk) { return HostKeyBound(kHudAction, vk); }

}  // namespace input
namespace input {

namespace {

// where the pointer is, for the game's menus
std::mutex g_cursor_lock;
float g_cursor_x = 0, g_cursor_y = 0;
bool g_cursor_down = false;  // the left button, for a menu row held
std::atomic<uint64_t> g_cursor_ms{0};

}  // namespace

void AddLook(float dx, float dy) {
    // the fraction of a count carried to the next reading: slow turns add up
    static float rx = 0, ry = 0;
    rx += dx;
    ry += dy;
    int ix = int(rx), iy = int(ry);
    rx -= float(ix);
    ry -= float(iy);
    g_dx += ix;
    g_dy += iy;
}

namespace {

bool PadPlugged() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) SDL_free(ids);
    return n > 0;
}

void KeyEvent(const SDL_KeyboardEvent& k) {
    int vk = VkOf(k.scancode);
    if (!vk) return;
    g_down[size_t(vk)] = k.down;
    if (!k.down) return;
    g_keyboard = true;
    if (!k.repeat && HudKey(vk)) game::g_hide_hud = !game::g_hide_hud;
    // a name being typed takes Backspace and Enter as WM_CHAR has them
    if (vk == VK_BACK) OnChar('\b');
    if (vk == VK_RETURN) OnChar('\r');
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(e.key);
            return true;
        case SDL_EVENT_TEXT_INPUT:
            for (const char* c = e.text.text; c && *c; c++)
                if (uint8_t(*c) < 0x80 && *c >= ' ') OnChar(unsigned(*c));
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            g_keyboard = true;
            if (Active() && Play()) {
                // a turn in play, in mouse counts, the fractions kept
                AddLook(e.motion.xrel, e.motion.yrel);
            } else {
                std::lock_guard lock(g_cursor_lock);
                g_cursor_x = e.motion.x;
                g_cursor_y = e.motion.y;
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (int vk = VkOfButton(e.button.button)) g_down[size_t(vk)] = e.button.down;
            if (e.button.button == SDL_BUTTON_LEFT) {
                std::lock_guard lock(g_cursor_lock);
                g_cursor_down = e.button.down;
            }
            g_keyboard = true;
            g_cursor_ms = NowMs();
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y != 0) {
                int notch = e.wheel.y > 0 ? 1 : -1;
                g_wheel_ms[notch < 0] = NowMs();
                if (Active() && !Play() && !KeyCaptureUp()) g_wheel += notch;
            }
            return true;
        default:
            return false;
    }
}

// each turn of the event loop: the mouse is captured in play as a PC game captures it, so the view keeps turning past
// the screen's edge and no pointer floats over the picture
void Pump() {
    SDL_Window* window = host::Window();
    bool capture = g_keyboard && LookActive();
    if (window && capture != SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, capture);
    bool want = g_text && Active();
    if (want == g_text_shown) return;
    g_text_shown = want;
    if (want) SDL_StartTextInput(host::Window());
    else SDL_StopTextInput(host::Window());
}

bool LookActive() { return Active() && Play() && !g_menu; }

bool Firing() { return Down(VK_LBUTTON); }

void PadUsed() {}

bool Shown() { return !PadPlugged(); }

bool CursorShown() { return g_menu && g_keyboard; }

bool Cursor(float& x, float& y, bool& down) {
    if (!Active() || !CursorShown()) return false;
    int w, h;
    host::WindowPixels(w, h);
    if (w <= 0 || h <= 0) return false;
    float px, py;
    bool held;
    {
        std::lock_guard lock(g_cursor_lock);
        px = g_cursor_x;
        py = g_cursor_y;
        held = g_cursor_down;
    }
    // the frame is shown in the middle of the window
    float aspect = game::Aspect();
    float vw = std::min(float(w), float(h) * aspect);
    float vh = vw / aspect;
    x = (px - (float(w) - vw) / 2) / vw;
    y = (py - (float(h) - vh) / 2) / vh;
    down = held || Down(VK_LBUTTON);
    return true;
}

bool LeftButton() { return Active() && !g_settle && Down(VK_LBUTTON); }

}  // namespace input
