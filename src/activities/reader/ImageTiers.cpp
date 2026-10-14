#include "ImageTiers.h"

#include <Arduino.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace image_tiers {
namespace {

// 変換器が書く 2bit BMP（ヘッダ 70 バイト、上から下へ並ぶ行）
constexpr uint32_t BMP_HEADER_SIZE = 70;

std::string stemOf(const std::string& imagePath) {
  const size_t dot = imagePath.rfind('.');
  return dot != std::string::npos ? imagePath.substr(0, dot) : imagePath;
}

std::string metaPath(const std::string& imagePath) { return stemOf(imagePath) + ".tiers"; }
std::string zoomPath(const std::string& imagePath) { return stemOf(imagePath) + ".zoom.bmp"; }

uint32_t readLE32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));  // RISC-V は非整列読みで落ちるので memcpy
  return v;
}
void writeLE32(uint8_t* p, const uint32_t v) { memcpy(p, &v, sizeof(v)); }

// 段数と、作ったときの幅（画面の向き・機種が変わったら作り直す）
bool readMeta(const std::string& imagePath, int& count, int& width) {
  HalFile f;
  if (!Storage.exists(metaPath(imagePath).c_str()) || !Storage.openFileForRead("TIER", metaPath(imagePath), f)) {
    return false;
  }
  char buf[32] = {};
  f.read(buf, sizeof(buf) - 1);
  f.close();
  // 段数 0 は「段に分けない絵」（ポスターなど）として覚えておく
  return sscanf(buf, "%d,%d", &count, &width) == 2 && count >= 0 && count <= MAX_TIERS;
}

// ページ画像を横 targetWidth（元の幅を超えない）の 2bit BMP に展開する
bool decodeZoom(const std::string& imagePath, int targetWidth) {
  ImageDimensions dims = {0, 0};
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder || !decoder->getDimensions(imagePath, dims) || dims.width <= 0 || dims.height <= 0) {
    LOG_ERR("TIER", "No dimensions: %s", imagePath.c_str());
    return false;
  }
  // 拡大はしない（変換器は縮小前提）。変換器は枠を埋める側（大きい方）の倍率を選ぶので、
  // 高さも幅と同じ倍率になる値をちょうど渡す（大きな値を渡すと高さに合わせて巨大になる）
  const int w = std::min<int>(targetWidth, dims.width);
  const int h = std::max(1, static_cast<int>(static_cast<int64_t>(dims.height) * w / dims.width));

  HalFile in;
  HalFile out;
  if (!Storage.openFileForRead("TIER", imagePath, in)) return false;
  if (!Storage.openFileForWrite("TIER", zoomPath(imagePath), out)) {
    in.close();
    return false;
  }
  const bool ok = FsHelpers::hasJpgExtension(imagePath)
                      ? JpegToBmpConverter::jpegFileToBmpStreamWithSize(in, out, w, h)
                      : PngToBmpConverter::pngFileToBmpStreamWithSize(in, out, w, h);
  in.close();
  out.close();
  if (!ok) {
    LOG_ERR("TIER", "Decode failed: %s", imagePath.c_str());
    Storage.remove(zoomPath(imagePath).c_str());
  }
  return ok;
}

struct Band {
  int top;
  int bottom;
};

