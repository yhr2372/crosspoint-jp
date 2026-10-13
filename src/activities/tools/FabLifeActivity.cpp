#include "FabLifeActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kLogPath = "/fab_life_log.txt";

// 数字の墨のベースラインからの高さ（lifecount_*_bold.h の字形表から。縦の中央寄せに使う）
constexpr int kDigitInk150 = 226;
constexpr int kDigitInk100 = 151;

// 前面ボタン（横向きCCWで上から Left, Right, Confirm, Back）と、増減する側・向き
struct FrontKey {
  MappedInputManager::Button button;
  int player;  // 0=左(自分) 1=右(相手)
  int sign;
};
constexpr FrontKey kFrontKeys[4] = {
    {MappedInputManager::Button::Left, 1, -1},
    {MappedInputManager::Button::Right, 1, +1},
    {MappedInputManager::Button::Confirm, 0, +1},
    {MappedInputManager::Button::Back, 0, -1},
};
}  // namespace

void FabLifeActivity::onEnter() {
  Activity::onEnter();
  life[0] = life[1] = startLife;
  for (auto& h : history) {
    h.clear();
    h.push_back(static_cast<int16_t>(startLife));
  }
  lastChangeMs[0] = lastChangeMs[1] = 0;
  state = State::PLAY;
  heldKey = -1;
  upArmed = false;
  firstRender = true;
  requestUpdate();
}

void FabLifeActivity::setState(const State s) {
  state = s;
  heldKey = -1;
  upArmed = false;
  requestUpdate();
}

void FabLifeActivity::adjust(const int player, const int delta) {
  const int next = std::clamp(life[player] + delta, 0, kMaxLife);
  if (next == life[player]) return;
  life[player] = next;

  auto& h = history[player];
  const unsigned long now = millis();
  if (h.size() > 1 && now - lastChangeMs[player] < kSettleMs) {
    h.back() = static_cast<int16_t>(next);  // 連続操作は 1 件にまとめる
  } else {
    if (h.size() >= kMaxHistory) h.erase(h.begin() + 1);
    h.push_back(static_cast<int16_t>(next));
  }
  lastChangeMs[player] = now;
  requestUpdate();
}

