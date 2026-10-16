#include "EpubReaderActivity.h"

#include <Bitmap.h>
#include <Epub/InlineImage.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_system.h>

#include <algorithm>

#include "BookFileHelper.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "ImageTiers.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/LineSpacingSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// 画像 1 枚だけのページなら、その画像（違えば nullptr）
const ImageBlock* loneImage(const Page* page) {
  if (!page || page->elements.size() != 1 || page->elements[0]->getTag() != TAG_PageImage) return nullptr;
  return &static_cast<const PageImage&>(*page->elements[0]).getImageBlock();
}
}  // namespace

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // ルビフォントIDはrender()内でフォントロード後に設定

  // Screen orientation (both renderer and input) is already set by
  // ActivityManager → OrientationHelper::applyOrientation() before onEnter().

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  ImageBlock::releaseCachedFile();  // 開いたまま使い回していた表示用 BMP

  // 次のアクティビティの向き（renderer と入力の両方）は ActivityManager が
  // Pop / Replace 時に OrientationHelper で適用するので、ここでは戻さない。
  // renderer だけ Portrait に戻すと入力側の向きが残留する（#120）。

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();

  // Release the SD card font's lazy caches on the way out. Nothing outside the
  // reader draws with an SD font ID (every getReaderFontId() caller loads the
  // family first via ensureSdFontLoaded), so holding them costs memory for the
  // rest of the session and buys nothing.
  //
  // The kern matrix is the expensive one: it is a single leftClassCount x
  // rightClassCount block, 178x161 = 28KB per style for NotoSansJp, and the
  // reader loads two .cpfont bases (body + heading) x two styles. Four 28KB
  // blocks parked mid-arena is exactly what leaves the heap with 67KB free but
  // only a 26KB largest block — too fragmented for the ~16.5KB contiguous
  // inbound record buffer a TLS handshake needs, which is how the Aozora
  // downloads ended up failing with ESP_ERR_HTTP_CONNECT.
  //
  // Both caches are lazy-loaded again on the next prewarm, and the font IDs
  // stay registered, so section caches keyed on them remain valid.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // 操作があれば次のページの用意は見送る（その操作で描き直したあと、また予定し直す）。
  // 用意そのものは描画タスクに頼む（ここで作ると、その間は電源ボタンも含めて操作を受け付けなくなる）
  if (mappedInput.wasAnyPressed()) {
    prefetchDueAt = 0;
    prefetchRequested = false;
  } else if (prefetchDueAt != 0 && static_cast<long>(millis() - prefetchDueAt) >= 0) {
    prefetchDueAt = 0;
    prefetchRequested = true;
    requestUpdate();
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // 決定ボタンの長押しで段表示を切り替える（押している間に切り替え、離したときはメニューを開かない）
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && !confirmLongFired &&
      mappedInput.getHeldTime() >= TIER_TOGGLE_MS) {
    confirmLongFired = true;
    tierMode = !tierMode;
    tierIndex = 0;
    tierStartAtEnd = false;
    requestUpdate();
    return;
  }
  if (confirmLongFired) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) confirmLongFired = false;
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Snapshot reader state under render lock. This avoids racing with the
    // render task while it may rebuild/reset the current section.
    int menuSpineIndex = 0;
    int menuCurrentPage = 0;
    int menuTotalPages = 0;
    {
      RenderLock lock(*this);
      menuSpineIndex = currentSpineIndex;
      if (section) {
        menuCurrentPage = section->currentPage + 1;
        menuTotalPages = section->pageCount;
      }
    }

    if (!epub) return;

    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && menuTotalPages > 0) {
      const float chapterProgress = static_cast<float>(menuCurrentPage - 1) / static_cast<float>(menuTotalPages);
      bookProgress = epub->calculateProgress(menuSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), menuCurrentPage, menuTotalPages,
                               bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty(), verticalMode),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // 段表示中は、ページ送りでまず段を進める。最後の段の次（最初の段の前）で普通のページ送りになる。
  // 縦書き（右から左）ではページ送りのボタンが入れ替わるので、進む向きもそれに合わせる
  if (tierMode && tierCount > 0) {
    const bool forward = verticalMode ? prevTriggered : nextTriggered;
    if (forward && tierIndex < tierCount - 1) {
      tierIndex++;
      lastPageTurnTime = millis();
      requestUpdate();
      return;
    }
    if (!forward && tierIndex > 0) {
      tierIndex--;
      lastPageTurnTime = millis();
      requestUpdate();
      return;
    }
    tierIndex = 0;
    tierStartAtEnd = !forward;
  }

  // At end of the book, forward button goes home and back button returns to last page.
  // In vertical RTL the physical buttons are swapped for page turns (see pageTurn
  // below), so resolve "forward" the same way here — otherwise the book closes on
  // the button that should return to the last page.
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    const bool forwardTriggered = verticalMode ? prevTriggered : nextTriggered;
    if (forwardTriggered) {
      showFinishedBookPrompt();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    // 隣接する章が存在しない方向へのスキップは抑止し、通常のページめくりに落とす。
    // 1章構成の書籍で誤って長押しすると、前方は末尾（読了マークが付き読書位置を失う）、
    // 後方は書籍の先頭へ飛ばされてしまうため。
    const bool skipForward = verticalMode ? !nextTriggered : nextTriggered;
    const int targetSpineIndex = skipForward ? currentSpineIndex + 1 : currentSpineIndex - 1;
    if (targetSpineIndex >= 0 && targetSpineIndex < epub->getSpineItemsCount()) {
      lastPageTurnTime = millis();
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex = targetSpineIndex;
        section.reset();
      }
      requestUpdate();
      return;
    }
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(verticalMode);  // In vertical RTL: prev button = forward
  } else {
    pageTurn(!verticalMode);  // In vertical RTL: next button = backward
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::invalidateSectionPreservingPosition() {
  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      // Open settings directly on the Reader category and return to reader on back.
      startActivityForResult(std::make_unique<SettingsActivity>(
                                 renderer, mappedInput, [this] { finish(); }, 1, 1),
                             [this](const ActivityResult&) {
                               // Reader settings (font/line spacing/margins etc.) may change pagination.
                               // Cache dir may have been deleted by ClearCacheActivity — recreate it.
                               if (epub) epub->setupCacheDir();
                               invalidateSectionPreservingPosition();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_FIRST_LINE_INDENT: {
      auto& dirSettings = SETTINGS.getDirectionSettings(verticalMode);
      dirSettings.firstLineIndent = !dirSettings.firstLineIndent;
      SETTINGS.saveToFile();
      invalidateSectionPreservingPosition();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_INVERT_IMAGES: {
      SETTINGS.invertImages = !SETTINGS.invertImages;
      SETTINGS.saveToFile();
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_LINE_SPACING: {
      uint8_t& target = SETTINGS.getDirectionSettings(verticalMode).lineSpacing;
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) {
                               invalidateSectionPreservingPosition();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_STATUS_BAR: {
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (InlineImage::isInlineImage(w)) continue;  // 縦書きのインライン画像語は本文ではない
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          const int backupPercent = calculateBookPercent(section->currentPage, section->pageCount);
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount, false, backupPercent);
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        startActivityForResult(
            std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                   currentPage, totalPages),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& sync = std::get<SyncResult>(result.data);
                if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
                  RenderLock lock(*this);
                  currentSpineIndex = sync.spineIndex;
                  nextPageNumber = sync.page;
                  section.reset();
                }
              }
            });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TILT_PAGE_TURN:
      // Toggled inline in menu; IMU sync handled in result callback.
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer and input orientation to match the new coordinate system.
    OrientationHelper::applyOrientation(renderer, mappedInput, this);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// 読了画面で先送りされたときの確認 (#47)。「閉じる」で従来どおりホームへ、
// 「アーカイブ」で /Archived/ へ移動、「削除」は再確認を挟んでから消す。
// 「戻る」で読了画面に留まる（さらに戻ると最終ページへ）。
void EpubReaderActivity::showFinishedBookPrompt() {
  if (!epub) return;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, epub->getTitle(), tr(STR_FINISHED_BOOK_PROMPT),
                                             tr(STR_ARCHIVE), tr(STR_CLOSE_BOOK), tr(STR_BACK), tr(STR_DELETE)),
      [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          // Right → 閉じる
          onGoHome();
          return;
        }
        const auto* menu = std::get_if<MenuResult>(&res.data);
        if (!menu) {
          // Back → 読了画面に留まる
          requestUpdate();
          return;
        }
        if (menu->action == ConfirmationActivity::RESULT_NEVER) {
          // Left → アーカイブ
          finishBookFile(/*remove=*/false);
          return;
        }
        if (menu->action == ConfirmationActivity::RESULT_MIDDLE) {
          // Confirm → 削除。取り消せない操作なので一段挟む
          startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, epub->getTitle(),
                                                                        tr(STR_DELETE_BOOK_PROMPT), "", tr(STR_DELETE)),
                                 [this](const ActivityResult& confirm) {
                                   if (confirm.isCancelled) {
                                     requestUpdate();
                                     return;
                                   }
                                   finishBookFile(/*remove=*/true);
                                 });
          return;
        }
        requestUpdate();
      });
}

