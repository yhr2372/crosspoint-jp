#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  // JPEG の表示用キャッシュ（BMP）だけを作る。JPEG 以外や失敗なら false
  bool ensureJpegBmpCache() const;
  // JPEG から表示用 BMP を作った回数（デバッグ表示用。キャッシュが効いていれば増えない）
  static uint16_t decodeCount;
  // 開いたまま使い回している表示用 BMP を閉じる。その BMP を消す・書き直す前と、本を閉じるときに呼ぶ
  static void releaseCachedFile();
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
