#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "Engine.hpp"
#include "Tracker.hpp"

using namespace geode::prelude;

namespace {

ccColor3B colorForRow(int index) { return fcf::Tracker::get().colorOf(index); }

}  // namespace

// On-screen display. Lives in the UILayer, so it stays fixed to the screen.
class FcfHud : public CCNode {
public:
    static FcfHud* create() {
        auto ret = new FcfHud();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        m_text = CCNode::create();
        this->addChild(m_text);
        for (int i = 0; i < 2; ++i) {
            m_box[i] = CCLayerColor::create({255, 255, 255, 255}, 16.f, 16.f);
            this->addChild(m_box[i]);
        }
        return true;
    }

    void refresh(float fps, double levelTime) {
        auto& t = fcf::Tracker::get();
        t.now = levelTime;
        t.ensureKey();

        // ---- build the lines -----------------------------------------------
        struct Line {
            std::string text;
            ccColor3B color;
        };
        std::vector<Line> lines;

        bool show = fcf::cfg<bool>("hud");
        if (show) {
            auto const& status = fcf::Engine::get().status;
            if (!status.empty()) lines.push_back({status, {255, 210, 63}});
            if (fcf::cfg<bool>("show-frame")) {
                lines.push_back({fmt::format(
                    "Frame {} @{}  {:.2f}s", fcf::Tracker::frameNumber(levelTime),
                    fcf::cfg<int64_t>("count-fps"), levelTime
                ), {255, 255, 255}});
            }
            if (fcf::cfg<bool>("show-fps")) {
                lines.push_back({fmt::format("FPS {:.0f}", fps), {200, 200, 200}});
            }
            if (fcf::cfg<bool>("show-last-click") && !t.lastText.empty()) {
                lines.push_back({t.lastText, colorForRow(t.rowIndexFor(t.lastFrames))});
            }
            if (fcf::cfg<bool>("show-counters")) {
                for (size_t i = 0; i < t.rows.size(); ++i) {
                    lines.push_back({
                        fmt::format("{}f: {}", t.rows[i].label, t.rows[i].count),
                        colorForRow(static_cast<int>(i)),
                    });
                }
            }
        }

        // ---- layout ----------------------------------------------------------
        auto win = CCDirector::get()->getWinSize();
        auto corner = fcf::cfg<std::string>("hud-corner");
        bool right = corner.find("Right") != std::string::npos;
        bool top = corner.find("Top") != std::string::npos;
        float scale = static_cast<float>(fcf::cfg<double>("hud-scale"));
        GLubyte opacity = static_cast<GLubyte>(fcf::cfg<int64_t>("hud-opacity") * 255 / 100);
        float lineH = 15.f * scale;
        float margin = 6.f;
        float x = right ? win.width - margin : margin;
        float yStart = top ? win.height - margin - lineH / 2 : margin + lineH / 2;

        while (m_labels.size() < lines.size()) {
            auto l = CCLabelBMFont::create("", "chatFont.fnt");
            m_text->addChild(l);
            m_labels.push_back(l);
        }
        for (size_t i = 0; i < m_labels.size(); ++i) {
            auto l = m_labels[i];
            if (i >= lines.size()) {
                l->setVisible(false);
                continue;
            }
            l->setVisible(true);
            if (m_cache.size() <= i) m_cache.resize(i + 1);
            if (m_cache[i] != lines[i].text) {
                l->setString(lines[i].text.c_str());
                m_cache[i] = lines[i].text;
            }
            l->setColor(lines[i].color);
            l->setOpacity(opacity);
            l->setScale(0.6f * scale);
            l->setAnchorPoint({right ? 1.f : 0.f, 0.5f});
            float y = top ? yStart - lineH * i : yStart + lineH * (lines.size() - 1 - i);
            l->setPosition({x, y});
        }

        // ---- input display ---------------------------------------------------
        bool inputs = show && fcf::cfg<bool>("input-display");
        for (int p = 0; p < 2; ++p) {
            bool visible = inputs && (p == 0 || t.usedP2);
            m_box[p]->setVisible(visible);
            if (!visible) continue;
            bool down = t.held[p * 4 + 1] || t.held[p * 4 + 2] || t.held[p * 4 + 3];
            m_box[p]->setColor(down ? ccColor3B{141, 247, 145} : ccColor3B{120, 120, 120});
            m_box[p]->setOpacity(down ? opacity : static_cast<GLubyte>(opacity * 0.4f));
            m_box[p]->setScale(scale);
            m_box[p]->setPosition({p == 0 ? win.width / 2 - 4.f - 16.f * scale : win.width / 2 + 4.f, 8.f});
        }
    }

private:
    CCNode* m_text = nullptr;
    CCLayerColor* m_box[2] = {nullptr, nullptr};
    std::vector<CCLabelBMFont*> m_labels;
    std::vector<std::string> m_cache;
};

// ---- playback / scan helpers -------------------------------------------------

