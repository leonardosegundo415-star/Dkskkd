#pragma once

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// Frame Counter Free - input timing tracker.
//
// Everything here is based on the level time of the game (seconds) instead of
// a step counter: it is independent of the physics rate, so it keeps working
// with TPS changes and is restored by practice checkpoints.

namespace fcf {

template <class T>
inline T cfg(char const* key) {
    return geode::Mod::get()->getSettingValue<T>(key);
}

struct Row {
    int lo = 1;
    int hi = 1;  // INT_MAX = open ended ("5+")
    int count = 0;
    std::string label;
    bool hasColor = false;
    cocos2d::ccColor3B color{255, 255, 255};
};

struct InputEvent {
    int attempt;
    double time;          // level time in seconds
    int player;           // 1 or 2
    int button;           // 1 jump, 2 left, 3 right
    bool down;
    double intervalMs;    // -1 when it is the first input of that button
    int frames;           // interval converted to frames at the count FPS, 0 if none
};

class Tracker {
public:
    static Tracker& get() {
        static Tracker t;
        return t;
    }

    // ---- state read by the HUD ------------------------------------------
    int attempt = 1;
    double now = 0.0;       // updated every frame by the HUD
    double stepTime = 0.0;  // level time at the start of the current physics step
    double stepSec = 1.0 / 240.0;  // measured duration of one physics step
    bool calibrated = false;
    std::vector<Row> rows;
    std::string lastText;
    int lastFrames = 0;
    bool usedP2 = false;
    std::array<bool, 8> held{};
    std::vector<InputEvent> events;

    // ---- lifecycle -------------------------------------------------------
    void newSession(std::string const& name) {
        levelName = name;
        attempt = 1;
        events.clear();
        exportedUpTo = 0;
        usedP2 = false;
        calibrated = false;
        samples.clear();
        stepSec = 1.0 / 240.0;
        clearAttemptState();
        key.clear();
        ensureKey();
    }

    void resetAttempt() {
        ++attempt;
        clearAttemptState();
        if (cfg<bool>("reset-on-death")) {
            for (auto& r : rows) r.count = 0;
        }
    }

    // Row colour: the one chosen in "Counter colours", else tight = red ... wide = blue.
    cocos2d::ccColor3B colorOf(int index) const {
        static const cocos2d::ccColor3B palette[] = {
            {255, 76, 84}, {255, 138, 61}, {255, 210, 63}, {141, 247, 145},
            {90, 215, 255}, {108, 140, 255}, {176, 124, 255}, {255, 255, 255},
        };
        if (index < 0 || index >= static_cast<int>(rows.size())) return {200, 200, 200};
        if (rows[index].hasColor) return rows[index].color;
        return palette[std::min<size_t>(index, std::size(palette) - 1)];
    }

    // NaN GD preset overrides the counter rows (9-10 down to 1).
    static std::string activeRanges() {
        if (cfg<bool>("nan-gd-settings")) return "9-10,8,7,6,5,4,3,2,1";
        return cfg<std::string>("counter-ranges");
    }

    // Called at the start of every physics step. Measures the real step length
    // (median of the first 120 deltas) so shifts and windows are exact at any TPS.
    void observeStep(double t) {
        stepTime = t;
        if (!calibrated && prevStep >= 0.0) {
            double d = t - prevStep;
            if (d > 1e-5 && d < 0.05) samples.push_back(d);
            if (samples.size() >= 120) {
                std::sort(samples.begin(), samples.end());
                stepSec = samples[samples.size() / 2];
                calibrated = true;
            }
        }
        prevStep = t;
    }
    void forgetStep() { prevStep = -1.0; }

    int stepsToFrames(int steps) const {
        double fps = static_cast<double>(cfg<int64_t>("count-fps"));
        return std::max(1, static_cast<int>(std::ceil(steps * stepSec * fps - 0.05)));
    }

