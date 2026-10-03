#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <chrono>
#include <algorithm>
#include <deque>
#include <vector>
using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings helpers
// ---------------------------------------------------------------------------
static int offsetMs() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("offset-ms"));
}
static bool rollbackOn() {
    return Mod::get()->getSettingValue<bool>("rollback-enabled");
}

// ---------------------------------------------------------------------------
// Calibration screen: tap any key on each click, median gap = your offset
// ---------------------------------------------------------------------------
class CalibrationLayer : public CCLayer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastBeat = Clock::now();
    std::vector<double> m_diffs;
    int m_beats = 0;
    CCLabelBMFont* m_label = nullptr;
    static constexpr float INTERVAL = 0.6f;
    static constexpr int WARMUP = 2, TOTAL = 10;

    bool init() override {
        if (!CCLayer::init()) return false;
        this->setKeyboardEnabled(true);
        auto size = CCDirector::get()->getWinSize();
        this->addChild(CCLayerColor::create({20, 20, 30, 255}));
        m_label = CCLabelBMFont::create("Press any key on each click\n(Esc to cancel)", "bigFont.fnt");
        m_label->setScale(0.6f);
        m_label->setPosition(size / 2);
        this->addChild(m_label);
        this->schedule(schedule_selector(CalibrationLayer::onBeat), INTERVAL);
        return true;
    }

    void onBeat(float) {
        FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg");
        m_lastBeat = Clock::now();
        if (++m_beats > TOTAL) finish();
    }

    void keyDown(enumKeyCodes key) override {
        if (key == KEY_Escape) return close();
        if (m_beats < 1) return;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - m_lastBeat).count();
        if (ms > INTERVAL * 500) ms -= INTERVAL * 1000; // pressed early for the next beat
        if (m_beats > WARMUP) m_diffs.push_back(ms);
    }

    void finish() {
        this->unschedule(schedule_selector(CalibrationLayer::onBeat));
        if (m_diffs.empty()) return close();
        std::sort(m_diffs.begin(), m_diffs.end());
        int ms = std::clamp(static_cast<int>(m_diffs[m_diffs.size() / 2]), 0, 500);
        Mod::get()->setSettingValue<int64_t>("offset-ms", ms);
        m_label->setString(fmt::format("Offset set to {} ms", ms).c_str());
        this->runAction(CCSequence::create(
            CCDelayTime::create(1.5f),
            CCCallFunc::create(this, callfunc_selector(CalibrationLayer::close)),
            nullptr));
    }

    void close() {
        CCDirector::get()->popSceneWithTransition(0.3f, PopTransition::kPopTransitionFade);
    }

public:
    static void open() {
        auto layer = new CalibrationLayer();
        if (layer && layer->init()) {
            layer->autorelease();
            auto scene = CCScene::create();
            scene->addChild(layer);
            CCDirector::get()->pushScene(CCTransitionFade::create(0.3f, scene));
        } else CC_SAFE_DELETE(layer);
    }
};

class $modify(SyncMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Calibrate"), this, menu_selector(SyncMenu::onCalibrate));
        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        return true;
    }
    void onCalibrate(CCObject*) { CalibrationLayer::open(); }
};

// ---------------------------------------------------------------------------
// Rollback state
// ---------------------------------------------------------------------------
struct Snap {
    double t;             // ms on our own clock when taken
    CheckpointObject* cp; // retained
};

static std::deque<Snap> g_snaps;
static double g_clock = 0.0;      // ms of level time we've simulated
static int g_frame = 0;
static bool g_fromKeyboard = false;
static bool g_resim = false;

static void clearSnaps() {
    for (auto& s : g_snaps) s.cp->release();
    g_snaps.clear();
}

// Flag keyboard-originated input. Custom Keybinds fires its binds from inside
// this dispatch, so remapped keys are covered too. Touch input never passes here.
class $modify(SyncKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        g_fromKeyboard = true;
        bool r = CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
        g_fromKeyboard = false;
        return r;
    }
};

// Take snapshots as the level runs
class $modify(SyncPlay, PlayLayer) {
    void update(float dt) {
        PlayLayer::update(dt);
        if (g_resim) return;

        g_clock += dt * 1000.0;

        int ms = offsetMs();
        if (ms <= 0 || !rollbackOn()) {
            if (!g_snaps.empty()) clearSnaps();
            return;
        }

        // snapshot every 2nd frame to keep it light on a tablet
        if ((++g_frame & 1) == 0) {
            if (auto cp = this->createCheckpoint()) {
                cp->retain();
                g_snaps.push_back({g_clock, cp});
            }
        }

        // drop snapshots older than offset + margin
        double keepFrom = g_clock - ms - 150.0;
        while (!g_snaps.empty() && g_snaps.front().t < keepFrom) {
            g_snaps.front().cp->release();
            g_snaps.pop_front();
        }
    }

    void resetLevel() {
        clearSnaps();
        g_clock = 0.0;
        g_frame = 0;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        clearSnaps();
        g_clock = 0.0;
        g_frame = 0;
        PlayLayer::onQuit();
    }
};

// The actual rollback: rewind `offset` ms, apply the press there, re-sim to now
class $modify(SyncGame, GJBaseGameLayer) {
    void queueButton(int button, bool down, bool isPlayer1) {
        int ms = offsetMs();
        auto pl = PlayLayer::get();

        bool eligible = g_fromKeyboard && !g_resim && ms > 0 && rollbackOn()
            && pl && static_cast<GJBaseGameLayer*>(pl) == this && !g_snaps.empty();

        if (!eligible) {
            GJBaseGameLayer::queueButton(button, down, isPlayer1);
            return;
        }

        // newest snapshot at or before (now - offset)
        double target = g_clock - ms;
        const Snap* pick = nullptr;
        for (auto it = g_snaps.rbegin(); it != g_snaps.rend(); ++it) {
            if (it->t <= target) { pick = &*it; break; }
        }
        if (!pick) {
            GJBaseGameLayer::queueButton(button, down, isPlayer1);
            return;
        }

        double pickT = pick->t;
        CheckpointObject* cp = pick->cp;
        double remaining = g_clock - pickT;

        g_resim = true;
        pl->loadFromCheckpoint(cp);
        GJBaseGameLayer::queueButton(button, down, isPlayer1);

        // re-simulate back up to the present in ~60fps steps
        constexpr double STEP = 1000.0 / 60.0;
        while (remaining > 0.5) {
            double s = std::min(remaining, STEP);
            pl->update(static_cast<float>(s / 1000.0));
            remaining -= s;
        }
        g_resim = false;

        // snapshots newer than the one we rewound to belong to the old timeline
        while (!g_snaps.empty() && g_snaps.back().t > pickT) {
            g_snaps.back().cp->release();
            g_snaps.pop_back();
        }
    }
};
