#pragma once

#include <VerticalTextUtils.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../FootnoteEntry.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int superscriptUntilDepth = INT_MAX;
  int subscriptUntilDepth = INT_MAX;
  // <pre>: この深さより内側では空白と改行を原文のまま残す。
  int preUntilDepth = INT_MAX;
  bool preSkipLeadingNewline = false;  // <pre> 直後の改行 1 つは捨てる（HTML の規定）
  bool preSavedHyphenation = false;    // <pre> の間だけハイフネーションを切るための退避
  // <pre> の下のアキ。最初の行から外して最後の行に付け替える（行ごとに付くと枠が途切れる）
  int16_t preSavedMarginBottom = 0;
  int16_t preSavedPaddingBottom = 0;
  // 読んだが、まだ行として確定させていない改行の数。改行が来た時点ではなく次の中身が
  // 来た時点で行を切ることで、</pre> に来たときに「最後の行」が手元に残る（枠の下辺を
  // 付けるために必要）。末尾の改行はこの仕組みで自然に捨てられる。
  int prePendingNewlines = 0;
  BlockStyle preContinuationStyle() const;
  void preFlushPendingNewlines();
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int16_t currentPageNextX = 0;  // vertical mode: next column x position (decreases right-to-left)
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  int fontId;
  int headingFontIds[6] = {0, 0, 0, 0, 0, 0};  // per heading level (h1-h6), 0 = use page fontId
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool firstLineIndent;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  bool imageDirReady = false;  // 画像を置くフォルダを作ったか
  int imageCounter = 0;
  bool verticalMode = false;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  // 原文でこの直後の語の前に空白があったか。CJK は 1 文字ずつを語に割る関係で
  // nextWordContinues（＝スペース無しで前の語に続く）では区別できないため別に持つ。
  bool pendingSpace = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;

  // <ol> / <ul> のネスト。<li> のマーカーを連番にするか中黒にするかを決めるために持つ。
  // 4 段を超えるリストは実用上まれなので固定長配列にしてある（std::vector だと
  // 章ごとにヒープを踏む。listDepth 自体は対称に増減させるので入れ子は壊れない）。
  static constexpr int MAX_LIST_NESTING = 4;
  struct ListContext {
    uint16_t counter = 1;  // 次の <li> に振る番号
    bool ordered = false;
  };
  ListContext listStack[MAX_LIST_NESTING];
  int listDepth = 0;
  // リストマーカーを出した直後か。次に来る語をマーカーに必ずくっつけて語間 0 にするための印。
  // こうしないと語間が中身次第（CJK か欧文か、原文に空白があるか）で変わり、
  // ぶら下げインデントの幅と本文の開始位置がずれる。
  bool listMarkerPending = false;

  // Table grid buffering
  struct TableCellData {
    std::string text;
    bool isHeader = false;
  };
  struct TableRowData {
    std::vector<TableCellData> cells;
  };
  std::vector<TableRowData> tableBuffer;
  std::string tableCellTextBuffer;
  bool tableCellIsHeader = false;
  int tableFontId = 0;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  char currentFootnoteLinkText[24] = {};
  int currentFootnoteLinkTextLen = 0;
  char currentFootnoteLinkHref[64] = {};
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  void flushCurrentBlockMidPage();
  void makePages();
  void flushTableAsGrid();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool firstLineIndent,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
                                 const int* headingFontIds = nullptr, int tableFontId = 0, bool verticalMode = false)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        firstLineIndent(firstLineIndent),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        verticalMode(verticalMode) {
    if (headingFontIds) {
      for (int i = 0; i < 6; i++) this->headingFontIds[i] = headingFontIds[i];
    }
    this->tableFontId = tableFontId;
  }

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
};