    // A measured click window (playback of a scanned macro).
    void addWindow(int frames, bool capped, int number) {
        ensureKey();
        int ri = rowIndexFor(frames);
        if (ri >= 0) rows[ri].count++;
        lastFrames = frames;
        lastText = fmt::format("Click {} window {}{}f", number, frames, capped ? "+" : "");
    }

    // ---- conversion ------------------------------------------------------
    static int toFrames(double seconds) {
        double fps = static_cast<double>(cfg<int64_t>("count-fps"));
        // 0.05 absorbs float error: exactly 1/240 s at 240 FPS must be 1 frame.
        int f = static_cast<int>(std::ceil(seconds * fps - 0.05));
        return std::max(1, f);
    }

    static int frameNumber(double seconds) {
        double fps = static_cast<double>(cfg<int64_t>("count-fps"));
        return static_cast<int>(std::floor(seconds * fps + 0.05));
    }

    int rowIndexFor(int frames) const {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (frames >= rows[i].lo && frames <= rows[i].hi) return static_cast<int>(i);
        }
        return -1;
    }

    // ---- input -----------------------------------------------------------
    void onInput(double time, bool down, int button, bool player1, bool quiet = false) {
        int idx = (player1 ? 0 : 4) + std::clamp(button, 0, 3);
        held[idx] = down;
        if (!player1) usedP2 = true;
        if (quiet) return;  // playback / scan: windows are shown instead of intervals

        double intervalSec = last[idx] < 0.0 ? -1.0 : std::max(0.0, time - last[idx]);
        last[idx] = time;

        ensureKey();

        InputEvent ev{attempt, time, player1 ? 1 : 2, button, down, -1.0, 0};
        if (intervalSec >= 0.0) {
            ev.intervalMs = intervalSec * 1000.0;
            ev.frames = toFrames(intervalSec);
        }
        if (events.size() < kMaxEvents) events.push_back(ev);

        if (ev.frames <= 0) return;                         // first input of this button
        if (!down && !cfg<bool>("count-releases")) return;  // releases not counted

        int ri = rowIndexFor(ev.frames);
        if (ri >= 0) rows[ri].count++;

        lastFrames = ev.frames;
        lastText = fmt::format(
            "P{} {} {}f ({:.1f} ms)", ev.player, down ? "gap" : "hold", ev.frames, ev.intervalMs
        );

        if (down && cfg<bool>("sounds") && ev.frames <= cfg<int64_t>("sound-max-frames")) {
            playDing(ev.frames);
        }
    }

    // Re-parses the ranges and rebuilds the counts if a related setting changed.
    void ensureKey() {
        auto k = fmt::format(
            "{}|{}|{}|{}|{}", activeRanges(), cfg<int64_t>("count-fps"),
            cfg<bool>("count-releases"), cfg<bool>("reset-on-death"), cfg<std::string>("counter-colors")
        );
        if (k == key) return;
        key = std::move(k);
        parseRanges(activeRanges());
        applyColors(cfg<std::string>("counter-colors"));
        rebuildCounts();
    }

    // ---- export ----------------------------------------------------------
    void exportSession() {
        if (!cfg<bool>("export-csv") || events.size() <= exportedUpTo) return;

        auto dir = geode::Mod::get()->getSaveDir() / "sessions";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string safe;
        for (char c : levelName) {
            safe += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        }
        if (safe.empty()) safe = "level";
        auto file = dir / fmt::format("{}_{}.csv", safe, static_cast<long long>(std::time(nullptr)));

        std::string out = "attempt,time_s,frame,player,button,action,interval_ms,interval_frames\n";
        for (auto const& e : events) {
            out += fmt::format(
                "{},{:.5f},{},{},{},{},{},{}\n", e.attempt, e.time, frameNumber(e.time), e.player,
                e.button, e.down ? "press" : "release",
                e.intervalMs < 0 ? std::string() : fmt::format("{:.3f}", e.intervalMs),
                e.frames > 0 ? std::to_string(e.frames) : std::string()
            );
        }
        auto res = geode::utils::file::writeString(file, out);
        if (res.isErr()) {
            geode::log::warn("Could not write {}: {}", file.string(), res.unwrapErr());
        } else {
            geode::log::info("Saved {} inputs to {}", events.size(), file.string());
            exportedUpTo = events.size();
        }
    }

