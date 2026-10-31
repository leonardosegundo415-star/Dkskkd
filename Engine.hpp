#pragma once

#include "Tracker.hpp"

#include <chrono>
#include <fstream>

// Macro recording, playback and the window scan (pure logic, no node code).
//
// A macro is the list of inputs of a completed run, stamped with the level time
// at the start of the physics step they were pressed on.
//
// The scan answers "how many steps early or late could this click have been?":
// for every click it replays the macro with that single click moved by 1, 2, 3...
// steps (earlier and later) and checks that the icon survives. The widest
// contiguous range of passing offsets is the click's window.

namespace fcf {

struct MacroEvent {
    double time;
    bool down;
    int button;
    bool p1;
    int gm = 0;  // 0 other, 1 ship, 2 wave (for the straight-fly / wave-spam filters)
};

struct ClickResult {
    bool measured = false;
    int lo = 0;  // steps it can move earlier
    int hi = 0;  // steps it can move later
    bool capLo = false;
    bool capHi = false;
    bool useless = false;  // the run still finishes without this click
    int steps() const { return lo + hi + 1; }
    bool capped() const { return capLo || capHi; }
};

enum class Mode { Idle, Playback, Scan };

class Engine {
public:
    static Engine& get() {
        static Engine e;
        return e;
    }

    Mode mode = Mode::Idle;
    std::vector<MacroEvent> macro;
    std::vector<ClickResult> results;
    std::vector<MacroEvent> recording;
    std::vector<MacroEvent> play;  // what the injector plays right now
    size_t idx = 0;
    bool injecting = false;

    enum class Outcome { Pending, Pass, Fail };
    Outcome outcome = Outcome::Pending;
    double passTime = 0.0;
    int warmup = 0;
    std::string status;
    double endTime = 0.0;   // level time where the recording ended
    bool incomplete = false;

    // ---- level ------------------------------------------------------------
    void setLevel(std::string const& name) {
        levelName = safeName(name);
        macro.clear();
        results.clear();
        recording.clear();
        play.clear();
        endTime = 0.0;
        incomplete = false;
        mode = Mode::Idle;
        status.clear();
        load();
    }

    bool hasMacro() const { return !macro.empty(); }

    void commitRecording(double end, bool isIncomplete) {
        if (recording.empty()) return;
        if (isIncomplete) {
            // Keep the furthest incomplete run, and never replace a finished macro by a failed one.
            if (!macro.empty() && (!incomplete || end <= endTime)) return;
        }
        macro = recording;
        endTime = end;
        incomplete = isIncomplete;
        results.assign(macro.size(), ClickResult{});
        save();
        if (!isIncomplete) notify(fmt::format("Macro saved ({} clicks)", macro.size()), true);
    }

    // ---- playback ---------------------------------------------------------
    void startPlayback() {
        mode = Mode::Playback;
        play = macro;
        idx = 0;
        status = "REPLAY";
    }

    // Restores speed and, if a scan was running, keeps what it measured.
    void stop() {
        if (mode == Mode::Scan) {
            save();
            exportWindows();
        }
        mode = Mode::Idle;
        status.clear();
        setSpeed(1.f);
    }

    // ---- scan -------------------------------------------------------------
    void startScan(Tracker const& tr) {
        mode = Mode::Scan;
        phase = Phase::Baseline;
        stepSec = tr.stepSec;
        range_ = static_cast<int>(cfg<int64_t>("scan-range"));
        if (results.size() != macro.size() || !cfg<bool>("scan-resume")) results.assign(macro.size(), ClickResult{});
        classify();
        play = macro;
        idx = 0;
        trials = 0;
        k = 0;
        outcome = Outcome::Pending;
        warmup = 3;
        removing = false;
        removeTested = false;
        scanStart = std::chrono::steady_clock::now();
        passTime = cap(macro.back().time + horizon());
        status = "SCAN baseline";
        setSpeed(static_cast<float>(cfg<int64_t>("scan-speed")));
    }

    // Called once `outcome` is decided. True: reset the level and run the next
    // trial. False: the scan is over (mode is back to Idle).
    bool advance(Tracker const& tr) {
        bool pass = outcome == Outcome::Pass;
        outcome = Outcome::Pending;
        warmup = 3;
        stepSec = tr.stepSec;

        int limit = static_cast<int>(cfg<int64_t>("scan-time-limit-minutes"));
        if (limit > 0 && std::chrono::steady_clock::now() - scanStart > std::chrono::minutes(limit)) {
            stop();
            notify("Scan stopped: time limit reached (the rest is left unmeasured)", true);
            return false;
        }

        if (phase == Phase::Baseline) {
            if (!pass) {
                notify("Scan stopped: the macro does not get through on its own", false);
                stop();
                return false;
            }
            phase = Phase::Probe;
            if (!seek()) return false;
            return nextTrial();
        }
        if (removing) {  // result of "play without this click"
            removing = false;
            removeTested = true;
            if (pass) {
                ClickResult u;
                u.measured = true;
                u.useless = true;
                results[k] = u;
                if (partnerOf(k) >= 0) results[partnerOf(k)] = u;
                ++k;
                if (!seek()) return false;
            }
            return nextTrial();
        }
        range_ = static_cast<int>(cfg<int64_t>("scan-range"));
        if (pass && d < range_) {
            ++d;
            return nextTrial();
        }
        if (!endDirection(pass ? d : d - 1, pass)) return false;
        return nextTrial();
    }

