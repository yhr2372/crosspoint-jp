#pragma once
#include <string>

// 漫画ページを「段」に分けて横向きで大きく見るための下ごしらえ。
//
// ページ画像を横 targetWidth の 2bit BMP に展開し、横一直線に通る枠線（ページ幅の 8 割以上
// 途切れずに続く黒い線）を段の区切りとして探す。見つかった段ごとに <画像>.t<N>.bmp を作り、
// 段数を <画像>.tiers に書いておく（2 回目以降は展開しない）。区切りが見つからないページは 3 等分。
namespace image_tiers {

constexpr int MAX_TIERS = 6;

// 段の BMP を用意して段数を返す（失敗なら 0）。用意済みならすぐ返る
int prepare(const std::string& imagePath, int targetWidth);

// 用意済みか（展開に時間がかかるので、まだなら呼び出し側で「処理中」を出す）
bool isPrepared(const std::string& imagePath, int targetWidth);

std::string tierPath(const std::string& imagePath, int index);

}  // namespace image_tiers
