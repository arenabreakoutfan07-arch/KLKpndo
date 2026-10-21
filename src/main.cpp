#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ============================================================
//  Optimized Predictive Buffer v1.2.0
//  - Always on by default (Normal + Practice)
//  - True Off switch
//  - Per-mode toggles
//  - Ticks 1-8 (same for Wave)
//  - Dual off by default
//  - NO simulation cap (removed)
//  - Reuses 2 ghost players
//  - No particles during sim
//  - Fully disabled in editor (fixes crash)
// ============================================================

namespace {

constexpr float TICK_DT = 0.25f; // 240 TPS normalized

struct BufferSettings {
    bool enabled = true;
    int maxTicks = 2;
    bool dual = false;
    bool showIndicator = false;

    bool modeCube = true;
    bool modeShip = true;
    bool modeBall = true;
    bool modeUfo = true;
    bool modeWave = true;
    bool modeRobot = true;
    bool modeSpider = true;
    bool modeSwing = true;

    void load() {
        auto mod = Mod::get();
        enabled       = mod->getSettingValue<bool>("enabled");
        maxTicks      = mod->getSettingValue<int64_t>("max-ticks");
        dual          = mod->getSettingValue<bool>("dual-enabled");
        showIndicator = mod->getSettingValue<bool>("show-indicator");

        modeCube   = mod->getSettingValue<bool>("mode-cube");
        modeShip   = mod->getSettingValue<bool>("mode-ship");
        modeBall   = mod->getSettingValue<bool>("mode-ball");
        modeUfo    = mod->getSettingValue<bool>("mode-ufo");
        modeWave   = mod->getSettingValue<bool>("mode-wave");
        modeRobot  = mod->getSettingValue<bool>("mode-robot");
        modeSpider = mod->getSettingValue<bool>("mode-spider");
        modeSwing  = mod->getSettingValue<bool>("mode-swing");

        maxTicks = std::clamp(maxTicks, 1, 8);
    }
};

BufferSettings g_settings;

// Spam guard for Wave (still useful, but soft)
constexpr int SPAM_INTERVAL = 6;
constexpr int SPAM_STREAK_NEEDED = 4;

class TrajectorySim {
public:
    PlayerObject* m_ghost1 = nullptr;
    PlayerObject* m_ghost2 = nullptr;

    bool m_simulating = false;
    bool m_simDead = false;
    bool m_firing = false;

    // Buffer state
    int  m_waitTicks = -1;          // -1 = idle, >=0 = countdown, -2 = queued opposite
    bool m_bufferedPush = true;
    bool m_queuedOpposite = false;
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // Spam
    int m_ticksSinceLast = 999;
    int m_rapidStreak = 0;

    // Indicator
    int m_lastDelayedTicks = 0;
    cocos2d::CCLabelBMFont* m_indicator = nullptr;

    float m_dt = TICK_DT;

    bool isInEditor() const {
        return LevelEditorLayer::get() != nullptr;
    }

    void ensureGhosts(PlayLayer* pl) {
        if (!pl || isInEditor()) return;
        if (!m_ghost1) {
            m_ghost1 = PlayerObject::create(1, 1, pl, pl, true);
            if (m_ghost1) {
                m_ghost1->retain();
                m_ghost1->setVisible(false);
                m_ghost1->setPosition({-9999.f, -9999.f});
                pl->m_objectLayer->addChild(m_ghost1);
            }
        }
        if (!m_ghost2) {
            m_ghost2 = PlayerObject::create(1, 1, pl, pl, true);
            if (m_ghost2) {
                m_ghost2->retain();
                m_ghost2->setVisible(false);
                m_ghost2->setPosition({-9999.f, -9999.f});
                pl->m_objectLayer->addChild(m_ghost2);
            }
        }
    }

    void destroyGhosts() {
        if (m_ghost1) {
            m_ghost1->removeFromParentAndCleanup(true);
            m_ghost1->release();
            m_ghost1 = nullptr;
        }
        if (m_ghost2) {
            m_ghost2->removeFromParentAndCleanup(true);
            m_ghost2->release();
            m_ghost2 = nullptr;
        }
        if (m_indicator) {
            m_indicator->removeFromParentAndCleanup(true);
            m_indicator = nullptr;
        }
        resetState();
    }