    int windowFrames(Tracker const& tr, size_t i) const {
        return tr.stepsToFrames(results[i].steps());
    }

    static std::string safeName(std::string const& n) {
        std::string s;
        for (char c : n) s += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        return s.empty() ? "level" : s;
    }

    static void setSpeed(float s) {
        cocos2d::CCDirector::get()->getScheduler()->setTimeScale(s);
    }

    static void notify(std::string const& msg, bool ok) {
        geode::Notification::create(
            msg, ok ? geode::NotificationIcon::Success : geode::NotificationIcon::Error
        )->show();
    }

private:
    enum class Phase { Baseline, Probe };
    Phase phase = Phase::Baseline;
    size_t k = 0;
    int dir = 1;
    int d = 1;
    int trials = 0;
    int range_ = 12;
    bool removing = false;
    bool removeTested = false;
    double stepSec = 1.0 / 240.0;
    ClickResult cur;
    std::string levelName;
    std::vector<char> excluded;  // straight fly / wave spam: not measured
    std::chrono::steady_clock::time_point scanStart;

    static double horizon() { return cfg<double>("scan-horizon"); }

    // An incomplete macro only counts up to where it died.
    double cap(double t) const { return incomplete ? std::min(t, endTime - 0.02) : t; }

    int partnerOf(size_t i) const {
        if (!macro[i].down) return -1;
        for (size_t j = i + 1; j < macro.size(); ++j) {
            if (macro[j].p1 == macro[i].p1 && macro[j].button == macro[i].button && !macro[j].down) {
                return static_cast<int>(j);
            }
        }
        return -1;
    }

    // Marks clicks that are straight fly (ship) or wave spam: they are skipped.
    void classify() {
        excluded.assign(macro.size(), 0);
        bool nan = cfg<bool>("nan-gd-settings");
        if (!nan && cfg<bool>("count-ship") && cfg<bool>("count-wave-spam")) return;
        int flyN = nan ? 4 : static_cast<int>(cfg<int64_t>("straight-fly-clicks"));
        int waveN = nan ? 5 : static_cast<int>(cfg<int64_t>("wave-spam-clicks"));

        if (nan || !cfg<bool>("count-ship")) {
            size_t i = 0;
            while (i < macro.size()) {
                if (macro[i].gm != 1) { ++i; continue; }
                size_t j = i;
                while (j + 1 < macro.size() && macro[j + 1].gm == 1 && macro[j + 1].time - macro[j].time < 0.25) ++j;
                if (static_cast<int>(j - i + 1) >= flyN) {
                    for (size_t x = i; x <= j; ++x) excluded[x] = 1;
                }
                i = j + 1;
            }
        }
        if (nan || !cfg<bool>("count-wave-spam")) {
            // N or more wave clicks inside about two blocks (0.19 s at normal speed).
            for (size_t i = 0; i < macro.size(); ++i) {
                if (macro[i].gm != 2) continue;
                size_t j = i;
                while (j + 1 < macro.size() && macro[j + 1].gm == 2 && macro[j + 1].time - macro[i].time <= 0.19) ++j;
                if (static_cast<int>(j - i + 1) >= waveN) {
                    for (size_t x = i; x <= j; ++x) excluded[x] = 1;
                }
            }
        }
    }

    bool skip(size_t i) const {
        auto const& r = results[i];
        if (excluded[i]) return true;
        if (r.useless) return true;
        double pct = endTime > 0.0 ? macro[i].time / endTime * 100.0 : 0.0;
        if (pct < cfg<int64_t>("scan-part-from") || pct > cfg<int64_t>("scan-part-to")) return true;
        if (cfg<bool>("scan-resume") && r.measured && (!r.capped() || range_ <= std::max(r.lo, r.hi))) return true;
        return false;
    }

    // Moves k to the next click that has to be measured. False when there is none.
    bool seek() {
        while (k < macro.size() && skip(k)) ++k;
        if (k >= macro.size()) {
            stop();
            notify("Scan finished", true);
            return false;
        }
        dir = 1;
        d = 1;
        cur = {};
        removeTested = false;
        return true;
    }

    // Finishes one side (+ or -) of click k. False when that was the last click.
    bool endDirection(int margin, bool capped) {
        if (dir == 1) {
            cur.hi = std::max(0, margin);
            cur.capHi = capped;
            dir = -1;
            d = 1;
            return true;
        }
        cur.lo = std::max(0, margin);
        cur.capLo = capped;
        cur.measured = true;
        results[k] = cur;
        ++k;
        return seek();
    }