// 今のゲームを SD に 1 行で追記して、新しいゲームを始める
void FabLifeActivity::saveGameLog() const {
  std::string line = "game " + std::to_string(gameNumber);
  const time_t t = time(nullptr);
  if (t > 1600000000) {  // RTC が有効なときだけ日時を付ける
    struct tm ti;
    localtime_r(&t, &ti);
    char buf[64];  // 範囲外の値でも切り詰めない大きさ
    snprintf(buf, sizeof(buf), " %04d/%02d/%02d %02d:%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour,
             ti.tm_min);
    line += buf;
  }
  const char* names[2] = {" | left:", " | right:"};
  for (int p = 0; p < 2; p++) {
    line += names[p];
    for (size_t i = 0; i < history[p].size(); i++) {
      line += (i == 0 ? " " : ">");
      line += std::to_string(history[p][i]);
    }
  }
  line += "\n";

  auto file = Storage.open(kLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (file) {
    file.write(line.c_str(), line.size());
    file.close();
  }
}

void FabLifeActivity::newGame() {
  if (!untouched()) {
    saveGameLog();
    gameNumber++;
  }
  life[0] = life[1] = startLife;
  for (auto& h : history) {
    h.clear();
    h.push_back(static_cast<int16_t>(startLife));
  }
  lastChangeMs[0] = lastChangeMs[1] = 0;
}

void FabLifeActivity::loop() {
  switch (state) {
    case State::PLAY:
      loopPlay();
      break;
    case State::HISTORY:
      loopHistory();
      break;
    case State::MENU:
      loopMenu();
      break;
  }
}

void FabLifeActivity::loopPlay() {
  using B = MappedInputManager::Button;

  // 前面ボタン: 押した瞬間に ±1。押し続けると kLongPressMs で合計 ±5、
  // 以降 kRepeatMs ごとに ±5 ずつ足す（離すのを待たない）
  for (int i = 0; i < 4; i++) {
    if (mappedInput.wasPressed(kFrontKeys[i].button)) {
      heldKey = i;
      heldSteps = 0;
      adjust(kFrontKeys[i].player, kFrontKeys[i].sign);
      return;
    }
  }
  if (heldKey >= 0) {
    const FrontKey& k = kFrontKeys[heldKey];
    if (!mappedInput.isPressed(k.button)) {
      heldKey = -1;
    } else {
      const unsigned long held = mappedInput.getHeldTime();
      const int due = held >= kLongPressMs ? 1 + static_cast<int>((held - kLongPressMs) / kRepeatMs) : 0;
      while (heldSteps < due) {
        adjust(k.player, k.sign * (heldSteps == 0 ? kLongStep - 1 : kLongStep));
        heldSteps++;
      }
    }
  }

  // 側面 Up: 押し続けて kMenuHoldMs に達した時点でメニュー。それより前に離したら履歴
  if (mappedInput.wasPressed(B::Up)) {
    upArmed = true;
  }
  if (upArmed) {
    if (!mappedInput.isPressed(B::Up)) {
      setState(State::HISTORY);
    } else if (mappedInput.getHeldTime() >= kMenuHoldMs) {
      menuIndex = 0;
      setState(State::MENU);
    }
    return;
  }

  if (mappedInput.wasPressed(B::Down)) {
    finish();
  }
}

void FabLifeActivity::loopHistory() {
  using B = MappedInputManager::Button;
  // Up（または Back / Down）で閉じる
  if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Back) || mappedInput.wasPressed(B::Down)) {
    setState(State::PLAY);
  }
}

void FabLifeActivity::loopMenu() {
  using B = MappedInputManager::Button;

  if (mappedInput.wasPressed(B::Left)) {
    menuIndex = (menuIndex + kMenuItems - 1) % kMenuItems;
    requestUpdate();
  } else if (mappedInput.wasPressed(B::Right)) {
    menuIndex = (menuIndex + 1) % kMenuItems;
    requestUpdate();
  } else if (mappedInput.wasPressed(B::Confirm)) {
    switch (menuIndex) {
      case 0:  // 初期ライフ 20 <-> 40
        startLife = (startLife == 20) ? 40 : 20;
        if (untouched()) {  // まだ何も操作していなければ、そのまま今のゲームにも反映
          newGame();
        }
        requestUpdate();
        break;
      case 1:  // 新規ゲーム
        newGame();
        setState(State::PLAY);
        break;
      case 2:  // 履歴
        setState(State::HISTORY);
        break;
      case 3:  // 終了
        finish();
        break;
    }
  } else if (mappedInput.wasPressed(B::Back) || mappedInput.wasPressed(B::Up)) {
    setState(State::PLAY);
  } else if (mappedInput.wasPressed(B::Down)) {
    finish();
  }
}

// パネル (px,py,pw,ph) の中央に数字を描く。flipped なら 180° 回して描く
void FabLifeActivity::drawNumber(int value, int px, int py, int pw, int ph, bool flipped) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  // 2 桁までは 150pt、3 桁は 100pt（どちらも 1 パネルの幅に収まる大きさ）
  const bool big = strlen(buf) <= 2;
  const int fontId = big ? LIFECOUNT_150_FONT_ID : LIFECOUNT_100_FONT_ID;
  const int ink = big ? kDigitInk150 : kDigitInk100;

  // 横向きの反対側（CW）に一時的に切り替えると、同じ枠の中に 180° 回った文字が描ける。
  // CCW の (x,y) は CW では (W-x, H-y) なので、枠もそれに合わせて写す
  const auto saved = renderer.getOrientation();
  if (flipped) {
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
    px = renderer.getScreenWidth() - (px + pw);
    py = renderer.getScreenHeight() - (py + ph);
  }

  const int w = renderer.getTextWidth(fontId, buf);
  const int bearing = renderer.getTextLeftBearing(fontId, buf);
  const int baseline = py + (ph - ink) / 2 + ink;
  renderer.drawText(fontId, px + (pw - w) / 2 - bearing, baseline - renderer.getFontAscenderSize(fontId), buf, true);

  renderer.setOrientation(saved);
}

void FabLifeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case State::PLAY:
      renderPlay();
      break;
    case State::HISTORY:
      renderHistory();
      break;
    case State::MENU:
      renderMenu();
      break;
  }

  // 反応の速さを優先して常に高速更新。開いた直後だけ前の画面の残像を消す
  renderer.displayBuffer(firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}

void FabLifeActivity::renderPlay() {
  // ボタンヒント領域を除いた矩形（横向きでは右端がヒント）
  const Rect area = UITheme::getContentArea(renderer);
  const int half = area.width / 2;

  // 左=自分(正立) 右=相手(180°回転)
  drawNumber(life[0], area.x, area.y, half, area.height, false);
  drawNumber(life[1], area.x + half, area.y, area.width - half, area.height, true);

  // 中央の区切り線
  renderer.fillRect(area.x + half - 1, area.y + 20, 2, area.height - 40, true);

  // 上から - + / + -（右=相手, 左=自分）
  const auto labels = mappedInput.mapLabels("-", "+", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FabLifeActivity::renderHistory() {
  const Rect area = UITheme::getContentArea(renderer);
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int half = area.width / 2;
  const int top = area.y + 10;

  renderer.drawText(UI_10_FONT_ID, area.x + 10, top, "History");
  renderer.drawText(UI_10_FONT_ID, area.x + 10, top + lh + 6, "LEFT");
  renderer.drawText(UI_10_FONT_ID, area.x + half + 10, top + lh + 6, "RIGHT");
  renderer.fillRect(area.x, top + 2 * lh + 10, area.width, 2, true);
  renderer.fillRect(area.x + half - 1, top + lh + 6, 2, area.height - (lh + 16), true);

  const int rowsTop = top + 2 * lh + 18;
  const int maxRows = std::max(1, (area.y + area.height - rowsTop - 6) / lh);

  for (int p = 0; p < 2; p++) {
    const auto& h = history[p];
    const size_t n = h.size();
    const size_t first = n > static_cast<size_t>(maxRows) ? n - maxRows : 0;  // 最新 maxRows 件
    int y = rowsTop;
    for (size_t i = first; i < n; i++) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", h[i]);
      renderer.drawText(UI_10_FONT_ID, area.x + p * half + 10, y, buf);
      y += lh;
    }
  }

  const auto labels = mappedInput.mapLabels("Close", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FabLifeActivity::renderMenu() {
  const Rect area = UITheme::getContentArea(renderer);
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowH = lh + 18;
  int y = area.y + 14;

  renderer.drawText(UI_10_FONT_ID, area.x + 14, y, "Menu");
  y += lh + 18;

  char startBuf[40];
  snprintf(startBuf, sizeof(startBuf), "Start life: %d  (OK = 20/40)", startLife);
  const char* items[kMenuItems] = {startBuf, "New game (save current)", "History", "Exit"};

  for (int i = 0; i < kMenuItems; i++) {
    if (i == menuIndex) {
      renderer.fillRect(area.x + 8, y - 6, area.width - 16, rowH, true);
      renderer.drawText(UI_10_FONT_ID, area.x + 20, y, items[i], false);  // 反転表示
    } else {
      renderer.drawText(UI_10_FONT_ID, area.x + 20, y, items[i]);
    }
    y += rowH;
  }

  const auto labels = mappedInput.mapLabels("Close", "OK", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
