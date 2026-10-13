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
// 7セグ: bit0=a(上) 1=b(右上) 2=c(右下) 3=d(下) 4=e(左下) 5=f(左上) 6=g(中)
constexpr uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
constexpr const char* kLogPath = "/fab_life_log.txt";
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
  requestUpdate();
}

void FabLifeActivity::setState(const State s) {
  state = s;
  // 大きい数字の画面に戻るときだけゴースト消去の半リフレッシュ。それ以外は高速更新で軽く保つ
  forceHalf = (s == State::PLAY);
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

  // 物理ボタンは上から Left, Right, Confirm, Back（横向きCCW）。
  // 長押しは離した時点の押下時間で判定して ±5。
  auto step = [this]() { return mappedInput.getHeldTime() >= kLongPressMs ? kLongStep : 1; };

  if (mappedInput.wasReleased(B::Left)) {
    adjust(1, -step());
  } else if (mappedInput.wasReleased(B::Right)) {
    adjust(1, +step());
  } else if (mappedInput.wasReleased(B::Confirm)) {
    adjust(0, +step());
  } else if (mappedInput.wasReleased(B::Back)) {
    adjust(0, -step());
  } else if (mappedInput.wasReleased(B::Up)) {
    if (mappedInput.getHeldTime() >= kMenuHoldMs) {
      menuIndex = 0;
      setState(State::MENU);
    } else {
      setState(State::HISTORY);
    }
  } else if (mappedInput.wasReleased(B::Down)) {
    finish();
  }
}

void FabLifeActivity::loopHistory() {
  using B = MappedInputManager::Button;
  // Up 短押し（または Back / Down）で閉じる
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Down)) {
    setState(State::PLAY);
  }
}

void FabLifeActivity::loopMenu() {
  using B = MappedInputManager::Button;

  if (mappedInput.wasReleased(B::Left)) {
    menuIndex = (menuIndex + kMenuItems - 1) % kMenuItems;
    requestUpdate();
  } else if (mappedInput.wasReleased(B::Right)) {
    menuIndex = (menuIndex + 1) % kMenuItems;
    requestUpdate();
  } else if (mappedInput.wasReleased(B::Confirm)) {
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
  } else if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Up)) {
    setState(State::PLAY);
  } else if (mappedInput.wasReleased(B::Down)) {
    finish();
  }
}

// パネル内ローカル座標の矩形を画面座標へ。flipped なら 180° 回転
void FabLifeActivity::fillLocal(int px, int py, int pw, int ph, bool flipped, int x, int y, int w, int h) const {
  if (flipped) {
    renderer.fillRect(px + pw - x - w, py + ph - y - h, w, h, true);
  } else {
    renderer.fillRect(px + x, py + y, w, h, true);
  }
}

void FabLifeActivity::drawNumber(int value, int px, int py, int pw, int ph, bool flipped) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  const int n = static_cast<int>(strlen(buf));

  // 桁の大きさ: 高さはパネルの 7 割、幅は高さの 0.55、桁間は太さ 2 つ分
  int dh = ph * 7 / 10;
  int t = std::max(4, dh / 7);
  int dw = dh * 55 / 100;
  int gap = t * 2;
  const int maxW = pw - 2 * t;
  if (n * dw + (n - 1) * gap > maxW) {  // 3 桁で収まらなければ縮める
    const int scale = maxW * 100 / (n * dw + (n - 1) * gap);
    dh = dh * scale / 100;
    dw = dw * scale / 100;
    t = std::max(3, t * scale / 100);
    gap = t * 2;
  }
  const int total = n * dw + (n - 1) * gap;
  const int x0 = (pw - total) / 2;
  const int y0 = (ph - dh) / 2;
  const int hh = (dh - 3 * t) / 2;

  for (int i = 0; i < n; i++) {
    const uint8_t s = kSeg[buf[i] - '0'];
    const int x = x0 + i * (dw + gap);
    if (s & 0x01) fillLocal(px, py, pw, ph, flipped, x + t, y0, dw - 2 * t, t);                  // a
    if (s & 0x02) fillLocal(px, py, pw, ph, flipped, x + dw - t, y0 + t, t, hh);                  // b
    if (s & 0x04) fillLocal(px, py, pw, ph, flipped, x + dw - t, y0 + 2 * t + hh, t, hh);         // c
    if (s & 0x08) fillLocal(px, py, pw, ph, flipped, x + t, y0 + dh - t, dw - 2 * t, t);          // d
    if (s & 0x10) fillLocal(px, py, pw, ph, flipped, x, y0 + 2 * t + hh, t, hh);                  // e
    if (s & 0x20) fillLocal(px, py, pw, ph, flipped, x, y0 + t, t, hh);                           // f
    if (s & 0x40) fillLocal(px, py, pw, ph, flipped, x + t, y0 + t + hh, dw - 2 * t, t);          // g
  }
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

  // 通常は高速更新。ゴースト対策の半リフレッシュは 30 回に 1 回と、数字画面へ戻るときだけ
  if (forceHalf || ++updatesSinceFull >= 30) {
    updatesSinceFull = 0;
    forceHalf = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
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