// 展開した BMP を 1 行ずつ読み、段の上下（行番号、bottom は含まない）を求める。
// 手描きの枠線はわずかに斜めなので、前後 k 行を重ねてから「黒が横に何画素続くか」を数える
bool findTiers(HalFile& bmp, const int width, const int height, const int rowBytes, std::vector<Band>& tiers,
               bool& illustration) {
  illustration = false;
  const int k = std::max(2, height / 400);
  const int window = 2 * k + 1;
  const int maskBytes = (width + 7) / 8;

  // 行バッファ（～200B）、直近 window 行の黒マスク（～1KB）、行ごとの最長の黒の並び（2B×高さ）。
  // 高さが 2000 行を超えうるのでスタックではなくヒープに置き、関数を出る前に解放する
  auto* row = static_cast<uint8_t*>(malloc(rowBytes));
  auto* masks = static_cast<uint8_t*>(calloc(window, maskBytes));
  auto* merged = static_cast<uint8_t*>(malloc(maskBytes));
  auto* runs = static_cast<uint16_t*>(calloc(height, sizeof(uint16_t)));
  if (!row || !masks || !merged || !runs) {
    LOG_ERR("TIER", "malloc failed (%d rows)", height);
    free(row);
    free(masks);
    free(merged);
    free(runs);
    return false;
  }

  auto longestRunAround = [&](const int center) {
    memset(merged, 0, maskBytes);
    for (int y = std::max(0, center - k); y <= std::min(height - 1, center + k); y++) {
      const uint8_t* m = masks + (y % window) * maskBytes;
      for (int i = 0; i < maskBytes; i++) merged[i] |= m[i];
    }
    int best = 0;
    int run = 0;
    for (int x = 0; x < width; x++) {
      if (merged[x >> 3] & (0x80 >> (x & 7))) {
        best = std::max(best, ++run);
      } else {
        run = 0;
      }
    }
    return best;
  };

  int firstInk = -1;
  int lastInk = -1;
  int64_t darkTotal = 0;
  bool readOk = true;
  for (int y = 0; y < height; y++) {
    if (bmp.read(row, rowBytes) != rowBytes) {
      readOk = false;
      break;
    }
    uint8_t* m = masks + (y % window) * maskBytes;
    memset(m, 0, maskBytes);
    int dark = 0;
    for (int x = 0; x < width; x++) {
      const uint8_t v = (row[x >> 2] >> (6 - 2 * (x & 3))) & 0x03;  // 0 黒 / 1 濃い灰 / 2 薄い灰 / 3 白
      if (v == 0) {
        m[x >> 3] |= 0x80 >> (x & 7);
        dark++;
      }
      if (v <= 1) darkTotal++;
    }
    if (dark > width / 100) {
      if (firstInk < 0) firstInk = y;
      lastInk = y;
    }
    if (y >= k) runs[y - k] = static_cast<uint16_t>(longestRunAround(y - k));
  }
  if (readOk) {
    for (int c = std::max(0, height - k); c < height; c++) runs[c] = static_cast<uint16_t>(longestRunAround(c));
  }

  // 黒と濃い灰が 35% を超える絵（暗いポスターなどの一枚絵）は段に分けず、普通に表示させる。
  // 漫画のページは夜の場面でも 1 割ほど（実測）なので、十分に離れている
  if (readOk && darkTotal * 100 > static_cast<int64_t>(width) * height * 35) illustration = true;

  if (readOk && firstInk >= 0 && !illustration) {
    // 枠の幅（最長の横線）の 8 割以上続く行を「線」とし、近い行どうしを 1 本にまとめる
    int frame = 0;
    for (int y = 0; y < height; y++) frame = std::max<int>(frame, runs[y]);
    std::vector<Band> lines;
    lines.reserve(16);
    for (int y = 0; y < height; y++) {
      if (runs[y] < frame * 8 / 10) continue;
      if (!lines.empty() && y - lines.back().bottom <= window) {
        lines.back().bottom = y;
      } else if (lines.size() < 16) {
        lines.push_back({y, y});
      }
    }
    // 外枠が拾えなかった側は、絵のある範囲の端を区切りにする
    if (lines.empty() || lines.front().top > firstInk + height / 20) lines.insert(lines.begin(), {firstInk, firstInk});
    if (lines.back().bottom < lastInk - height / 20) lines.push_back({lastInk, lastInk});

    // 線と線の間が十分高ければ 1 段。枠線も含めて切り出す（コマの枠が見えるように）
    tiers.reserve(MAX_TIERS);
    for (size_t i = 0; i + 1 < lines.size(); i++) {
      if (lines[i + 1].top - lines[i].bottom > height * 8 / 100) {
        tiers.push_back({lines[i].top, std::min(height, lines[i + 1].bottom + 1)});
      }
    }
    if (tiers.size() < 2 || tiers.size() > static_cast<size_t>(MAX_TIERS)) {
      // 区切りが分からないページは、絵のある範囲を 3 等分する
      tiers.clear();
      const int span = lastInk + 1 - firstInk;
      for (int t = 0; t < 3; t++) tiers.push_back({firstInk + span * t / 3, firstInk + span * (t + 1) / 3});
    } else {
      // 段の下の枠がコマごとに段違いだと横一直線にならず、2 段が 1 段にまとまる。
      // ページの 45% より高い段は、段の中ほど（25〜75%）で横線がいちばん長く続く行で 2 つに分ける
      std::vector<Band> split;
      split.reserve(MAX_TIERS);
      for (const Band& t : tiers) {
        const int h = t.bottom - t.top;
        if (h > height * 45 / 100 && split.size() + 2 <= static_cast<size_t>(MAX_TIERS)) {
          int cut = t.top + h / 4;
          for (int y = t.top + h / 4; y < t.top + h * 3 / 4; y++) {
            if (runs[y] > runs[cut]) cut = y;
          }
          split.push_back({t.top, std::min(height, cut + k + 1)});
          split.push_back({std::max(0, cut - k), t.bottom});
        } else if (split.size() < static_cast<size_t>(MAX_TIERS)) {
          split.push_back(t);
        }
      }
      tiers.swap(split);
    }
  }

  free(row);
  row = nullptr;
  free(masks);
  masks = nullptr;
  free(merged);
  merged = nullptr;
  free(runs);
  runs = nullptr;
  return readOk && (illustration || !tiers.empty());
}

