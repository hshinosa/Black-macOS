// The host: SDL's window and event loop, the settings and folders, the app's lifecycle; the game is described by
// app::kGame (src/common/game_info.cpp). It also implements the app:: interfaces the runtime and the game call, and the
// window's own state the renderer asks for.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

std::thread::id g_main_thread;
SDL_Window* g_window = nullptr;
std::atomic<bool> g_foreground{true};
std::atomic<uint32_t> g_surface_epoch{0};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;

// jobs the main thread runs for the renderer: a Cocoa window and its Metal view live on the main thread
std::mutex g_jobs_lock;
std::condition_variable g_jobs_cv;
std::deque<std::function<void()>> g_jobs;
uint64_t g_jobs_posted = 0, g_jobs_done = 0;

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (on && !g_foreground) g_surface_epoch++;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "in front" : "in the background");
}

// SDL's display mode: the window is not up yet when the settings load
app::Mode ScreenMode() {
    if (g_window) {
        int w = 0, h = 0;
        host::WindowPixels(w, h);
        if (w > 0 && h > 0) return {uint32_t(w), uint32_t(h)};
    }
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return {1920, 1080};
    uint32_t w = uint32_t(m->w * m->pixel_density), h = uint32_t(m->h * m->pixel_density);
    return {std::max(w, h), std::min(w, h)};
}

float RefreshRate() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() { return g_foreground; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load(); });
}

void WindowPixels(int& w, int& h) {
    w = h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

bool MainThreadIsCurrent() { return std::this_thread::get_id() == g_main_thread; }

void OnMainThread(const std::function<void()>& job) {
    // the main thread asking for its own job runs it here
    if (std::this_thread::get_id() == g_main_thread) {
        job();
        return;
    }
    uint64_t mine;
    {
        std::lock_guard lock(g_jobs_lock);
        g_jobs.push_back(job);
        mine = ++g_jobs_posted;
        g_jobs_cv.notify_all();
    }
    std::unique_lock lock(g_jobs_lock);
    g_jobs_cv.wait(lock, [mine] { return g_jobs_done >= mine; });
}

void PostMainThread(const std::function<void()>& job) {
    if (std::this_thread::get_id() == g_main_thread) {
        job();
        return;
    }
    {
        std::lock_guard lock(g_jobs_lock);
        g_jobs.push_back(job);
    }
    g_jobs_cv.notify_all();
}

void RunMainThreadJobs() {
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard lock(g_jobs_lock);
            if (g_jobs.empty()) return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
        {
            std::lock_guard lock(g_jobs_lock);
            g_jobs_done++;
            g_jobs_cv.notify_all();
        }
    }
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
// the defaults a settings file was written under: 2 has the frame rate unlimited and vsync off, which a file from
// before takes once, its player's own choices kept from then on
constexpr int kDefaults = 2;

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

std::filesystem::path SettingsFile() { return File(); }

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"defaults", int64_t(kDefaults)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

// a Mac opens in its own window at 1280x720, vsync on and the frame rate at the display's rate: the console's frame is
// drawn at whatever the window's height asks for
constexpr uint32_t kFirstRunRenderPercent = 100;
constexpr uint32_t kFirstRunWidth = 1280;
constexpr uint32_t kFirstRunHeight = 720;

void DesktopFrameDefaults(Settings& s) {
    s.display_mode = DisplayMode::Windowed;
    s.width = kFirstRunWidth;
    s.height = kFirstRunHeight;
    s.fps_cap = kFpsCapRefresh;
    s.vsync = true;
}

}  // namespace