// 読了した本をアーカイブまたは削除してホームへ戻る。失敗しても本は開いたままに
// せず、ホームへ戻す（読了画面に戻っても再試行以外にできることがない）。
void EpubReaderActivity::finishBookFile(bool remove) {
  if (!epub) return;
  const std::string path = epub->getPath();
  {
    // キャッシュ削除・ファイル移動中にレンダリングが走らないようにする
    RenderLock lock(*this);
    section.reset();
    const bool ok = remove ? BookFileHelper::remove(path, /*isDirectory=*/false)
                           : BookFileHelper::archive(path, /*isDirectory=*/false);
    if (!ok) {
      LOG_ERR("ERS", "Failed to %s finished book: %s", remove ? "delete" : "archive", path.c_str());
    }
  }
  // 次回起動時に消えたパスを開こうとしないようにする。onExit() が保存する。
  APP_STATE.openEpubPath = "";
  onGoHome();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // loop() から頼まれた次のページの用意だけをする（画面は描き直さない）
  if (prefetchRequested.exchange(false)) {
    if (section) {
      const unsigned long prefetchStart = millis();
      prefetchNextPage(prefetchViewportW, prefetchViewportH);
      debugPrefetchMs = millis() - prefetchStart;
    }
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Resolve effective writing mode before viewport calculation (needed for direction-specific settings).
  // Resolved before the end-of-book screen: handleInput() mirrors the button
  // direction off verticalMode there, and resuming a book that was saved at the
  // end returns early below, so the flag would otherwise still hold its default.
  if (!section) {
    if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
      verticalMode = true;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
      verticalMode = false;
    } else {
      // Auto: check OPF hints
      verticalMode = epub && epub->isPageProgressionRtl() &&
                     (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" || epub->getLanguage() == "zh" ||
                      epub->getLanguage() == "zho");
    }
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    saveProgress(currentSpineIndex, 0, 0, true, 100);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += ds.screenMargin;
  orientedMarginLeft += ds.screenMargin;
  orientedMarginRight += ds.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(ds.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(ds.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // 画像だけのページは「画面余白」を無視して、本体の見える範囲いっぱいに出す。
  // ステータスバー（または自動めくりの表示）がある間は、下はそのぶんだけ空けておく
  {
    int bezelTop, bezelRight, bezelBottom, bezelLeft;
    renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
    const bool reserveBottom = statusBarHeight > 0 || automaticPageTurnActive;
    const int bottom = reserveBottom ? orientedMarginBottom : bezelBottom;
    fullBleedX = static_cast<int16_t>(bezelLeft - orientedMarginLeft);
    fullBleedY = static_cast<int16_t>(bezelTop - orientedMarginTop);
    fullBleedW = static_cast<uint16_t>(renderer.getScreenWidth() - bezelLeft - bezelRight);
    fullBleedH = static_cast<uint16_t>(renderer.getScreenHeight() - bezelTop - bottom);
  }

  if (!section) {
    // Ensure the correct SD card font is loaded for the resolved writing direction.
    // goToReader() calls ensureSdFontLoaded(false) before verticalMode is known,
    // so we reload here with the correct direction after resolution.
    ensureSdFontLoaded(verticalMode);

    // ルビ用フォント: フォントロード後に8ptフォントを取得
    // rubyEnabled が OFF の場合は rubyFontId=0 でルビ描画をスキップ
    {
      const auto& rubyDs = SETTINGS.getDirectionSettings(verticalMode);
      if (!rubyDs.rubyEnabled) {
        TextBlock::rubyFontId = 0;
      } else {
        static constexpr uint8_t RUBY_FONT_SIZE_ENUM = 5;  // 8pt
        int rubyId = 0;
        if (rubyDs.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
          rubyId = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, rubyDs.sdFontFamilyName, RUBY_FONT_SIZE_ENUM);
        }
        if (rubyId == 0) rubyId = SETTINGS.getReaderFontId(verticalMode);
        TextBlock::rubyFontId = rubyId;
      }
    }

    // <sup> / <sub> 用の小さいフォント。ルビと同じ 8pt を使うが、ルビ表示の ON/OFF
    // とは無関係に必要なので別に解決する。SD カードフォントを引けない場合は 0 のままで、
    // TextBlock 側が本文フォントにフォールバックする（従来どおりの見た目）。
    {
      static constexpr uint8_t SCRIPT_FONT_SIZE_ENUM = 5;  // 8pt
      int smallId = 0;
      const auto& scriptDs = SETTINGS.getDirectionSettings(verticalMode);
      if (scriptDs.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
        smallId =
            SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, scriptDs.sdFontFamilyName, SCRIPT_FONT_SIZE_ENUM);
      }
      TextBlock::smallFontId = smallId;
    }

    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    const unsigned long sectionStart = millis();
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    section->setFullBleedArea(fullBleedX, fullBleedY, fullBleedW, fullBleedH);

    const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u, vertical=%d", ds.lineSpacing,
            lineCompression, viewportWidth, viewportHeight, verticalMode);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing,
                                  ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                  ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode,
                                  ds.charSpacing)) {
      LOG_DBG("ERS", "Cache not found, building...");

      // Apply vertical character spacing for layout calculation
      renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

      // Free SD card font data before section building to maximize available heap.
      // clearCache() frees prewarm data (~130KB: miniGlyphs + miniBitmap).
      // freeKernLigatureData() frees kern/ligature tables (~22KB per style).
      // Both are lazy-loaded again during the render pass.
      auto* fcm = renderer.getFontCacheManager();
      if (fcm) {
        fcm->clearCache();
        fcm->freeKernLigatureData();
      }

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      const int headingFontIds[6] = {
          SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};

      section->setFullBleedArea(fullBleedX, fullBleedY, fullBleedW, fullBleedH);
      if (!section->createSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing,
                                      ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                      ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode,
                                      ds.charSpacing, popupFn, headingFontIds, SETTINGS.getTableFontId(verticalMode))) {
        LOG_ERR("ERS", "Failed to persist page data to SD (free heap: %d)", ESP.getFreeHeap());
        section.reset();
        // Show error and return to home to avoid infinite retry loop
        // (loop() would call requestUpdate() → render() → same failure)
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        vTaskDelay(pdMS_TO_TICKS(2000));
        onGoHome();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }
    debugSectionMs = millis() - sectionStart;
    snprintf(debugSectionNote, sizeof(debugSectionNote), "%s", section->loadNote);

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    // 段表示中で、画像 1 枚だけのページなら段を横向きで出す（できなければ普通に描く）
    const bool isLoneImage = loneImage(p.get()) != nullptr;
    bool renderedTier = false;
    if (tierMode && isLoneImage) {
      renderedTier = renderTier(loneImage(p.get())->getImagePath());
    } else {
      tierCount = 0;
      tierStartAtEnd = false;
    }
    if (!renderedTier) {
      renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    }
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    debugDrawMs = millis() - start;
    prefetchViewportW = viewportWidth;
    prefetchViewportH = viewportHeight;
    prefetchDueAt = millis() + PREFETCH_IDLE_MS;
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  {
    const int percent = calculateBookPercent(section->currentPage, section->pageCount);
    const bool nearEnd = percent >= 95;
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount, nearEnd, percent);
  }

  if (SETTINGS.debugDisplay) writeDebugLog();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