// 展開した BMP の [top, bottom) 行を、ヘッダの高さだけ書き換えた別の BMP に写す
bool writeTier(HalFile& src, const uint8_t* header, const int rowBytes, const Band& band, const std::string& path) {
  const int rows = band.bottom - band.top;
  uint8_t h[BMP_HEADER_SIZE];
  memcpy(h, header, sizeof(h));
  writeLE32(h + 2, BMP_HEADER_SIZE + rows * rowBytes);  // ファイルサイズ
  writeLE32(h + 22, static_cast<uint32_t>(-rows));      // 負の高さ = 上から下
  writeLE32(h + 34, rows * rowBytes);                   // 画素データの大きさ

  HalFile out;
  if (!Storage.openFileForWrite("TIER", path, out)) return false;
  out.write(h, sizeof(h));
  src.seekSet(BMP_HEADER_SIZE + static_cast<size_t>(band.top) * rowBytes);
  uint8_t buf[256];  // スタックに置ける上限の目安（256B）に合わせて少しずつ写す
  size_t remaining = static_cast<size_t>(rows) * rowBytes;
  bool ok = true;
  while (remaining > 0) {
    const size_t n = std::min(remaining, sizeof(buf));
    if (src.read(buf, n) != static_cast<int>(n) || out.write(buf, n) != n) {
      ok = false;
      break;
    }
    remaining -= n;
  }
  out.close();
  return ok;
}

}  // namespace

std::string tierPath(const std::string& imagePath, const int index) {
  return stemOf(imagePath) + ".t" + std::to_string(index) + ".bmp";
}

bool isPrepared(const std::string& imagePath, const int targetWidth) {
  int count = 0;
  int width = 0;
  return readMeta(imagePath, count, width) && width == targetWidth;
}

int prepare(const std::string& imagePath, const int targetWidth) {
  int count = 0;
  int width = 0;
  if (readMeta(imagePath, count, width) && width == targetWidth) return count;

  const unsigned long start = millis();
  if (!decodeZoom(imagePath, targetWidth)) return 0;

  HalFile bmp;
  if (!Storage.openFileForRead("TIER", zoomPath(imagePath), bmp)) return 0;
  uint8_t header[BMP_HEADER_SIZE];
  const bool headerOk = bmp.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) && header[0] == 'B' &&
                        header[1] == 'M' && readLE32(header + 10) == BMP_HEADER_SIZE && header[28] == 2;
  const int bmpWidth = static_cast<int>(readLE32(header + 18));
  const int bmpHeight = -static_cast<int32_t>(readLE32(header + 22));  // 上から下（負の高さ）だけを扱う
  if (!headerOk || bmpWidth <= 0 || bmpHeight <= 0) {
    LOG_ERR("TIER", "Unexpected BMP format: %s", zoomPath(imagePath).c_str());
    bmp.close();
    Storage.remove(zoomPath(imagePath).c_str());
    return 0;
  }
  const int rowBytes = (bmpWidth * 2 + 31) / 32 * 4;

  std::vector<Band> tiers;
  bool ok = true;
  bool illustration = false;
  if (bmpHeight * 10 < bmpWidth * 12) {
    // 縦長でない絵（見開き・横長の一枚絵）は分けず、1 枚まるごとを横向きで出す
    tiers.push_back({0, bmpHeight});
  } else {
    ok = findTiers(bmp, bmpWidth, bmpHeight, rowBytes, tiers, illustration);
  }
  for (size_t i = 0; ok && i < tiers.size(); i++) {
    ok = writeTier(bmp, header, rowBytes, tiers[i], tierPath(imagePath, static_cast<int>(i)));
  }
  bmp.close();
  Storage.remove(zoomPath(imagePath).c_str());  // 段に分けたら全体の BMP は要らない
  if (!ok) {
    LOG_ERR("TIER", "Failed to split %s", imagePath.c_str());
    return 0;
  }

  HalFile meta;
  if (Storage.openFileForWrite("TIER", metaPath(imagePath), meta)) {
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "%d,%d", static_cast<int>(tiers.size()), targetWidth);
    meta.write(buf, n);
    meta.close();
  }
  LOG_DBG("TIER", "%s: %d tiers (%dx%d) in %lums", imagePath.c_str(), static_cast<int>(tiers.size()), bmpWidth,
          bmpHeight, millis() - start);
  return static_cast<int>(tiers.size());
}

}  // namespace image_tiers