    // Custom sound for that frame count (1-9, "wide" from 10), else the bundled ding.
    static void playDing(int frames) {
        auto* engine = FMODAudioEngine::sharedEngine();
        float volume = static_cast<float>(cfg<double>("sound-volume"));
        std::string key = frames <= 9 ? fmt::format("sound-{}", frames) : std::string("sound-wide");
        auto custom = cfg<std::filesystem::path>(key.c_str());
        if (!custom.empty() && std::filesystem::exists(custom)) {
            engine->playEffect(custom.string(), 1.f, 0.f, volume);
            return;
        }
        static auto path = (geode::Mod::get()->getResourcesDir() / "ding.wav").string();
        // 1 frame = lowest pitch, 9+ frames = highest (two semitones per step).
        float speed = static_cast<float>(std::pow(2.0, (std::min(frames, 9) - 1) * 2.0 / 12.0 - 1.0));
        engine->playEffect(path, speed, 0.f, volume);
    }

private:
    static constexpr size_t kMaxEvents = 300000;

    std::string levelName;
    std::string key;
    size_t exportedUpTo = 0;
    std::array<double, 8> last{};
    std::vector<double> samples;
    double prevStep = -1.0;

    Tracker() { last.fill(-1.0); }

    void clearAttemptState() {
        last.fill(-1.0);
        held.fill(false);
        lastText.clear();
        lastFrames = 0;
    }

    void parseRanges(std::string const& text, bool fallback = false) {
        rows.clear();
        std::stringstream ss(text);
        std::string tok;
        while (std::getline(ss, tok, ',') && rows.size() < 12) {
            tok.erase(
                std::remove_if(tok.begin(), tok.end(), [](unsigned char c) { return std::isspace(c); }),
                tok.end()
            );
            if (tok.empty()) continue;
            Row r;
            r.label = tok;
            try {
                auto plus = tok.find('+');
                auto dash = tok.find('-');
                if (plus != std::string::npos) {
                    r.lo = std::stoi(tok.substr(0, plus));
                    r.hi = INT_MAX;
                } else if (dash != std::string::npos) {
                    r.lo = std::stoi(tok.substr(0, dash));
                    r.hi = std::stoi(tok.substr(dash + 1));
                } else {
                    r.lo = r.hi = std::stoi(tok);
                }
            } catch (...) {
                continue;
            }
            if (r.lo < 1 || r.hi < r.lo) continue;
            rows.push_back(std::move(r));
        }
        if (rows.empty() && !fallback) parseRanges("1,2,3,4,5+", true);
    }

    // "1:ff4c54,5:8df791" -> the row whose first frame is 1 is red, and so on.
    void applyColors(std::string const& text) {
        std::stringstream ss(text);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            auto colon = tok.find(':');
            if (colon == std::string::npos) continue;
            try {
                int first = std::stoi(tok.substr(0, colon));
                unsigned v = static_cast<unsigned>(std::stoul(tok.substr(colon + 1), nullptr, 16));
                for (auto& r : rows) {
                    if (r.lo == first) {
                        r.hasColor = true;
                        r.color = {
                            static_cast<GLubyte>((v >> 16) & 255), static_cast<GLubyte>((v >> 8) & 255),
                            static_cast<GLubyte>(v & 255)
                        };
                    }
                }
            } catch (...) {
            }
        }
    }

    void rebuildCounts() {
        for (auto& r : rows) r.count = 0;
        bool onlyThisAttempt = cfg<bool>("reset-on-death");
        bool releases = cfg<bool>("count-releases");
        for (auto const& e : events) {
            if (e.frames <= 0) continue;
            if (!e.down && !releases) continue;
            if (onlyThisAttempt && e.attempt != attempt) continue;
            int ri = rowIndexFor(e.frames);
            if (ri >= 0) rows[ri].count++;
        }
    }
};

}  // namespace fcf