    void resetState() {
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_simulating = false;
        m_firing = false;
        m_simDead = false;
        m_physicalHolding = false;
        m_gameHolding = false;
        m_ticksSinceLast = 999;
        m_rapidStreak = 0;
        m_lastDelayedTicks = 0;
    }

    void syncGhost(PlayerObject* ghost, PlayerObject* real) {
        if (!ghost || !real) return;
        ghost->setVisible(false);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        if (ghost->m_collisionLogTop)    ghost->m_collisionLogTop->removeAllObjects();
        if (ghost->m_collisionLogBottom) ghost->m_collisionLogBottom->removeAllObjects();
        if (ghost->m_collisionLogLeft)   ghost->m_collisionLogLeft->removeAllObjects();
        if (ghost->m_collisionLogRight)  ghost->m_collisionLogRight->removeAllObjects();
    }

    bool isModeEnabled(PlayerObject* p) const {
        if (!p) return false;
        if (p->m_isShip)   return g_settings.modeShip;
        if (p->m_isBird)   return g_settings.modeUfo;
        if (p->m_isBall)   return g_settings.modeBall;
        if (p->m_isDart)   return g_settings.modeWave;
        if (p->m_isRobot)  return g_settings.modeRobot;
        if (p->m_isSpider) return g_settings.modeSpider;
        if (p->m_isSwing)  return g_settings.modeSwing;
        return g_settings.modeCube;
    }

    // Returns how many ticks the action would survive
    int simulateSurvival(PlayLayer* pl, PlayerObject* real1, PlayerObject* real2,
                         bool isDual, bool actionPush, int maxTicks) {
        if (!pl || !m_ghost1 || !real1 || maxTicks <= 0 || isInEditor()) return 0;

        m_simulating = true;
        m_simDead = false;

        syncGhost(m_ghost1, real1);
        if (isDual && m_ghost2 && real2) {
            syncGhost(m_ghost2, real2);
        }

        // Apply opposite first
        if (actionPush) {
            m_ghost1->releaseButton(PlayerButton::Jump);
            if (isDual && m_ghost2) m_ghost2->releaseButton(PlayerButton::Jump);
        } else {
            m_ghost1->pushButton(PlayerButton::Jump);
            if (isDual && m_ghost2) m_ghost2->pushButton(PlayerButton::Jump);
        }

        float dt = (m_dt > 0.001f && m_dt < 2.f) ? m_dt : TICK_DT;
        int survived = 0;
        int totalSteps = maxTicks + 2;

        for (int i = 0; i < totalSteps; ++i) {
            if (i == 0) {
                if (actionPush) {
                    m_ghost1->pushButton(PlayerButton::Jump);
                    if (isDual && m_ghost2) m_ghost2->pushButton(PlayerButton::Jump);
                } else {
                    m_ghost1->releaseButton(PlayerButton::Jump);
                    if (isDual && m_ghost2) m_ghost2->releaseButton(PlayerButton::Jump);
                }
            }

            m_ghost1->update(dt);
            m_ghost1->updateRotation(dt);
            if (isDual && m_ghost2) {
                m_ghost2->update(dt);
                m_ghost2->updateRotation(dt);
            }

            if (m_simDead) break;

            if (m_ghost1->m_collisionLogTop)    m_ghost1->m_collisionLogTop->removeAllObjects();
            if (m_ghost1->m_collisionLogBottom) m_ghost1->m_collisionLogBottom->removeAllObjects();
            pl->checkCollisions(m_ghost1, dt, false);
            if (m_simDead) break;

            if (isDual && m_ghost2 && real2) {
                if (m_ghost2->m_collisionLogTop)    m_ghost2->m_collisionLogTop->removeAllObjects();
                if (m_ghost2->m_collisionLogBottom) m_ghost2->m_collisionLogBottom->removeAllObjects();
                pl->checkCollisions(m_ghost2, dt, false);
                if (m_simDead) break;
            }

            survived++;
            if (survived >= maxTicks) break;
        }

        m_simulating = false;
        return survived;
    }