// 設定の「デバッグ表示」がオンのときだけ、このページの準備にかかった時間を左上に出す。
// 章=章の読み込み／作成、描画=画像の展開と画面への描き込み、先読み=前のページで次（このページ）を用意した時間
// 2 行目: 章キャッシュの結果（hit=使えた / kept=設定違いだが画像だけなので使った / none=無かった /
// miss=設定違いで作り直した。後ろは「保存値/今の値」）、JPEG から画像を作った回数、段を作った回数。
// 同じ内容を /.crosspoint/read_perf.log にも追記する（カードを PC で開いて回収できる）
// 画面左上の数字を、白黒パスのフレームバッファに描く（画面の書き換えはしない）。
// 書き換えを 1 回足すと、X3 の灰色表示（待受の壁紙など）が前提にしているパネルの状態が崩れて、
// 少したってから色が反転する。draw はまだ測り終わっていないので 1 つ前のページの値を出す
void EpubReaderActivity::drawDebugOverlay() {
  char line1[64];
  snprintf(line1, sizeof(line1), "sec %lu  draw(prev) %lu  pre %lu ms", debugSectionMs, debugDrawMs, debugPrefetchMs);
  char line2[128];
  snprintf(line2, sizeof(line2), "sec:%s dec:%d tier:%d", debugSectionNote[0] ? debugSectionNote : "-",
           ImageBlock::decodeCount - debugDecodeBase, image_tiers::buildCount - debugTierBase);
  renderer.fillRect(0, 0, renderer.getScreenWidth(), DEBUG_OVERLAY_H, false);
  renderer.drawText(UI_10_FONT_ID, 6, 4, line1, true);
  renderer.drawText(UI_10_FONT_ID, 6, 28, line2, true);
}

