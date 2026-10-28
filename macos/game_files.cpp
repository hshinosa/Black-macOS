// The game on macOS: its files or a disc image (.iso) beside the executable, in its assets folder, or in the
// application folder (~/Library/Application Support/Black). The xbe must be the one this build was recompiled from; the
// Europe xbe is turned into the US one by src/common/europe.patch, linked into the framework.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this framework, written by CMake

// src/common/europe.patch, as CMake turns it into europe_patch.cpp: the Mach-O assembler cannot incbin a file into a
// section from inline asm
extern "C" const uint8_t kEuropePatch[];
extern "C" const size_t kEuropePatchSize;  // generated beside the array: no label arithmetic across a Mach-O section

namespace app {

namespace {

namespace fs = std::filesystem;

// the folders the game is looked for in, in order: the executable's own folder (a checkout or a bundle's Resources),
// its assets folder, then the application support folder and its assets folder
std::vector<fs::path> AssetFolders() {
    std::filesystem::path base = SDL_GetBasePath() ? std::filesystem::path(SDL_GetBasePath()) : std::filesystem::path();
    std::vector<fs::path> out;
    if (!base.empty()) {
        out.push_back(base);
        out.push_back(base / "assets");
        // Black.app/Contents/Resources: where a bundle keeps the game's own files
        out.push_back(base / "../Resources");
        out.push_back(base / "../Resources/assets");
    }
    out.push_back(ExeDir());
    out.push_back(ExeDir() / "assets");
    // a run from a terminal looks beside itself too, as the Windows build does
    std::error_code cwd_ec;
    std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    if (!cwd_ec) {
        out.push_back(cwd);
        out.push_back(cwd / "assets");
    }
    std::vector<fs::path> unique;
    std::error_code ec;
    for (auto& p : out) {
        if (!fs::is_directory(p, ec)) continue;
        if (std::find(unique.begin(), unique.end(), p) == unique.end()) unique.push_back(p);
    }
    return unique;
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

constexpr char kEuropeSha1[] = "796ad8369354e48aa15644d4707dd70bd3bc3b4c";

class PatchReader {
public:
    PatchReader(const uint8_t* data, size_t size) : at_(data), end_(data + size) {}
    bool Done() const { return at_ == end_; }
    bool Failed() const { return failed_; }

    uint8_t Byte() {
        if (at_ == end_) {
            failed_ = true;
            return 0;
        }
        return *at_++;
    }

    // 7 bits a byte, low first
    uint64_t Number() {
        uint64_t n = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b = Byte();
            n |= uint64_t(b & 0x7F) << shift;
            if (b < 0x80) return n;
        }
        failed_ = true;
        return 0;
    }

private:
    const uint8_t* at_;
    const uint8_t* end_;
    bool failed_ = false;
};

struct EuropePatch {
    const std::vector<uint8_t>& from;
    PatchReader patch;
    uint64_t read = 0;
    std::vector<uint8_t> out;

    bool ApplyBlock() {
        uint64_t copy = patch.Number();
        if (read > from.size() || copy > from.size() - read) return false;
        size_t start = out.size();
        out.insert(out.end(), from.begin() + read, from.begin() + read + copy);
        uint64_t changes = patch.Number();
        uint64_t at = 0;
        for (uint64_t i = 0; i < changes && !patch.Failed(); i++) {
            at += patch.Number();
            if (at >= copy) return false;
            out[start + at] += patch.Byte();
            at++;
        }
        uint64_t added = patch.Number();
        for (uint64_t i = 0; i < added && !patch.Failed(); i++) out.push_back(patch.Byte());
        return MoveRead(read + copy) && !patch.Failed();
    }

