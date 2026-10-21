// Predictive Input Buffer - Geode mod for Geometry Dash (2.2081)
//
// What it does
// ------------
// Every jump-button release is held back until the click has lasted at least N
// physics ticks (1 tick = 1/240 s, so 4 ticks = 1 frame at 60 fps). A click you
// make slightly BEFORE landing / touching an orb therefore still counts as "held"
// when the game checks, instead of being lost.
//
//   STAGE 1: 2 ticks  All | 2 ticks Wave
//   STAGE 2: 4 ticks  All | 4 ticks Wave
//   STAGE 3: 8 ticks  All | 8 ticks Wave
//   STAGE 4: 16 ticks All | 8 ticks Wave
//   STAGE 5: OFF (100% raw input)
//
// F6 (while in a level) cycles the stage.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <chrono>
#include <string>

using namespace geode::prelude;

namespace ib {

	using Clock = std::chrono::steady_clock;

	constexpr double kTick = 1.0 / 240.0;   // one physics tick, in seconds
	constexpr int kJump = 1;                // PlayerButton::Jump
	constexpr int kFailsafeMs = 250;        // never hold a deferred release longer than this (real time)

	struct Stage {
		int all;    // padding window for everything except the wave, in ticks
		int wave;   // padding window while a wave, in ticks
	};

	constexpr int kStageCount = 5;
	constexpr Stage kStages[kStageCount] = {
		{ 2, 2 },
		{ 4, 4 },
		{ 8, 8 },
		{ 16, 8 },
		{ 0, 0 }    // OFF
	};

	// One slot per player (0 = player 1, 1 = player 2). Only the jump button is tracked.
	struct Slot {
		bool tracking = false;      // a press was delivered and not yet released
		bool pending = false;       // the release is being held back
		bool early = false;         // the press started while airborne
		double pressedAt = 0.0;     // game clock when the press was delivered
		double releaseAt = 0.0;     // game clock when the deferred release is due
		Clock::time_point deferredAt{};
	};

	struct State {
		double clock = 0.0;         // simulated seconds, advanced in processCommands
		Slot slot[2];
	};

	inline State g;
	inline int stage = 0;           // index into kStages, always starts at STAGE 1

	inline bool flag(char const* key) {
		return Mod::get()->getSettingValue<bool>(key);
	}

	inline void resetState() {
		g = State{};
	}

	// Text for one stage, matching the console format.
	inline std::string describe(int idx) {
		auto const& s = kStages[idx];
		if (s.all == 0) {
			return fmt::format("STAGE {}: OFF (100% Raw Input)", idx + 1);
		}
		double allF = s.all / 4.0;    // frames at 60 fps
		double waveF = s.wave / 4.0;
		if (s.all == s.wave) {
			return fmt::format(
				"STAGE {}: {} Ticks All | {} Ticks Wave ({:.1f}f @ 60fps)",
				idx + 1, s.all, s.wave, allF
			);
		}
		return fmt::format(
			"STAGE {}: {} Ticks All | {} Ticks Wave ({:.1f}f / {:.1f}f @ 60fps)",
			idx + 1, s.all, s.wave, allF, waveF
		);
	}

	inline bool wanted() {
		return flag("enabled") && kStages[stage].all > 0;
	}

	inline bool isPlayLayer(GJBaseGameLayer* layer) {
		auto pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
	}

	// How many ticks the release of this click should be padded to.
	inline int ticksFor(PlayerObject* p, bool early) {
		auto const& st = kStages[stage];
		if (!p) return 0;
		if (p->m_isDart) {
			return flag("buffer-wave") ? st.wave : 0;
		}
		if (p->m_isShip || p->m_isBird || p->m_isSwing) {
			return flag("buffer-flight-modes") ? st.all : 0;
		}
		// cube, ball, robot, spider
		if (flag("only-when-airborne") && !early) return 0;
		return st.all;
	}