// 灰色パスでは数字の欄に灰色の印を付けない（白黒パスの文字をそのまま見せる）
void EpubReaderActivity::clearDebugOverlayGray() const {
  renderer.fillRect(0, 0, renderer.getScreenWidth(), DEBUG_OVERLAY_H, true);
}

// このページの計測値を /.crosspoint/read_perf.log に書いて、次のページに向けて数え直す
void EpubReaderActivity::writeDebugLog() {
  const int decoded = ImageBlock::decodeCount - debugDecodeBase;
  const int tiersBuilt = image_tiers::buildCount - debugTierBase;
  debugDecodeBase = ImageBlock::decodeCount;
  debugTierBase = image_tiers::buildCount;

  char line1[64];
  snprintf(line1, sizeof(line1), "sec %lu  draw %lu  pre %lu ms", debugSectionMs, debugDrawMs, debugPrefetchMs);
  char line2[128];
  snprintf(line2, sizeof(line2), "sec:%s dec:%d tier:%d", debugSectionNote[0] ? debugSectionNote : "-", decoded,
           tiersBuilt);

  char logLine[320];
  const int len =
      snprintf(logLine, sizeof(logLine), "spine=%d page=%d tier=%d %s | %s | pre:%s | %s | heap %d max %d\n",
               currentSpineIndex, section ? section->currentPage : -1, tierMode ? tierIndex : -1, line1, line2,
               debugPrefetchNote[0] ? debugPrefetchNote : "-", debugRenderNote[0] ? debugRenderNote : "-",
               static_cast<int>(ESP.getFreeHeap()), static_cast<int>(ESP.getMaxAllocHeap()));
  if (len > 0) {
    auto file = Storage.open("/.crosspoint/read_perf.log", O_WRONLY | O_CREAT | O_APPEND);
    if (file) {
      if (file.size() > 64 * 1024) {
        file.close();
        Storage.remove("/.crosspoint/read_perf.log");
        file = Storage.open("/.crosspoint/read_perf.log", O_WRONLY | O_CREAT | O_APPEND);
      }
      if (file) {
        file.write(logLine, std::min(static_cast<size_t>(len), sizeof(logLine) - 1));
        file.close();
      }
    }
  }

  debugSectionMs = 0;
  debugPrefetchMs = 0;
  debugSectionNote[0] = '\0';
  debugPrefetchNote[0] = '\0';
  debugRenderNote[0] = '\0';
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  loadOrBuildSection(nextSection, viewportWidth, viewportHeight);
}

