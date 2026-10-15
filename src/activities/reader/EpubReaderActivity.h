#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  // 段表示（画像だけのページを横向きで 1 段ずつ見せる）。決定ボタンの長押しで切り替える
  bool tierMode = false;
  static constexpr unsigned long TIER_TOGGLE_MS = 700;
  bool confirmLongFired = false;  // 長押しで切り替えたので、離したときにメニューを開かない
  int tierIndex = 0;              // 今の段
  int tierCount = 0;              // 今のページの段数（段表示でない／画像だけのページでないなら 0）
  bool tierStartAtEnd = false;    // 前のページへ戻ったときは最後の段から出す
  int prefetchedSpine = -1;       // 次のページの段を先に作り終えた位置（同じページで繰り返さない）
  int prefetchedPage = -1;
  bool prefetchedTierMode = false;
  // 次のページの用意は、表示してから操作がないまま PREFETCH_IDLE_MS たってから loop() で始める
  // （表示の直後に始めると、その間ボタンが待たされて次の段がすぐ出ない）
  static constexpr unsigned long PREFETCH_IDLE_MS = 1500;
  unsigned long prefetchDueAt = 0;  // 0 なら予定なし
  uint16_t prefetchViewportW = 0;
  uint16_t prefetchViewportH = 0;
  // デバッグ表示（設定）用の計測値（ms）
  unsigned long debugSectionMs = 0;
  unsigned long debugDrawMs = 0;
  unsigned long debugPrefetchMs = 0;
  // 画像だけのページを広げる枠（ページ座標）。余白を除いた本体の見える範囲
  int16_t fullBleedX = 0;
  int16_t fullBleedY = 0;
  uint16_t fullBleedW = 0;
  uint16_t fullBleedH = 0;
  bool verticalMode = false;  // resolved effective writing mode for current book

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  bool renderTier(const std::string& imagePath);
  int tierAreaWidth() const;
  void prefetchNextPage(uint16_t viewportWidth, uint16_t viewportHeight);
  bool loadOrBuildSection(Section& target, uint16_t viewportWidth, uint16_t viewportHeight);
  void drawTierBitmap(const std::string& path) const;
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  // percent: 本全体の進捗率（0〜100）。不明なら -1（ホーム画面が book.bin を開かずに表示できるよう
  // progress.bin の末尾に 1 バイトで保存する）
  void drawDebugTimings();
  void saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished = false, int percent = -1);
  // 現在の章内位置から本全体の進捗率（0〜100）を求める。計算できなければ -1
  int calculateBookPercent(int currentPage, int pageCount) const;
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void invalidateSectionPreservingPosition();
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // 読了画面から先へ進んだときの確認ダイアログと、その結果の処理 (#47)
  void showFinishedBookPrompt();
  void finishBookFile(bool remove);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  bool supportsLandscape() const override { return true; }
};
