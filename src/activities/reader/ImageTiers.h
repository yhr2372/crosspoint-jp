#pragma once
#include <string>

// 漫画ページを「段」に分けて横向きで大きく見るための下ごしらえ。
//
// ページ画像を横 targetWidth の 2bit BMP に展開し、横一直線に通る枠線（ページ幅の 8 割以上
// 途切れずに続く黒い線）を段の区切りとして探す。見つかった段ごとに <画像>.t<N>.bmp を作り、
// 段数を <画像>.tiers に書いておく（2 回目以降は展開しない）。区切りが見つからないページは 3 等分。
// 縦長でない絵（縦が横の 1.2 倍未満。見開きや横長の一枚絵）は分けずに 1 段として扱う。
// 段違いの枠で 2 段がまとまった高すぎる段（ページの 45% 超）は、中ほどの横線でさらに分ける。
// 黒と濃い灰が 35% を超える絵（暗いポスターなど）は段数 0 として、普通の表示に任せる。
namespace image_tiers {

constexpr int MAX_TIERS = 6;

// 段を作った回数（デバッグ表示用。用意済みなら増えない）
extern int buildCount;

// 段の BMP を用意して段数を返す（失敗なら 0）。用意済みならすぐ返る
int prepare(const std::string& imagePath, int targetWidth);

// 用意済みなら段数、まだなら -1（展開に時間がかかるので、まだなら呼び出し側で「処理中」を出す）
int preparedCount(const std::string& imagePath, int targetWidth);

std::string tierPath(const std::string& imagePath, int index);

}  // namespace image_tiers