// 章のキャッシュがあれば読み、なければ（画面を出さずに）作る。読めたか作れたら true
bool EpubReaderActivity::loadOrBuildSection(Section& target, const uint16_t viewportWidth,
                                            const uint16_t viewportHeight) {
  const auto& silentDs = SETTINGS.getDirectionSettings(verticalMode);
  target.setFullBleedArea(fullBleedX, fullBleedY, fullBleedW, fullBleedH);
  if (target.loadSectionFile(SETTINGS.getReaderFontId(verticalMode), SETTINGS.getReaderLineCompression(verticalMode),
                             silentDs.extraParagraphSpacing, silentDs.paragraphAlignment, viewportWidth, viewportHeight,
                             silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle,
                             SETTINGS.imageRendering, verticalMode, silentDs.charSpacing)) {
    return true;
  }

  LOG_DBG("ERS", "Silently indexing chapter: %d", target.getSpineIndex());
  const int silentHeadingFontIds[6] = {
      SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};
  if (!target.createSectionFile(SETTINGS.getReaderFontId(verticalMode), SETTINGS.getReaderLineCompression(verticalMode),
                                silentDs.extraParagraphSpacing, silentDs.paragraphAlignment, viewportWidth,
                                viewportHeight, silentDs.hyphenationEnabled, silentDs.firstLineIndent,
                                SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode, silentDs.charSpacing,
                                nullptr, silentHeadingFontIds, SETTINGS.getTableFontId(verticalMode))) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", target.getSpineIndex());
    return false;
  }
  return true;
}