    // Builds the next playable trial. False when the scan is over.
    bool nextTrial() {
        for (;;) {
            // Does the run still finish with this click (and its release) left out?
            if (!removeTested && cfg<bool>("skip-useless") && macro[k].down && partnerOf(k) >= 0) {
                int pj = partnerOf(k);
                play = macro;
                play.erase(play.begin() + pj);
                play.erase(play.begin() + k);
                passTime = cap(macro[pj].time + horizon());
                removing = true;
                ++trials;
                status = fmt::format("SCAN {}/{}  test without  trials {}", k + 1, macro.size(), trials);
                return true;
            }
            removeTested = true;

            double nt = macro[k].time + dir * d * stepSec;
            double lowB = k > 0 ? macro[k - 1].time : -1e-9;
            double highB = k + 1 < macro.size() ? macro[k + 1].time : 1e18;
            bool inside = nt > lowB + 1e-6 && nt < highB - 1e-6;
            if (k == 0 && nt < 0.0) inside = false;
            if (!inside) {  // no room left on this side
                if (!endDirection(d - 1, false)) return false;
                continue;
            }
            play = macro;
            play[k].time = nt;
            passTime = cap(std::max(macro[k].time, nt) + horizon());
            passTime = std::max(passTime, std::max(macro[k].time, nt) + 0.01);
            ++trials;
            status = fmt::format(
                "SCAN {}/{}  {}{}  trials {}", k + 1, macro.size(), dir > 0 ? "+" : "-", d, trials
            );
            return true;
        }
    }

    // ---- files ------------------------------------------------------------
    std::filesystem::path dir_() const {
        auto p = geode::Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }

    void save() {
        if (macro.empty()) return;
        std::string out = "FCM2\n";
        out += fmt::format("T {:.9f} {}\n", endTime, incomplete ? 1 : 0);
        for (auto const& e : macro) {
            out += fmt::format(
                "E {:.9f} {} {} {} {}\n", e.time, e.down ? 1 : 0, e.button, e.p1 ? 1 : 0, e.gm
            );
        }
        for (size_t i = 0; i < results.size(); ++i) {
            auto const& r = results[i];
            if (!r.measured) continue;
            out += fmt::format(
                "R {} {} {} {} {} {}\n", i, r.lo, r.hi, r.capLo ? 1 : 0, r.capHi ? 1 : 0, r.useless ? 1 : 0
            );
        }
        auto res = geode::utils::file::writeString(dir_() / (levelName + ".fcm"), out);
        if (res.isErr()) geode::log::warn("Could not save macro: {}", res.unwrapErr());
    }

    void load() {
        auto res = geode::utils::file::readString(dir_() / (levelName + ".fcm"));
        if (res.isErr()) return;
        std::istringstream in(res.unwrap());
        std::string line;
        std::getline(in, line);
        if (line.rfind("FCM", 0) != 0) return;
        std::vector<std::pair<size_t, ClickResult>> pending;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            char tag = 0;
            ls >> tag;
            if (tag == 'T') {
                int inc = 0;
                if (ls >> endTime >> inc) incomplete = inc != 0;
            } else if (tag == 'E') {
                MacroEvent e{};
                int dn = 0, p1 = 1, gm = 0;
                if (ls >> e.time >> dn >> e.button >> p1) {
                    ls >> gm;
                    e.down = dn != 0;
                    e.p1 = p1 != 0;
                    e.gm = gm;
                    macro.push_back(e);
                }
            } else if (tag == 'R') {
                size_t i = 0;
                ClickResult r;
                int a = 0, b = 0, u = 0;
                if (ls >> i >> r.lo >> r.hi >> a >> b) {
                    ls >> u;
                    r.capLo = a != 0;
                    r.capHi = b != 0;
                    r.useless = u != 0;
                    r.measured = true;
                    pending.emplace_back(i, r);
                }
            }
        }
        if (endTime <= 0.0 && !macro.empty()) endTime = macro.back().time + 1.0;
        results.assign(macro.size(), ClickResult{});
        for (auto& [i, r] : pending) {
            if (i < results.size()) results[i] = r;
        }
    }

    void exportWindows() {
        std::string out = "click,time_s,action,button,player,window_steps,window_frames,at_least,left_out\n";
        Tracker const& tr = Tracker::get();
        for (size_t i = 0; i < macro.size(); ++i) {
            if (!results[i].measured) continue;
            auto const& e = macro[i];
            out += fmt::format(
                "{},{:.5f},{},{},{},{},{},{},{}\n", i + 1, e.time, e.down ? "press" : "release", e.button,
                e.p1 ? 1 : 2, results[i].steps(), windowFrames(tr, i), results[i].capped() ? 1 : 0,
                results[i].useless ? 1 : 0
            );
        }
        auto res = geode::utils::file::writeString(dir_() / (levelName + "_windows.csv"), out);
        if (res.isErr()) geode::log::warn("Could not save windows: {}", res.unwrapErr());
    }
};

}  // namespace fcf