Settings DefaultSettings() {
    Settings s;
    s.render_percent = kFirstRunRenderPercent;
    DesktopFrameDefaults(s);
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, BLACK_COMMIT);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("Device: macOS %s, %s", "15", SDL_GetPlatform());
    LOGR("CPU: %d threads", SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

// the display settings in the game's options: fullscreen, a borderless window the size of the screen, or the size the
// resolution row names. Any thread: the window itself is AppKit's and changes on the main thread
void ApplyDisplay() {
    if (!host::MainThreadIsCurrent()) {
        host::PostMainThread([] { ApplyDisplay(); });
        return;
    }
    static std::mutex lock;
    std::lock_guard held(lock);
    if (!g_window) return;
    const Settings& s = Get();
    if (s.display_mode == DisplayMode::Fullscreen) {
        if (!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowFullscreen(g_window, true);
        return;
    }
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(g_window, false);
    SDL_SetWindowBordered(g_window, s.display_mode == DisplayMode::Windowed);
    int w = int(s.width), h = int(s.height);
    if (!w || !h) {
        // the display's own size, less the menu bar and the dock for a bordered window
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
        if (!m) return;
        SDL_Rect usable{};
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &usable);
        int max_w = usable.w > 0 ? usable.w : m->w, max_h = usable.h > 0 ? usable.h : m->h;
        if (s.display_mode == DisplayMode::Borderless) {
            w = m->w, h = m->h;
        } else {
            // the largest of the settings' sizes that fits
            constexpr int kSizes[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}};
            w = 1280, h = 720;
            for (auto& size : kSizes)
                if (size[0] <= max_w && size[1] <= max_h) {
                    w = size[0], h = size[1];
                    break;
                }
        }
    }
    SDL_SetWindowSize(g_window, w, h);
    if (s.display_mode == DisplayMode::Borderless) SDL_SetWindowPosition(g_window, 0, 0);
    else SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

Mode Desktop() { return ScreenMode(); }

// the renderer's target: the window's size in pixels, which on a Retina display is twice its size in points
Mode Output() { return Desktop(); }

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

// SSAA renders twice as wide and high, the present averaging each 2x2 back to a pixel
float RenderScale(uint32_t guest_height) {
    float ssaa = game::Opt().anti_aliasing == 2 ? 2.0f : 1.0f;
    float h = Output().height * (dynres::Percent() / 100.0f) * ssaa;
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

// ~/Library/Application Support/Black: the settings, logs, saves and the game's data. Always here, never in the
// bundle, which is read-only in /Applications and replaced by the next build
std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        const char* home = std::getenv("HOME");
        std::filesystem::path p = std::filesystem::path(home ? home : ".") / "Library/Application Support/Black";
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return dir;
}

int Instance() { return 0; }

void* Window() { return g_window; }

// the game window is the only window: no splash and no relaunch into another executable
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

// the runtime's ImGui lock: this build draws no ImGui, but the interface asks for it
std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            logx::Flush();
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            SetForeground(false);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            SetForeground(true);
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: the system is low on memory");
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED: {
            // the renderer makes its swap chain again from the window's size; the size is remembered for the next run
            app::Settings& s = app::Get();
            if (s.display_mode != app::DisplayMode::Fullscreen) {
                int w = 0, h = 0;
                SDL_GetWindowSize(g_window, &w, &h);
                if (w > 0 && h > 0) {
                    s.width = uint32_t(w);
                    s.height = uint32_t(h);
                }
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // F11 toggles the display mode, Esc in the front end / Alt+Enter elsewhere is the game's own
            if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
                app::Settings& s = app::Get();
                s.display_mode = s.display_mode == app::DisplayMode::Fullscreen ? app::DisplayMode::Windowed
                                                                              : app::DisplayMode::Fullscreen;
                SDL_SetWindowFullscreen(g_window, s.display_mode == app::DisplayMode::Fullscreen);
            }
            break;
        default:
            break;
    }
    input::OnEvent(e);
}

}  // namespace

int main(int, char**) {
    g_main_thread = std::this_thread::get_id();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOGE("app: SDL_Init: %s", SDL_GetError());
        return 1;
    }
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    // a first run writes its settings out, so the file is there to edit before the game ever changes one
    if (!std::filesystem::exists(app::SettingsFile())) app::Save();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    // the window: fullscreen on the display, or the size the settings ask for, which a resize writes back
    const app::Settings& s = app::Get();
    std::string title = Utf8(app::kGame.title);
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    int ww = int(s.width), wh = int(s.height);
    if (s.display_mode == app::DisplayMode::Fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
        ww = wh = 0;
    } else {
        if (s.display_mode == app::DisplayMode::Borderless) flags |= SDL_WINDOW_BORDERLESS;
        if (!ww || !wh) {
            app::Mode m = app::Desktop();
            ww = int(m.width) * 3 / 4, wh = int(m.height) * 3 / 4;
        }
    }
    g_window = SDL_CreateWindow(title.c_str(), ww, wh, flags);
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        return 1;
    }

    app::GameFiles game;
    if (!app::FindGame(game)) {
        logx::Flush();
        return 1;
    }

    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 "The game failed to start, see the log in the logs folder.", g_window);
        return 1;
    }

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        input::Pump();
        host::RunMainThreadJobs();
    }
}