// 次のページ（同じ章の次、なければ次の章の先頭）を、今のページを表示し終えた裏で用意しておく。
// 次の章のキャッシュを作り、画像だけのページなら、段表示中は段を、そうでなければ表示用の画像を作る。
// 次のページに進んだとき、章の準備や JPEG の展開を待たずに済む（漫画は 1 ページ 1 章のことが多い）
void EpubReaderActivity::prefetchNextPage(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!section || !epub) return;
  if (prefetchedSpine == currentSpineIndex && prefetchedPage == section->currentPage &&
      prefetchedTierMode == tierMode) {
    return;
  }
  prefetchedSpine = currentSpineIndex;
  prefetchedPage = section->currentPage;
  prefetchedTierMode = tierMode;

  std::unique_ptr<Page> next;
  if (section->currentPage + 1 < section->pageCount) {
    const int saved = section->currentPage;
    section->currentPage = saved + 1;
    next = section->loadPageFromSectionFile();
    section->currentPage = saved;
  } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
    Section nextSection(epub, currentSpineIndex + 1, renderer);
    const bool loaded = loadOrBuildSection(nextSection, viewportWidth, viewportHeight);
    snprintf(debugPrefetchNote, sizeof(debugPrefetchNote), "%s", nextSection.loadNote);
    if (!loaded || nextSection.pageCount == 0) return;
    nextSection.currentPage = 0;
    next = nextSection.loadPageFromSectionFile();
  }
  const ImageBlock* image = loneImage(next.get());
  if (!image) return;
  if (tierMode) {
    LOG_DBG("ERS", "Prefetching tiers: %s", image->getImagePath().c_str());
    image_tiers::prepare(image->getImagePath(), tierAreaWidth());
  } else {
    LOG_DBG("ERS", "Prefetching image: %s", image->getImagePath().c_str());
    image->ensureJpegBmpCache();
  }
}

