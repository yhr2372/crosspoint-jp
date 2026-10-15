#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool firstLineIndent, bool embeddedStyle, uint8_t imageRendering, bool verticalMode,
                              uint8_t charSpacing);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  void expandLoneImage(Page& page) const;
  bool isLoneImageSection();
  void refitLoneImageToArea(Page& page) const;

  // 画像 1 枚だけのページを広げる先（ページ座標。余白の外まで含むので負になりうる）。w==0 なら広げない
  int16_t fullBleedX = 0;
  int16_t fullBleedY = 0;
  uint16_t fullBleedW = 0;
  uint16_t fullBleedH = 0;
  // 設定が違うまま使っている画像だけの章（読み込んだページの画像を今の枠に収め直す）
  bool refitLoneImage = false;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;
  int getSpineIndex() const { return spineIndex; }

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  // loadSectionFile() / createSectionFile() の前に呼ぶ。画像だけのページはこの枠いっぱいに収め直す
  void setFullBleedArea(int16_t x, int16_t y, uint16_t w, uint16_t h) {
    fullBleedX = x;
    fullBleedY = y;
    fullBleedW = w;
    fullBleedH = h;
  }
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                       bool embeddedStyle, uint8_t imageRendering, bool verticalMode, uint8_t charSpacing);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                         bool embeddedStyle, uint8_t imageRendering, bool verticalMode, uint8_t charSpacing,
                         const std::function<void()>& popupFn = nullptr, const int* headingFontIds = nullptr,
                         int tableFontId = 0);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