    void fireButton(PlayLayer* pl, bool push) {
        if (!pl || !pl->m_player1 || isInEditor()) return;
        m_firing = true;
        if (push) {
            pl->m_player1->pushButton(PlayerButton::Jump);
            if (g_settings.dual && pl->m_gameState.m_isDualMode && pl->m_player2) {
                pl->m_player2->pushButton(PlayerButton::Jump);
            }
        } else {
            pl->m_player1->releaseButton(PlayerButton::Jump);
            if (g_settings.dual && pl->m_gameState.m_isDualMode && pl->m_player2) {
                pl->m_player2->releaseButton(PlayerButton::Jump);
            }
        }
        m_firing = false;
        m_gameHolding = push;
    }

    void updateIndicator(PlayLayer* pl) {
        if (!g_settings.showIndicator || !pl || isInEditor()) {
            if (m_indicator) m_indicator->setVisible(false);
            return;
        }

        if (!m_indicator) {
            m_indicator = cocos2d::CCLabelBMFont::create("0", "bigFont.fnt");
            if (m_indicator) {
                m_indicator->setScale(0.25f);
                m_indicator->setOpacity(180);
                m_indicator->setAnchorPoint({0.f, 1.f});
                m_indicator->setPosition({8.f, pl->getContentSize().height - 8.f});
                pl->addChild(m_indicator, 999);
            }
        }
        if (m_indicator) {
            m_indicator->setVisible(true);
            m_indicator->setString(std::to_string(m_lastDelayedTicks).c_str());
        }
    }

    // Returns true if the original input should still be processed
    bool intercept(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        // Never run in editor
        if (isInEditor()) return true;

        PlayLayer* pl = typeinfo_cast<PlayLayer*>(layer);
        if (!pl) return true;

        if (!g_settings.enabled) {
            m_gameHolding = push;
            return true;
        }

        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        bool twoPlayer = pl->m_level && pl->m_level->m_twoPlayerMode;

        if (!isPlayer1) {
            if (isDual && !twoPlayer && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        m_physicalHolding = push;

        PlayerObject* p1 = pl->m_player1;
        if (!p1 || p1->m_isDead) {
            resetState();
            m_gameHolding = push;
            return true;
        }

        if (!isModeEnabled(p1)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // Already waiting
        if (m_waitTicks >= 0) {
            if (push != m_bufferedPush) {
                m_queuedOpposite = true;
            } else {
                m_queuedOpposite = false;
            }
            m_ticksSinceLast = 0;
            return false;
        }

        // Queued opposite due
        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != !m_bufferedPush) {
                fireButton(pl, !m_bufferedPush);
            }
        }

        // Soft Wave spam guard
        bool isWave = p1->m_isDart;
        if (isWave) {
            if (m_ticksSinceLast < SPAM_INTERVAL) {
                m_rapidStreak++;
            } else {
                m_rapidStreak = 0;
            }
            m_ticksSinceLast = 0;

            if (m_rapidStreak >= SPAM_STREAK_NEEDED) {
                m_gameHolding = push;
                return true; // raw input on heavy spam
            }
        } else {
            m_ticksSinceLast = 0;
            m_rapidStreak = 0;
        }

        int maxT = g_settings.maxTicks;
        if (maxT <= 0) {
            m_gameHolding = push;
            return true;
        }

        ensureGhosts(pl);
        int survived = simulateSurvival(pl, p1, pl->m_player2, isDual && g_settings.dual, push, maxT);

        if (survived >= maxT) {
            m_lastDelayedTicks = 0;
            m_gameHolding = push;
            return true;
        }

        // Buffer it
        int delay = maxT - survived;
        if (delay < 1) delay = 1;
        if (delay > maxT) delay = maxT;

        m_waitTicks = delay;
        m_bufferedPush = push;
        m_queuedOpposite = false;
        m_lastDelayedTicks = delay;

        return false; // swallow
    }

    void onTick(PlayLayer* pl) {
        if (!pl || !g_settings.enabled || isInEditor()) return;

        m_ticksSinceLast++;
        if (m_ticksSinceLast > 1000) m_ticksSinceLast = 1000;

        if (m_waitTicks > 0) {
            m_waitTicks--;
            if (m_waitTicks == 0) {
                fireButton(pl, m_bufferedPush);
                m_waitTicks = -1;

                if (m_queuedOpposite) {
                    m_waitTicks = -2;
                    m_queuedOpposite = false;
                }
            }
        }

        updateIndicator(pl);
    }

    bool isSimulating() const { return m_simulating; }
    bool isFiring() const { return m_firing; }
};

TrajectorySim g_sim;

} // namespace

// ============================================================
// Hooks
// ============================================================

class $modify(BufPlayLayer, PlayLayer) {
    struct Fields {
        bool inited = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Never init ghosts / state while in editor
        if (LevelEditorLayer::get()) {
            m_fields->inited = false;
            return true;
        }