int EpubReaderActivity::calculateBookPercent(int currentPage, int pageCount) const {
  if (!epub || epub->getBookSize() == 0 || pageCount <= 0) {
    return -1;
  }
  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  return clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished, int percent) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    // 形式は docs/file-formats.md の progress.bin を参照。末尾の進捗率は後から追加したフィールドで、
    // 旧ファイル（7 バイト）は ReadingProgress::PERCENT_UNKNOWN 扱いになる
    uint8_t data[8];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    data[6] = isFinished ? 1 : 0;
    data[7] = (percent >= 0 && percent <= 100) ? static_cast<uint8_t>(percent) : ReadingProgress::PERCENT_UNKNOWN;
    f.write(data, sizeof(data));
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d, Finished: %d, %d%%", spineIndex, currentPage, isFinished,
            percent);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
// 段の BMP を横向き（本体を左に倒した向き＝LandscapeCounterClockwise）で、見える範囲いっぱいに中央寄せで描く。
// 白黒と灰色 2 回の計 3 回描くので、ファイルは呼び出し側で 1 回だけ開いて渡す
void EpubReaderActivity::drawTierBitmap(HalFile& file) const {
  file.seek(0);
  Bitmap bmp(file);
  if (bmp.parseHeaders() == BmpReaderError::Ok) {
    const auto saved = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
    int top, right, bottom, left;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int areaW = renderer.getScreenWidth() - left - right;
    const int areaH = renderer.getScreenHeight() - top - bottom;
    // drawBitmap は縮小だけするので、描かれる大きさを同じ式で求めて中央に置く
    const float scale = std::min(
        1.0f, std::min(static_cast<float>(areaW) / bmp.getWidth(), static_cast<float>(areaH) / bmp.getHeight()));
    const int w = static_cast<int>(bmp.getWidth() * scale);
    const int h = static_cast<int>(bmp.getHeight() * scale);
    renderer.drawBitmap(bmp, left + (areaW - w) / 2, top + (areaH - h) / 2, areaW, areaH);
    renderer.setOrientation(saved);
  }
}

// 画像だけのページを段に分け、今の段を横向きで表示する。段に分けられなければ false（普通に描く）
// 段の幅＝横向き（左倒し）にしたときの見える範囲の幅
int EpubReaderActivity::tierAreaWidth() const {
  const auto saved = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int areaW = renderer.getScreenWidth() - left - right;
  renderer.setOrientation(saved);
  return areaW;
}