namespace {

// Floating "3f" numbers that appear on each click while a scanned macro plays.
struct Popup {
    Ref<CCNode> node;
    double expires;
};
std::vector<Popup> g_popups;
double g_clock = 0.0;
bool g_deathHitboxes = false;

void clearPopups() { g_popups.clear(); }

void showWindow(GJBaseGameLayer* layer, size_t i) {
    auto& e = fcf::Engine::get();
    auto& t = fcf::Tracker::get();
    if (i >= e.results.size() || !e.results[i].measured || e.results[i].useless) return;

    int frames = e.windowFrames(t, i);
    bool capped = e.results[i].capped();
    t.addWindow(frames, capped, static_cast<int>(i) + 1);

    if (fcf::cfg<bool>("sounds") && frames <= fcf::cfg<int64_t>("sound-max-frames")) {
        fcf::Tracker::playDing(frames);
    }
    if (!fcf::cfg<bool>("show-popups")) return;

    auto* player = e.macro[i].p1 ? layer->m_player1 : layer->m_player2;
    if (!player || !layer->m_objectLayer) return;

    auto text = fmt::format("{}{}", frames, capped ? "+" : "");
    auto lbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    lbl->setScale(0.45f * static_cast<float>(fcf::cfg<double>("hud-scale")));
    lbl->setColor(colorForRow(t.rowIndexFor(frames)));
    lbl->setPosition(player->getPosition() + CCPoint{0.f, e.macro[i].down ? 28.f : -28.f});
    layer->m_objectLayer->addChild(lbl, 1000);
    lbl->runAction(CCSpawn::create(CCMoveBy::create(0.8f, {0.f, 18.f}), CCFadeOut::create(0.8f), nullptr));
    g_popups.push_back({Ref<CCNode>(lbl), g_clock + 1.0});
}

// Plays every macro input that is due on this physics step.
void injectMacro(GJBaseGameLayer* layer) {
    auto& e = fcf::Engine::get();
    auto& t = fcf::Tracker::get();
    while (e.idx < e.play.size() && e.play[e.idx].time <= t.stepTime + 1e-4) {
        auto ev = e.play[e.idx];
        size_t i = e.idx++;
        e.injecting = true;
        layer->handleButton(ev.down, ev.button, ev.p1);
        e.injecting = false;
        if (e.mode == fcf::Mode::Playback) showWindow(layer, i);
    }
}

}  // namespace

// ---- inputs ---------------------------------------------------------------

class $modify(FcfBaseLayer, GJBaseGameLayer) {
    bool isMainLevel() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (this->isMainLevel()) {
            fcf::Tracker::get().observeStep(m_gameState.m_levelTime);
            if (fcf::Engine::get().mode != fcf::Mode::Idle) injectMacro(this);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        bool mine = this->isMainLevel();
        auto& e = fcf::Engine::get();

        // While a macro plays, the player's own touches are ignored.
        if (mine && e.mode != fcf::Mode::Idle && !e.injecting) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!mine) return;

        auto* player = isPlayer1 ? m_player1 : m_player2;
        if (player && player->m_isDead) return;

        auto& t = fcf::Tracker::get();
        if (e.mode == fcf::Mode::Idle) {
            int gm = (player && player->m_isShip) ? 1 : (player && player->m_isDart) ? 2 : 0;
            if (e.recording.size() < 300000) e.recording.push_back({t.stepTime, down, button, isPlayer1, gm});
            t.onInput(t.stepTime, down, button, isPlayer1);
        } else {
            t.onInput(t.stepTime, down, button, isPlayer1, true);
        }
    }
};

// ---- level lifecycle --------------------------------------------------------

class $modify(FcfPlayLayer, PlayLayer) {
    struct Fields {
        FcfHud* hud = nullptr;
        float acc = 0.f;
        int frames = 0;
        float fps = 0.f;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        clearPopups();
        fcf::Tracker::get().newSession(std::string(m_level->m_levelName));
        fcf::Engine::get().setLevel(std::string(m_level->m_levelName));
        if (auto hud = FcfHud::create()) {
            m_uiLayer->addChild(hud, 100);
            m_fields->hud = hud;
        }
    }

    void startTrial() {
        auto& e = fcf::Engine::get();
        e.idx = 0;
        this->resetLevelFromStart();
    }

