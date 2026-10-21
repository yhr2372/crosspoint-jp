#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "EpdFont.h"
#include "EpdFontData.h"

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 128;
  static constexpr uint8_t MAX_STYLES = 4;

  SdCardFont() = default;
  ~SdCardFont();

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from utf8Text (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // advance テーブルの参照結果。getAdvance() が返す 0 の意味を呼び出し側が
  // 区別できるようにするためのもの。
  enum class AdvanceLookup : uint8_t {
    NotCached,  // テーブルに載っていない（このテキスト用に構築されていない）
    Found,      // グリフがあり advanceOut が有効（幅0の結合文字などもここ）
    NoGlyph,    // フォントにグリフが無いと分かっている（renderChar は '?' を描く）
  };
  AdvanceLookup lookupAdvance(uint32_t codepoint, uint8_t style, uint16_t& advanceOut) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles, restore stub EpdFontData.
  void clearCache();

  // Free kern/ligature data for all styles (reclaim memory before heavy operations).
  // Data will be lazy-loaded again on next prewarm.
  void freeKernLigatureData();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // 省メモリの読み込み方。load() の前に設定する（変えたらフォントを読み直す）。
  // LOAD_KERN_LATIN_ONLY: 字間の調整表のうち英字（U+2100 未満）どうしのぶんだけを RAM に置く。
  //   日本語フォントの表は大半がかなの組み合わせで、NotoSansJp では 30 KB -> 約 4 KB になる。
  // LOAD_SYNTHETIC_BOLD: 太字などのスタイルを読まず、通常の字を 1px ずらして重ね描きする
  //   （GfxRenderer の synthBold）。太字の字の一覧・調整表・先読みのぶんが要らなくなる。
  // どちらも字の幅が変わるので、オンのときは contentHash（= fontId）に混ぜて章のレイアウトを作り直させる。
  static constexpr uint8_t LOAD_KERN_LATIN_ONLY = 0x01;
  static constexpr uint8_t LOAD_SYNTHETIC_BOLD = 0x02;
  static void setLoadOptions(uint8_t options) { loadOptions_ = options; }
  static uint8_t loadOptions() { return loadOptions_; }

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Look up vertical substitute glyph for a codepoint.
  // Returns the vert EpdGlyph* if found, nullptr otherwise.
  // Vert data must be loaded via loadVertData() first.
  const EpdGlyph* getVertGlyph(uint32_t codepoint, uint8_t style = 0) const;

  // Returns the bitmap data for a vert glyph (must be from getVertGlyph).
  const uint8_t* getVertBitmap(const EpdGlyph* vertGlyph, uint8_t style = 0) const;

  // Returns true if this font has vert data for any style.
  bool hasVertData() const;

  // Load vert glyph data from SD card for the given style.
  // Called during prewarm when vertical mode is active.
  bool loadVertData(uint8_t style);

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;

    // Persistent kern/ligature data (lazy-loaded on first prewarm)
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    int8_t* kernMatrix = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;
    // RAM に置いた調整表の大きさ（LOAD_KERN_LATIN_ONLY のときはファイルの値より小さい）
    uint16_t kernLeftEntriesLoaded = 0;
    uint16_t kernRightEntriesLoaded = 0;
    uint8_t kernLeftClassesLoaded = 0;
    uint8_t kernRightClassesLoaded = 0;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Vertical glyph substitution data (from .cpfont v5 vert section)
    uint32_t vertSectionOffset = 0;  // File offset to vert section (0 = no vert data)
    uint16_t vertCount = 0;
    uint32_t* vertCodepoints = nullptr;
    EpdGlyph* vertGlyphs = nullptr;
    uint8_t* vertBitmap = nullptr;
    bool vertLoaded = false;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;
  static uint8_t loadOptions_;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  // 先読み（1 ページ MAX_PAGE_GLYPHS 字まで）に入らなかった字の置き場。漢字の多いページは 1 ページで
  // 150〜200 字種になり、8 では足りずに同じ字を描くたび .cpfont から読み直していた（1 パス数秒）。
  // 1 枠は字形の情報と小さなビットマップ（数十バイト）だけなので、1 ページぶんが収まる数にする
  static constexpr uint32_t OVERFLOW_CAPACITY = 64;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;
  // 先読みに入らなかった字を読むときの .cpfont。字ごとに開き直すと毎回パスをたどって探すので開いたままにする
  HalFile overflowFile_;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  //
  // エントリは呼び出しをまたいで**累積**する。レイアウトはテキストブロック
  // （段落 または 400語）ごとに buildAdvanceTable() を呼ぶため、毎回作り直すと
  // 同じ字を何度も .cpfont から読み直すことになる（Issue #99: 章ビルド時間の
  // 88.8% がこれだった）。累積させることで、1コードポイントにつき SD 読みは
  // 1セクションビルドあたり最大1回で済む。
  //
  // 寿命は clearAdvanceTables() まで。実際には clearCache() 経由で
  // (a) セクションビルドの直前（EpubReaderActivity）と
  // (b) 毎ページの描画前（FontCacheManager::PrewarmScope の ctor）
  // に破棄される。つまり読書中に載りっぱなしにはならず、常駐するのは
  // 1セクションビルド中および1ページ描画中だけ。
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
    uint8_t hasGlyph;   // 0 = このフォントにグリフが無い（ネガティブキャッシュ。再探索を防ぐ）
  };  // アラインメント込みで 8 バイト

  // 1スタイルあたりの上限。4096エントリ = 32KB。日本語の長編でも異なり字は
  // 3,000字程度（JIS第1水準 ≒ 3,000）なので通常は到達しない。
  static constexpr uint32_t ADVANCE_TABLE_INITIAL_CAP = 512;
  static constexpr uint32_t ADVANCE_TABLE_MAX_CAP = 4096;
  // 1回の呼び出しで新規に取得するコードポイント数の上限。
  static constexpr uint32_t MAX_MISSES_PER_CALL = 1024;

  AdvanceEntry* advanceTable_[MAX_STYLES] = {};  // コードポイント昇順。realloc で伸長するため malloc 系で管理
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  uint32_t advanceTableCap_[MAX_STYLES] = {};

  // 累積テーブルが上限に達した（または伸長に失敗した）ときの退避先。
  // buildAdvanceTable() の呼び出しごとに破棄されるブロックローカルのテーブル。
  // これが無いと、上限到達の瞬間から全ブロックが「毎回作り直し」に戻る性能の崖ができる。
  AdvanceEntry* advanceSpill_[MAX_STYLES] = {};
  uint32_t advanceSpillSize_[MAX_STYLES] = {};

  void clearAdvanceTables();
  void clearAdvanceSpill();
  bool ensureAdvanceCapacity(uint8_t styleIdx, uint32_t needed);
  const AdvanceEntry* findAdvanceEntry(uint8_t styleIdx, uint32_t codepoint) const;
  int buildAdvanceTableForStyle(uint8_t styleIdx, const char* utf8Text);

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  bool loadKernLatinOnly(PerStyle& s, FsFile& file);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