bool EpubReaderActivity::renderTier(const std::string& imagePath) {
  const int areaW = tierAreaWidth();

  tierCount = image_tiers::preparedCount(imagePath, areaW);
  if (tierCount < 0) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));  // 初めてのページは展開に数秒かかる
    tierCount = image_tiers::prepare(imagePath, areaW);
  }
  if (tierCount <= 0) {
    tierCount = 0;
    tierStartAtEnd = false;
    return false;
  }
  if (tierStartAtEnd) {
    tierIndex = tierCount - 1;
    tierStartAtEnd = false;
  }
  tierIndex = std::clamp(tierIndex, 0, tierCount - 1);
  HalFile file = Storage.open(image_tiers::tierPath(imagePath, tierIndex).c_str());
  if (!file.isOpen()) {
    tierCount = 0;
    return false;
  }

  renderer.clearScreen();
  drawTierBitmap(file);
  if (SETTINGS.debugDisplay) drawDebugOverlay();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  // 普通の画像ページと同じく、アンチエイリアスが有効なら灰色の 2 パスを重ねる（白黒を保存できたときだけ）
  const bool bwStored = renderer.storeBwBuffer();
  if (bwStored && SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    drawTierBitmap(file);
    if (SETTINGS.debugDisplay) clearDebugOverlayGray();
    renderer.copyGrayscaleLsbBuffers();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    drawTierBitmap(file);
    if (SETTINGS.debugDisplay) clearDebugOverlayGray();
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  if (bwStored) renderer.restoreBwBuffer();
  file.close();
  return true;
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const int viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const auto t0 = millis();

  // Preload external font glyphs: collect codepoints from page, sort them,
  // and batch-read from SD sequentially. Much faster than random reads during render.
  FontManager& fm = FontManager::getInstance();
  if (fm.isExternalFontEnabled()) {
    ExternalFont* extFont = fm.getActiveFont();
    if (extFont) {
      std::vector<uint32_t> codepoints;
      page->collectCodepoints(codepoints, extFont->getCacheCapacity());
      if (!codepoints.empty()) {
        extFont->preloadGlyphs(codepoints.data(), codepoints.size());
      }
    }
  }

  // Apply vertical character spacing setting for this render
  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  // 画像の表示用 BMP は、文字の先読み（フォントのデータを確保する）より前に作っておく。JPEG の展開は
  // 空きメモリが 52KB 以上ないと断られるので、文字と画像が同じページにあると先読みのあとでは足りず、
  // 画像だけ出ないことがあった（実機の詰将棋の盤面）。scope を作った時点でフォントのキャッシュは空
  for (const auto& element : page->elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).getImageBlock().ensureJpegBmpCache();
    }
  }
  page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
               viewportWidth);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing;

  // アンチエイリアスするときだけ、BW パスでグリフのフチまで黒く敷いておく
  // （後段のグレー2パスがその画素を灰色へ持ち上げる）。しないときは 50% しきい値の
  // Sharp のままにする。フチまで黒くすると漢字の画がつぶれて解像感が落ちるため。
  const bool grayscaleText = SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing &&
                             !FontManager::getInstance().isExternalFontEnabled();
  renderer.setGlyphInk(grayscaleText ? GfxRenderer::GlyphInk::GrayscaleBase : GfxRenderer::GlyphInk::Sharp);
  page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop, viewportWidth);
  // ステータスバーはグレーパスで描き直さないので常に Sharp で描く
  renderer.setGlyphInk(GfxRenderer::GlyphInk::Sharp);
  renderStatusBar();
  if (SETTINGS.debugDisplay) drawDebugOverlay();
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      renderer.setGlyphInk(grayscaleText ? GfxRenderer::GlyphInk::GrayscaleBase : GfxRenderer::GlyphInk::Sharp);
      page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                   viewportWidth);
      renderer.setGlyphInk(GfxRenderer::GlyphInk::Sharp);
      if (SETTINGS.debugDisplay) drawDebugOverlay();  // 画像を描き直すと上に重なるので描き直す
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync.
  // 保存できなかった（メモリ不足）ときは灰色を重ねない。重ねると白黒の画面に戻せず、フレームバッファが
  // 灰色用の中身のまま残って、次の書き換えで黒地に白い点の画面になる
  const bool bwStored = renderer.storeBwBuffer();
  const auto tBwStore = millis();
  if (!bwStored) LOG_ERR("ERS", "BW buffer store failed (free heap %d), skipping grayscale", ESP.getFreeHeap());

  // Grayscale rendering - skip for external fonts (1-bit bitmap, no antialiasing benefit)
  if (grayscaleText && bwStored) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                 viewportWidth);
    if (SETTINGS.debugDisplay) clearDebugOverlayGray();
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                 viewportWidth);
    if (SETTINGS.debugDisplay) clearDebugOverlayGray();
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    snprintf(debugRenderNote, sizeof(debugRenderNote), "bw%lu disp%lu lsb%lu msb%lu gdisp%lu", tBwRender - tPrewarm,
             tDisplay - tBwRender, tGrayLsb - tBwStore, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb);
  } else {
    // restore the bw data
    if (bwStored) renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
    snprintf(debugRenderNote, sizeof(debugRenderNote), "bw%lu disp%lu", tBwRender - tPrewarm, tDisplay - tBwRender);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, verticalMode);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