    void postUpdate(float dt) {
        auto& eng = fcf::Engine::get();
        bool hitboxes = eng.mode != fcf::Mode::Scan && (fcf::cfg<bool>("show-hitboxes") || g_deathHitboxes);
        if (hitboxes) m_isDebugDrawEnabled = true;
        PlayLayer::postUpdate(dt);
        if (hitboxes && m_debugDrawNode) m_debugDrawNode->setVisible(true);

        // Game speed: recording / playback slow motion, fast while scanning.
        float want = eng.mode == fcf::Mode::Scan
            ? static_cast<float>(fcf::cfg<int64_t>("scan-speed"))
            : eng.mode == fcf::Mode::Playback ? static_cast<float>(fcf::cfg<double>("playback-speed"))
                                              : static_cast<float>(fcf::cfg<double>("record-speed"));
        auto* sched = CCDirector::get()->getScheduler();
        if (std::abs(sched->getTimeScale() - want) > 1e-3f) sched->setTimeScale(want);
        auto f = m_fields.self();
        f->acc += dt;
        f->frames++;
        if (f->acc >= 0.5f) {
            f->fps = f->frames / f->acc;
            f->acc = 0.f;
            f->frames = 0;
        }
        if (f->hud) f->hud->refresh(f->fps, m_gameState.m_levelTime);

        g_clock += dt;
        while (!g_popups.empty() && g_popups.front().expires <= g_clock) {
            g_popups.front().node->removeFromParentAndCleanup(true);
            g_popups.erase(g_popups.begin());
        }

        auto& e = fcf::Engine::get();
        if (e.mode != fcf::Mode::Scan) return;

        if (e.warmup > 0) {
            --e.warmup;
        } else if (e.outcome == fcf::Engine::Outcome::Pending && m_gameState.m_levelTime >= e.passTime) {
            e.outcome = fcf::Engine::Outcome::Pass;
        }
        if (e.outcome != fcf::Engine::Outcome::Pending) {
            if (e.advance(fcf::Tracker::get())) this->startTrial();
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& t = fcf::Tracker::get();
        auto& e = fcf::Engine::get();
        t.resetAttempt();
        t.forgetStep();
        e.idx = 0;
        g_deathHitboxes = false;
        if (e.mode == fcf::Mode::Idle) {
            // Practice checkpoints rewind the level time: drop what was recorded after it.
            double now = m_gameState.m_levelTime;
            while (!e.recording.empty() && e.recording.back().time >= now - 1e-6) e.recording.pop_back();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& e = fcf::Engine::get();
        if (e.mode == fcf::Mode::Scan) {
            // The icon does not die during a scan: the trial just failed.
            if (e.outcome == fcf::Engine::Outcome::Pending) e.outcome = fcf::Engine::Outcome::Fail;
            return;
        }
        if (e.mode == fcf::Mode::Idle && fcf::cfg<bool>("save-incomplete") && fcf::cfg<bool>("record-macros")) {
            e.commitRecording(m_gameState.m_levelTime, true);
        }
        g_deathHitboxes = fcf::cfg<bool>("hitboxes-on-death");
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& e = fcf::Engine::get();
        if (e.mode == fcf::Mode::Scan) {
            if (e.outcome == fcf::Engine::Outcome::Pending) e.outcome = fcf::Engine::Outcome::Pass;
            return;
        }
        PlayLayer::levelComplete();
        if (e.mode == fcf::Mode::Playback) {
            e.stop();
        } else if (fcf::cfg<bool>("record-macros")) {
            e.commitRecording(m_gameState.m_levelTime, false);
        }
        fcf::Tracker::get().exportSession();
    }

    void onQuit() {
        fcf::Engine::get().stop();
        clearPopups();
        fcf::Tracker::get().exportSession();
        PlayLayer::onQuit();
    }
};

// ---- pause menu ----------------------------------------------------------------

class $modify(FcfPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto& e = fcf::Engine::get();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        float y = 24.f;
        auto add = [&](char const* label, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(label, 0.6f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({70.f, y});
            menu->addChild(btn);
            y += 30.f;
        };
        add("Frame HUD", menu_selector(FcfPause::onToggleHud));
        if (e.hasMacro() || e.mode != fcf::Mode::Idle) {
            add(e.mode == fcf::Mode::Playback ? "Stop replay" : "Replay", menu_selector(FcfPause::onReplay));
            add(e.mode == fcf::Mode::Scan ? "Cancel scan" : "Scan windows", menu_selector(FcfPause::onScan));
        }
        this->addChild(menu, 10);
    }

    void onToggleHud(CCObject*) {
        auto mod = Mod::get();
        mod->setSettingValue<bool>("hud", !mod->getSettingValue<bool>("hud"));
    }

    void onReplay(CCObject*) {
        auto& e = fcf::Engine::get();
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (e.mode != fcf::Mode::Idle) {
            e.stop();
            fcf::Engine::notify("Stopped", true);
            return;
        }
        e.startPlayback();
        this->onResume(nullptr);
        pl->resetLevelFromStart();
    }

    void onScan(CCObject*) {
        auto& e = fcf::Engine::get();
        auto& t = fcf::Tracker::get();
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (e.mode != fcf::Mode::Idle) {
            e.stop();
            fcf::Engine::notify("Scan cancelled (results so far are kept)", true);
            return;
        }
        if (!t.calibrated || t.stepSec > 0.0075) {
            fcf::Engine::notify("Play the level for a second first so the step length can be measured", false);
            return;
        }
        e.startScan(t);
        this->onResume(nullptr);
        static_cast<FcfPlayLayer*>(pl)->startTrial();
    }
};