    bool MoveRead(uint64_t from_end) {
        uint64_t move = patch.Number();
        uint64_t distance = move >> 1;
        if (distance > from.size()) return false;
        if (!(move & 1)) {
            read = from_end + distance;
            return true;
        }
        if (distance >= from_end) return false;
        read = from_end - distance - 1;
        return true;
    }
};

bool ConvertEurope(std::vector<uint8_t>& xbe) {
    size_t size = kEuropePatchSize;
    if (!size) return false;
    EuropePatch europe{xbe, PatchReader(kEuropePatch, size)};
    while (!europe.patch.Done())
        if (!europe.ApplyBlock()) return false;
    xbe = std::move(europe.out);
    return true;
}

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

void CheckImage(const kernel::Disc& disc, const fs::path& iso) {
    std::error_code ec;
    uint64_t size = fs::file_size(iso, ec);
    if (ec) {
        LOGE("game: %s: its size cannot be read: %s", iso.string().c_str(), ec.message().c_str());
        return;
    }
    uint64_t end = 0;
    size_t files = 0, past = 0;
    std::string first;
    std::vector<std::pair<const kernel::Disc::Entry*, std::string>> todo{{disc.Find(std::string_view()), ""}};
    while (!todo.empty()) {
        auto [dir, path] = todo.back();
        todo.pop_back();
        if (!dir) continue;
        for (const kernel::Disc::Entry& e : dir->children) {
            std::string name = path.empty() ? e.name : path + "\\" + e.name;
            if (e.dir) {
                todo.push_back({&e, name});
                continue;
            }
            files++;
            uint64_t file_end = e.offset + e.size;
            end = std::max(end, file_end);
            if (file_end > size && past++ == 0) first = name;
        }
    }
    LOGR("Game: image %llu bytes, %zu files up to byte %llu", (unsigned long long)size, files, (unsigned long long)end);
    if (past)
        LOGE("game: %s is cut short: %zu of its files end past its %llu bytes, %s the first; copy the image again",
             iso.string().c_str(), past, (unsigned long long)size, first.c_str());
}

// no game yet: say where it goes and wait for it, keeping SDL's events moving
void WaitForGame() {
    static bool logged = false;
    if (!logged) {
        std::string where;
        for (const fs::path& p : AssetFolders()) where += (where.empty() ? "" : ", ") + p.string();
        LOGE("game: no game files in %s, waiting for them", where.c_str());
        logged = true;
    }
    for (int i = 0; i < 10; i++) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
                logx::Flush();
                std::_Exit(0);
            }
        SDL_Delay(100);
    }
}

fs::path AllowedFile() { return ExeDir() / "other_releases.txt"; }

bool AllowedRelease(const std::string& sha) {
    std::ifstream f(AllowedFile());
    for (std::string line; std::getline(f, line);)
        if (line == sha) return true;
    return false;
}

void AllowRelease(const std::string& sha) {
    std::ofstream f(AllowedFile(), std::ios::app);
    f << sha << "\n";
    if (!f) LOGW("game: %s cannot be written", AllowedFile().string().c_str());
}

bool AskOtherRelease(const fs::path& name, const std::string& sha) {
    if (AllowedRelease(sha)) {
        LOGR("Game: another release, started without asking as the player chose");
        return true;
    }
    std::string text = name.filename().string() +
                       " is a different release of the game than this build was made from, and will most likely "
                       "crash.\n\nStart it anyway?";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
        {0, 2, "Yes, don't ask again"},
    };
    std::string title = Utf8(kGame.short_title);
    SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), text.c_str(), 3, buttons, nullptr};
    int answer = 0;
    if (!SDL_ShowMessageBox(&box, &answer)) return false;
    if (answer == 2) AllowRelease(sha);
    return answer >= 1;
}

// 1 found, 0 none there, -1 the player turned down another release
//
// A disc image has the code and the data the game loads. An extracted install has both only with GlobData.bin and the
// level folders beside default.xbe. A lone default.xbe is the code alone: the game starts and then waits on files it
// cannot open, so the image and the full folder win over it wherever they are
int Search(GameFiles& game) {
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    std::string other_sha;

    auto consider = [&](const fs::path& assets, const fs::path& name, std::vector<uint8_t> xbe,
                        std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool europe = sha == kEuropeSha1;
        if (europe) europe = ConvertEurope(xbe);
        std::string image_sha = europe ? Sha1(xbe) : sha;
        bool match = image_sha == kImageSha1;
        const char* release = europe ? ", Europe release" : "";
        LOGR("Game: %s in %s, sha1 %s%s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(), release,
             match ? "" : ", another release");
        if (match) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
            other_sha = sha;
        }
        return false;
    };

    std::error_code ec;
    std::vector<fs::path> folders = AssetFolders();

    // the disc images first
    std::vector<fs::path> isos;
    for (const fs::path& assets : folders) {
        if (!fs::is_directory(assets, ec)) continue;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    }
    std::sort(isos.begin(), isos.end());
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        std::vector<uint8_t> xbe;
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        CheckImage(*disc, iso);
        if (consider(iso.parent_path(), iso, std::move(xbe), std::move(disc))) return 1;
    }

    // then the extracted installs, the ones with their data beside the xbe
    for (int pass = 0; pass < 2; pass++) {
        for (const fs::path& assets : folders) {
            if (!fs::is_directory(assets, ec)) continue;
            bool full = fs::exists(assets / "GlobData.bin", ec);
            if (pass == 0 && !full) continue;
            if (pass == 1 && full) continue;  // already looked at
            std::vector<uint8_t> xbe;
            if (ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, assets / IMAGE_XBE, std::move(xbe), nullptr))
                return 1;
        }
    }

    if (other_name.empty()) return 0;
    if (!AskOtherRelease(other_name, other_sha)) return -1;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return 1;
}

}  // namespace

bool FindGame(GameFiles& game) {
    for (;;) {
        int found = Search(game);
        if (found) return found > 0;
        WaitForGame();
    }
}

}  // namespace app
