#pragma once
#include <cstdint>
#include <vector>

#include "../Activity.h"

// Flesh and Blood 用ライフカウンター。
// 横向き(LandscapeCCW)固定。画面左が自分、右は対面の相手向けに 180° 回転して表示する。
//
// 前面ボタン（右端、上→下）: [-][+] = 右(相手)のライフ / [+][-] = 左(自分)のライフ（長押しで ±5）
// 側面 Up   : 短押し = ライフ履歴 ⇔ 閉じる、長押し(0.7秒) = メニュー
// 側面 Down : ホームへ戻る
// メニュー  : 初期ライフ(20/40) / 新規ゲーム(今のゲームを SD に記録して開始) / 履歴 / 終了
class FabLifeActivity final : public Activity {
  enum class State : uint8_t { PLAY, HISTORY, MENU };

  static constexpr int kMaxLife = 999;
  static constexpr int kLongPressMs = 500;   // ±5 になる長押し
  static constexpr int kLongStep = 5;
  static constexpr int kMenuHoldMs = 700;    // 側面 Up 長押しでメニュー
  static constexpr int kSettleMs = 4000;     // この間隔内の連続操作は履歴 1 件にまとめる
  static constexpr size_t kMaxHistory = 60;
  static constexpr int kMenuItems = 4;

  State state = State::PLAY;
  int life[2] = {20, 20};  // [0]=左(自分) [1]=右(相手)
  int startLife = 20;      // 20 or 40
  int menuIndex = 0;
  int gameNumber = 1;
  int updatesSinceFull = 0;
  bool forceHalf = false;
  std::vector<int16_t> history[2];
  unsigned long lastChangeMs[2] = {0, 0};

  void adjust(int player, int delta);
  void newGame();
  void saveGameLog() const;
  bool untouched() const { return history[0].size() == 1 && history[1].size() == 1; }
  void setState(State s);

  void loopPlay();
  void loopHistory();
  void loopMenu();
  void renderPlay();
  void renderHistory();
  void renderMenu();

  void drawNumber(int value, int px, int py, int pw, int ph, bool flipped) const;
  void fillLocal(int px, int py, int pw, int ph, bool flipped, int x, int y, int w, int h) const;

 public:
  explicit FabLifeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FabLife", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool forceLandscapeCCW() const override { return true; }
  bool preventAutoSleep() override { return true; }
};