	inline void banner() {
		std::string sep(60, '=');
		log::info("{}", sep);
		log::info("{}", " [InputBuffer] v4.6 LOADED (Wave Spikes/Saws Trajectory Fix)");
		log::info("{}", " [InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)");
		log::info("{}", " [InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
		log::info("{}", sep);
	}

	inline void cycleStage() {
		stage = (stage + 1) % kStageCount;
		auto text = describe(stage);
		log::info("[InputBuffer] F6 Cycle -> {}", text);
		if (flag("show-toast")) {
			Notification::create(
				fmt::format("Input Buffer: {}", text),
				NotificationIcon::None,
				1.5f
			)->show();
		}
	}

} // namespace ib

$on_mod(Loaded) {
	ib::banner();
}

class $modify(IBGameLayer, GJBaseGameLayer) {

	// Deliver a release that was being held back.
	void ibRelease(int s) {
		auto& sl = ib::g.slot[s];
		sl.pending = false;
		sl.tracking = false;
		GJBaseGameLayer::handleButton(false, ib::kJump, s == 0);
	}

	// Every button event goes through here (after GD has timestamped and queued it).
	void handleButton(bool down, int button, bool isPlayer1) {
		if (button != ib::kJump || !ib::wanted() || !ib::isPlayLayer(this)) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int s = isPlayer1 ? 0 : 1;
		auto& sl = ib::g.slot[s];
		PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
		if (!p) p = m_player1;

		if (down) {
			// A new press while a release is still pending: deliver that release first
			// so the new press is a real, separate click.
			if (sl.pending) this->ibRelease(s);

			sl.tracking = true;
			sl.early = p ? !p->m_isOnGround : false;
			sl.pressedAt = ib::g.clock;
			GJBaseGameLayer::handleButton(true, button, isPlayer1);
			return;
		}

		// Release of a click we did not see being pressed (e.g. the stage was just switched on).
		if (!sl.tracking) {
			GJBaseGameLayer::handleButton(false, button, isPlayer1);
			return;
		}

		int ticks = ib::ticksFor(p, sl.early);
		double due = sl.pressedAt + ticks * ib::kTick;

		if (ticks <= 0 || ib::g.clock >= due) {
			sl.tracking = false;
			sl.pending = false;
			GJBaseGameLayer::handleButton(false, button, isPlayer1);
			return;
		}

		sl.pending = true;
		sl.releaseAt = due;
		sl.deferredAt = ib::Clock::now();

		if (ib::flag("debug-log")) {
			double heldTicks = (ib::g.clock - sl.pressedAt) / ib::kTick;
			log::info(
				"[InputBuffer] release deferred: held {:.1f} ticks -> padded to {} ticks (early={}, wave={})",
				heldTicks, ticks, sl.early, p ? p->m_isDart : false
			);
		}
	}

	// Runs once per physics step: advance our clock and deliver releases that are due.
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (ib::isPlayLayer(this)) {
			ib::g.clock += dt;
			bool wanted = ib::wanted();
			for (int s = 0; s < 2; ++s) {
				auto& sl = ib::g.slot[s];
				if (sl.pending && (!wanted || ib::g.clock >= sl.releaseAt)) {
					this->ibRelease(s);
				}
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	// Failsafe: if the step clock ever stops advancing, never leave a release stuck.
	void update(float dt) {
		GJBaseGameLayer::update(dt);
		if (!ib::isPlayLayer(this)) return;

		auto now = ib::Clock::now();
		for (int s = 0; s < 2; ++s) {
			auto& sl = ib::g.slot[s];
			if (sl.pending && now - sl.deferredAt > std::chrono::milliseconds(ib::kFailsafeMs)) {
				this->ibRelease(s);
			}
		}
	}
};

class $modify(IBPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		ib::resetState();
	}
};

class $modify(IBKeyboard, CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
		if (down && !repeat && key == enumKeyCodes::KEY_F6 && PlayLayer::get()) {
			ib::cycleStage();
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
	}
};