        g_settings.load();
        g_sim.ensureGhosts(this);
        g_sim.resetState();
        m_fields->inited = true;
        return true;
    }

    void onQuit() {
        g_sim.destroyGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        if (!LevelEditorLayer::get()) {
            g_sim.resetState();
        }
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (g_sim.isSimulating() && (player == g_sim.m_ghost1 || player == g_sim.m_ghost2)) {
            g_sim.m_simDead = true;
            return;
        }
        if (!LevelEditorLayer::get()) {
            g_sim.resetState();
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void update(float dt) {
        if (this->m_gameState.m_timeWarp > 0.f) {
            g_sim.m_dt = dt / this->m_gameState.m_timeWarp;
        } else {
            g_sim.m_dt = dt;
        }

        PlayLayer::update(dt);

        if (m_fields->inited && !LevelEditorLayer::get()) {
            g_sim.onTick(this);
        }
    }
};

class $modify(BufGJBase, GJBaseGameLayer) {
    void handleButton(bool push, int player, bool playerButton) {
        if (!playerButton) {
            GJBaseGameLayer::handleButton(push, player, playerButton);
            return;
        }

        // Completely skip in editor
        if (LevelEditorLayer::get()) {
            GJBaseGameLayer::handleButton(push, player, playerButton);
            return;
        }

        if (g_sim.isFiring()) {
            GJBaseGameLayer::handleButton(push, player, playerButton);
            return;
        }

        bool isP1 = (player == 1 || player == 0);
        bool allow = g_sim.intercept(this, push, isP1);

        if (allow) {
            GJBaseGameLayer::handleButton(push, player, playerButton);
        }
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    void playDeathEffect() {
        if (g_sim.isSimulating()) return;
        PlayerObject::playDeathEffect();
    }
};

class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(g_settings.enabled ? "Buf ON" : "Buf OFF", 40, true, "goldFont.fnt", "GJ_button_01.png", 28.f, 0.6f),
            this,
            menu_selector(BufPauseLayer::onToggleBuffer)
        );
        btn->setID("predictive-buffer-toggle"_spr);

        CCMenu* menu = typeinfo_cast<CCMenu*>(this->getChildByID("center-menu"));
        if (!menu) menu = this->getChildByType<CCMenu>(0);
        if (menu) {
            btn->setPosition({0.f, -110.f});
            menu->addChild(btn);
        }
    }

    void onToggleBuffer(CCObject*) {
        g_settings.enabled = !g_settings.enabled;
        Mod::get()->setSettingValue("enabled", g_settings.enabled);

        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(this->getChildByIDRecursive("predictive-buffer-toggle"_spr))) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setString(g_settings.enabled ? "Buf ON" : "Buf OFF");
            }
        }
    }
};

$on_mod(Loaded) {
    g_settings.load();
    log::info("[PredictiveBufferOpt] v1.2.0 loaded. Ticks={}, Dual={}, Editor-safe",
              g_settings.maxTicks, g_settings.dual);
}

$on_mod(DataLoaded) {
    g_settings.load();
}
