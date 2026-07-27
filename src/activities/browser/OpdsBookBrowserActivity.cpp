#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/book24.h"
#include "components/icons/folder24.h"
#include "components/icons/search24.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/OpdsCoverCache.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace {
// Text-list layout (cover thumbnails disabled): compact 30 px rows from y=60.
constexpr int TEXT_PAGE_ITEMS = 23;
constexpr int TEXT_ROW_HEIGHT = 30;
constexpr int TEXT_LIST_TOP = 60;

// Header + search affordance (shared by both list layouts).
constexpr int HEADER_Y = 15;
constexpr int HEADER_X = 16;
constexpr int SEARCH_ICON_SIZE = 24;
constexpr int SEARCH_ICON_MARGIN = 14;
constexpr int SEARCH_ICON_Y = 15;

// Rich-list layout (cover thumbnails enabled): a cover on the left, stacked
// metadata on the right. Row height is derived from font line heights (see
// rowHeight()) so the four metadata lines never overlap. The header title is
// drawn at y=15 in UI_12 (ink bottom ~44); starting at 48 clears it by a few px
// while still fitting the maximum rows on an X3 (792 tall: 7 rows vs 6 at >=50).
constexpr int RICH_LIST_TOP = 48;
constexpr int RICH_ROW_PAD = 5;
constexpr int RICH_SIDE_MARGIN = 10;
constexpr int RICH_TEXT_GAP = 10;   // Gap between thumbnail and text column
constexpr int RICH_META_LINES = 2;  // Metadata lines below the title (format+author/series, summary)
// Extra spacing added to each line's advance. The UI fonts render slightly taller
// than their reported line height, so plain line-height stacking packs them too
// tightly; this keeps the lines (notably the format footer) clearly separated.
constexpr int RICH_LINE_GAP = 4;
constexpr int ICON_SIZE = 24;  // Placeholder icon assets are 24x24 (drawIcon does not scale)

constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Long-press duration on Confirm to trigger a bulk "download all" of the feed
// (matches the hold-to-act threshold used elsewhere, e.g. RecentBooksActivity).
constexpr unsigned long BULK_LONG_PRESS_MS = 1000;
// Hard cap on feed pages walked during a bulk scan/download, so a malformed
// next-page link can't loop forever.
constexpr int MAX_BULK_FEED_PAGES = 100;

// Defer cover fetch/decode until input has been idle this long. This must be
// comfortably longer than one e-ink refresh + a human's re-press cadence — a
// short window reopens between single presses, so a blocking decode fires right
// before the next press and every scroll step pays for a fetch. Covers instead
// load once the user actually pauses.
constexpr unsigned long COVER_IDLE_MS = 900;

// Entries held per feed page when cover thumbnails are on. Smaller than the
// parser default to leave heap for one cover decode with WiFi up (~272 bytes per
// held entry). Larger catalogs page via the feed's next/prev links.
constexpr size_t COVER_MODE_MAX_ENTRIES = 38;

// Auto-retry covers that were skipped transiently (low heap / flaky download):
// re-arm the visible page's skipped covers every interval, a bounded number of
// times, so they load without the user re-entering the page.
constexpr unsigned long COVER_RETRY_INTERVAL_MS = 1500;
constexpr int MAX_COVER_RETRY_ROUNDS = 6;

// Short format label from an acquisition MIME type, e.g. "EPUB". Empty if unknown.
std::string formatLabel(const std::string& mediaType) {
  if (mediaType.find("epub") != std::string::npos) return "EPUB";
  if (mediaType.find("pdf") != std::string::npos) return "PDF";
  return "";
}

// Human-readable file size, e.g. "1.2 MB". Empty when size is unknown.
std::string formatSize(uint64_t bytes) {
  if (bytes == 0) return "";
  char buf[24];
  if (bytes >= 1024ULL * 1024ULL) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

// Leading 4-digit year from an ISO 8601 publication date; empty if not a year.
std::string yearOf(const std::string& published) {
  if (published.size() < 4) return "";
  for (int i = 0; i < 4; i++) {
    if (published[i] < '0' || published[i] > '9') return "";
  }
  return published.substr(0, 4);
}

Rect searchIconRect(const GfxRenderer& renderer) {
  return Rect{renderer.getScreenWidth() - SEARCH_ICON_SIZE - SEARCH_ICON_MARGIN, SEARCH_ICON_Y, SEARCH_ICON_SIZE + 8,
              SEARCH_ICON_SIZE + 8};
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  coverStates.clear();
  downloadedFlags.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  bulkLongPressFired = false;
  bulkCancel = false;
  bulkTotalCount = bulkCurrentIndex = bulkOkCount = bulkFailCount = 0;
  bulkSummary.clear();
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  coversEnabled = SETTINGS.opdsCoverThumbnails != 0;
  richListPainted = false;

  // Bound the on-SD cover cache once per browse session (cheap, off the render path).
  if (coversEnabled) OpdsCoverCache::enforceBudget();

  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  coverStates.clear();
  downloadedFlags.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  // Bulk scan/download drives its own UI from within promptBulkDownload/
  // runBulkDownload (blocking, cancel handled inside the progress callback).
  if (state == BrowserState::DOWNLOADING || state == BrowserState::BULK_DOWNLOADING) return;

  if (state == BrowserState::BULK_DONE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      richListPainted = false;
      requestUpdate();
    }
    return;
  }

  if (state == BrowserState::BROWSING) {
    // Long-press Confirm: bulk-download every not-yet-present book in this feed.
    // The guard swallows input until Confirm is physically released, so the
    // release that ends the hold doesn't also trigger a single download/navigate
    // (firmware hold-to-act pattern, cf. RecentBooksActivity).
    if (bulkLongPressFired) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) bulkLongPressFired = false;
      return;
    }
    if (!entries.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= BULK_LONG_PRESS_MS) {
      bulkLongPressFired = true;
      promptBulkDownload();
      return;
    }

    auto activateSelected = [this] {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    };

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    int tx = 0;
    int ty = 0;
    if (!searchTemplate.empty() && mappedInput.wasScreenTapped(tx, ty) && contains(searchIconRect(renderer), tx, ty)) {
      launchSearch();
      return;
    }

    if (!entries.empty()) {
      const int pageItems = itemsPerPage();
      // Row geometry follows the active list layout (rich covers vs. compact text).
      const int listTop = coversEnabled ? RICH_LIST_TOP : TEXT_LIST_TOP;
      const int rowStep = coversEnabled ? rowHeight() : TEXT_ROW_HEIGHT;

      int row = -1;
      const auto touch = mappedInput.rowTouch(row, listTop, rowStep, pageItems);
      if (touch != MappedInputManager::RowTouch::None) {
        const int touched = selectorIndex / pageItems * pageItems + row;
        if (touched >= 0 && touched < static_cast<int>(entries.size())) {
          if (touch == MappedInputManager::RowTouch::Down) {
            if (selectorIndex != touched) {
              selectorIndex = touched;
              lastInteractionMs = millis();
              requestUpdate();
            }
          } else {
            selectorIndex = touched;
            activateSelected();
          }
          return;
        }
      }

      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), pageItems);
        lastInteractionMs = millis();
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), pageItems);
        lastInteractionMs = millis();
        requestUpdate();
        return;
      }

      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        lastInteractionMs = millis();
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        lastInteractionMs = millis();
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this, pageItems] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), pageItems);
        lastInteractionMs = millis();
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this, pageItems] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), pageItems);
        lastInteractionMs = millis();
        requestUpdate();
      });
    }

    // Lazily fetch/decode one cover per loop — visible rows first, then prefetch
    // off-screen ones so scrolling into them is instant (never more than one decode
    // in flight; skips under heap pressure). Runs after input, and only once input
    // has been idle, so navigation stays responsive.
    if (coversEnabled) loadNextCover();
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  // Fast path: a same-page selection move in the rich cover list. Rather than
  // clearing and re-blitting every visible cover, repaint only the two rows whose
  // highlight changed — the old row loses its grey fill, the new one gains it.
  // Everything else already in the framebuffer is left untouched.
  // Snapshot selectorIndex once: loop() mutates it from the main task without the
  // render lock, so re-reading it mid-render would let paintedSelector diverge from
  // the row actually filled — leaving the old highlight uncleared on fast scrolls.
  const int sel = selectorIndex;
  if (state == BrowserState::BROWSING && coversEnabled && !entries.empty() && richListPainted) {
    const int pageItems = itemsPerPage();
    const int pageStart = sel / pageItems * pageItems;
    if (pageStart == paintedPageStart && hintLabelsSame(paintedSelector, sel)) {
      if (paintedSelector != sel) {
        const int rowH = rowHeight();
        const int oldRowY = RICH_LIST_TOP + (paintedSelector % pageItems) * rowH;
        const int newRowY = RICH_LIST_TOP + (sel % pageItems) * rowH;
        renderRichRow(paintedSelector, oldRowY, rowH, false);  // clear old highlight
        renderRichRow(sel, newRowY, rowH, true);               // draw new highlight
        paintedSelector = sel;
      }
      renderer.displayBuffer();
      return;
    }
  }

  // Full render invalidates the fast-path state until the rich list repaints it.
  richListPainted = false;

  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  const int headerRightInset = searchTemplate.empty() ? HEADER_X : (SEARCH_ICON_SIZE + SEARCH_ICON_MARGIN * 2 + 8);
  const auto clippedHeader =
      renderer.truncatedText(UI_12_FONT_ID, headerTitle, pageWidth - HEADER_X - headerRightInset, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, HEADER_X, HEADER_Y, clippedHeader.c_str(), true, EpdFontFamily::BOLD);
  if (!searchTemplate.empty()) {
    const auto rect = searchIconRect(renderer);
    renderer.drawIcon(Search24Icon.bits, rect.x + 4, rect.y + 4, Search24Icon.w);
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    if (mappedInput.hasTouch()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, tr(STR_TAP_TO_RETRY));
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::BULK_DOWNLOADING) {
    if (bulkTotalCount == 0) {
      // Scan phase: no per-book progress yet.
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SCANNING));
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 55, tr(STR_DOWNLOADING));
      char counter[32];
      snprintf(counter, sizeof(counter), tr(STR_OPDS_DOWNLOADING_N), bulkCurrentIndex, bulkTotalCount);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, counter);
      if (!statusMessage.empty()) {
        auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 5, title.c_str());
      }
      if (downloadTotal > 0) {
        GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                            downloadTotal);
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::BULK_DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, bulkSummary.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else if (coversEnabled) {
    renderRichList();
  } else {
    renderTextList();
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::renderTextList() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageStartIndex = selectorIndex / TEXT_PAGE_ITEMS * TEXT_PAGE_ITEMS;
  renderer.fillRect(0, TEXT_LIST_TOP + (selectorIndex % TEXT_PAGE_ITEMS) * TEXT_ROW_HEIGHT - 2, pageWidth - 1,
                    TEXT_ROW_HEIGHT);

  for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + TEXT_PAGE_ITEMS);
       i++) {
    const auto& entry = entries[i];
    std::string displayText = (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : entry.title;
    if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
    const int rowTop = TEXT_LIST_TOP + (i % TEXT_PAGE_ITEMS) * TEXT_ROW_HEIGHT;
    const bool foreground = i != static_cast<size_t>(selectorIndex);
    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
    renderer.drawText(UI_10_FONT_ID, 20, rowTop, item.c_str(), foreground);
    // Downloaded books get a checkmark in the left gutter (no cover here).
    if (entry.type == OpdsEntryType::BOOK && i < downloadedFlags.size() && downloadedFlags[i]) {
      drawCheck(3, rowTop + 5, 14, foreground);
    }
  }
}

void OpdsBookBrowserActivity::renderRichList() {
  const int pageItems = itemsPerPage();
  const int rowH = rowHeight();
  // Snapshot selectorIndex once so the highlighted row and paintedSelector agree even
  // if loop() advances the selection mid-paint (see the fast path in render()).
  const int sel = selectorIndex;
  const int pageStartIndex = sel / pageItems * pageItems;
  for (int i = pageStartIndex; i < static_cast<int>(entries.size()) && i < pageStartIndex + pageItems; i++) {
    const int rowY = RICH_LIST_TOP + (i % pageItems) * rowH;
    renderRichRow(i, rowY, rowH, i == sel);
  }
  // The framebuffer now holds this page's covers + text + highlight, so a
  // subsequent same-page move can take the two-row repaint fast path in render().
  richListPainted = true;
  paintedPageStart = pageStartIndex;
  paintedSelector = sel;
}

bool OpdsBookBrowserActivity::hintLabelsSame(int a, int b) const {
  // Button-hint labels depend only on entry type (Download vs Open) and whether
  // the row is the searchable index 0 (Search vs Dir-Up). If both match, the hint
  // bar is unchanged and the outline-only fast path is safe.
  const auto isBook = [this](int i) { return entries[i].type == OpdsEntryType::BOOK; };
  const auto isSearchRow = [this](int i) { return !searchTemplate.empty() && i == 0; };
  return isBook(a) == isBook(b) && isSearchRow(a) == isSearchRow(b);
}

void OpdsBookBrowserActivity::renderRichRow(int entryIndex, int rowY, int rowHeight, bool selected) {
  const auto& entry = entries[entryIndex];
  const int pageWidth = renderer.getScreenWidth();

  // Selection highlight is drawn by the active theme so it matches its list style
  // (grey/black, rounded/square) and tells us whether to invert the foreground.
  // Unselected rows are cleared to white too, so the fast path in render() can
  // repaint just the two rows whose highlight changed.
  const int hlX = RICH_SIDE_MARGIN / 2;
  const int hlW = pageWidth - RICH_SIDE_MARGIN;
  const bool invert = GUI.drawListRowSelection(renderer, Rect{hlX, rowY, hlW, rowHeight}, selected);

  int thumbW, thumbH;
  thumbSize(thumbW, thumbH);
  const int thumbX = RICH_SIDE_MARGIN;
  const int thumbY = rowY + RICH_ROW_PAD;

  bool coverDrawn = false;
  if (entry.type == OpdsEntryType::BOOK && !entry.thumbnailUrl.empty() &&
      coverStates[entryIndex] == CoverState::Ready) {
    const std::string bmpPath = OpdsCoverCache::cachePath(resolveCoverUrl(entry.thumbnailUrl), thumbW, thumbH);
    HalFile file;
    if (Storage.openFileForRead("OPDS", bmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bitmap, thumbX, thumbY, thumbW, thumbH);
        renderer.drawRect(thumbX, thumbY, thumbW, thumbH, !invert);
        coverDrawn = true;
      }
    }
  }
  if (!coverDrawn) {
    // Placeholder box (pending cover, no cover, or navigation entry) with a
    // centered icon: folder for navigation, book for a book. drawIcon only inks
    // black, so skip it on an inverted (dark-highlight) row — the box still shows.
    renderer.drawRect(thumbX, thumbY, thumbW, thumbH, !invert);
    if (!invert) {
      const uint8_t* icon = (entry.type == OpdsEntryType::NAVIGATION) ? Folder24Icon : Book24Icon;
      renderer.drawIcon(icon, thumbX + (thumbW - ICON_SIZE) / 2, thumbY + (thumbH - ICON_SIZE) / 2, ICON_SIZE);
    }
  }

  // "Already on the SD card" badge in the thumbnail's top-right corner.
  if (entry.type == OpdsEntryType::BOOK && entryIndex < static_cast<int>(downloadedFlags.size()) &&
      downloadedFlags[entryIndex]) {
    constexpr int badge = 16;
    drawDownloadedBadge(thumbX + thumbW - badge - 1, thumbY + 1, badge, invert);
  }

  // Metadata column. Lines are stacked top-down; `lineTop` is the line-box top
  // (drawText adds the font ascender itself — see GfxRenderer::drawText). Each
  // advance includes RICH_LINE_GAP so the lines never crowd each other.
  const int textX = thumbX + thumbW + RICH_TEXT_GAP;
  const int textW = pageWidth - textX - RICH_SIDE_MARGIN;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID) + RICH_LINE_GAP;
  const int metaLineH = renderer.getLineHeight(UI_10_FONT_ID) + RICH_LINE_GAP;
  int lineTop = rowY + RICH_ROW_PAD;

  // Title (bold, truncated).
  auto title = renderer.truncatedText(UI_12_FONT_ID, entry.title.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, lineTop, title.c_str(), !invert, EpdFontFamily::BOLD);
  lineTop += titleLineH;

  // Author / series line.
  std::string authorLine = entry.author;
  if (!entry.series.empty()) {
    if (!authorLine.empty()) authorLine += " \xE2\x80\xA2 ";  // bullet
    authorLine += entry.series;
  }
  if (!authorLine.empty()) {
    auto line = renderer.truncatedText(UI_10_FONT_ID, authorLine.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, textX, lineTop, line.c_str(), !invert);
  }
  lineTop += metaLineH;

  // Facts line — format, size, year, and genre, as the feed provides them. Joined
  // with bullets; only non-empty parts appear, so sparse feeds simply show less.
  std::string facts;
  const auto addFact = [&facts](const std::string& part) {
    if (part.empty()) return;
    if (!facts.empty()) facts += " \xE2\x80\xA2 ";  // bullet
    facts += part;
  };
  if (entry.type == OpdsEntryType::BOOK) addFact(formatLabel(entry.mediaType));
  addFact(formatSize(entry.fileSizeBytes));
  addFact(yearOf(entry.published));
  addFact(entry.category);
  if (!facts.empty()) {
    auto line = renderer.truncatedText(UI_10_FONT_ID, facts.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, textX, lineTop, line.c_str(), !invert);
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  // With covers on, hold fewer entries per page so there's enough free heap to
  // decode a cover thumbnail with WiFi up (the vector is kept for the whole
  // browse session). Larger catalogs still page via the feed's next/prev links.
  // Text-only browsing keeps the full default page size.
  OpdsParser parser(coversEnabled ? COVER_MODE_MAX_ENTRIES : OpdsParser::DEFAULT_MAX_ENTRIES);
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  entries = std::move(parser).getEntries();

  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  // Parallel per-entry cover state, reset for the freshly loaded page. Treat the
  // load as an interaction so covers only start after the page has had a moment
  // to render (and don't stall a rapid drill-down through folders).
  coverStates.assign(entries.size(), CoverState::Unknown);
  // Flag entries already present on the SD card (one stat each, once per page).
  downloadedFlags.assign(entries.size(), 0);
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].type == OpdsEntryType::BOOK) {
      downloadedFlags[i] = Storage.exists(localFilename(entries[i]).c_str()) ? 1 : 0;
    }
  }
  lastInteractionMs = millis();
  nextCoverRetryMs = 0;
  coverRetryRounds = 0;
  richListPainted = false;  // new page — next render must be a full paint

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::releaseEntries() {
  std::vector<OpdsEntry>().swap(entries);
  std::vector<CoverState>().swap(coverStates);
  std::vector<uint8_t>().swap(downloadedFlags);
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    releaseEntries();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

std::string OpdsBookBrowserActivity::localFilename(const OpdsEntry& book) const {
  // Pure: the intended on-SD path for a book (configured download folder +
  // formatted, sanitized filename). Used for existence checks and the downloaded
  // badge, so it must match where resolveDownloadPath() actually writes. No I/O.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  std::string path;
  path.reserve(96);
  if (folder[0] != '\0') path += folder;
  path += '/';
  path += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

std::string OpdsBookBrowserActivity::resolveDownloadPath(const OpdsEntry& book) {
  // Same path as localFilename(), but creates the configured download folder if
  // needed, falling back to the SD root when it can't be created so a download is
  // never lost. May do I/O — call at download time, not for badge/existence checks.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }
  std::string path;
  path.reserve(96);
  if (haveFolder) path += folder;
  path += '/';
  path += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

void OpdsBookBrowserActivity::drawCheck(int x, int y, int size, bool state) {
  // Checkmark filling most of the size box: mid-left, down to the corner, up to
  // the top-right.
  const int x0 = x + size * 15 / 100, y0 = y + size * 55 / 100;
  const int x1 = x + size * 40 / 100, y1 = y + size * 80 / 100;
  const int x2 = x + size * 85 / 100, y2 = y + size * 20 / 100;
  renderer.drawLine(x0, y0, x1, y1, 2, state);
  renderer.drawLine(x1, y1, x2, y2, 2, state);
}

void OpdsBookBrowserActivity::drawDownloadedBadge(int x, int y, int size, bool invert) {
  // Filled chip with a checkmark in the top-right of the thumbnail. Colours
  // contrast with the row background (chip = foreground colour, check = bg).
  renderer.fillRoundedRect(x, y, size, size, 3, invert ? Color::White : Color::Black);
  drawCheck(x, y, size, invert);  // check drawn in the row's background colour
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  std::string filename = resolveDownloadPath(book);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    // Reflect the new local copy so the row shows the downloaded badge.
    if (selectorIndex >= 0 && static_cast<size_t>(selectorIndex) < downloadedFlags.size()) {
      downloadedFlags[selectorIndex] = 1;
      richListPainted = false;  // force a full repaint so the badge appears
    }
    state = BrowserState::BROWSING;
  } else {
    LOG_ERR("OPDS", "Download failed: %d", static_cast<int>(result));
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

bool OpdsBookBrowserActivity::fetchFeedPage(const std::string& pageUrl, std::vector<OpdsEntry>& outEntries,
                                            std::string& outNextPageUrl) {
  OpdsParser parser(OpdsParser::DEFAULT_MAX_ENTRIES);
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(pageUrl, stream, server.username, server.password)) return false;
  }
  if (!parser) return false;
  const auto& nextUrl = parser.getNextPageUrl();
  outNextPageUrl = nextUrl.empty() ? std::string() : UrlUtils::buildUrl(pageUrl, nextUrl);
  outEntries = std::move(parser).getEntries();
  return true;
}

void OpdsBookBrowserActivity::promptBulkDownload() {
  // Scan every page of the current feed (following next-page links; no descent
  // into sub-folders) for books not already on the SD card, so we can warn the
  // user how many will be downloaded before starting. Uses a local parser per
  // page — the displayed `entries` are left untouched so we can return to them.
  state = BrowserState::BULK_DOWNLOADING;
  statusMessage.clear();
  downloadProgress = downloadTotal = 0;
  bulkTotalCount = bulkCurrentIndex = 0;
  requestUpdate(true);

  size_t count = 0;
  uint64_t totalBytes = 0;
  bool sizeApprox = false;
  std::string pageUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::vector<OpdsEntry> pageEntries;
  std::string nextUrl;
  int pages = 0;
  bool firstFetchFailed = false;
  while (!pageUrl.empty() && pages < MAX_BULK_FEED_PAGES) {
    if (!fetchFeedPage(pageUrl, pageEntries, nextUrl)) {
      if (pages == 0) firstFetchFailed = true;
      break;  // best-effort: offer to download whatever we managed to scan
    }
    for (const auto& e : pageEntries) {
      if (e.type != OpdsEntryType::BOOK) continue;
      if (Storage.exists(localFilename(e).c_str())) continue;
      count++;
      if (e.fileSizeBytes == 0)
        sizeApprox = true;
      else
        totalBytes += e.fileSizeBytes;
    }
    pageUrl = nextUrl;
    pages++;
  }

  // These branches return straight to a Confirm-sensitive state (ERROR/BULK_DONE)
  // without an intervening sub-activity. The long-press Confirm is still physically
  // held, so swallow its pending release; otherwise it immediately dismisses BULK_DONE
  // or re-fetches from ERROR. (The count>0 path routes through ConfirmationActivity,
  // which reads Left/Right, so it needs no guard.)
  if (firstFetchFailed) {
    consumeConfirm = true;
    state = BrowserState::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  if (count == 0) {
    consumeConfirm = true;
    bulkSummary = tr(STR_OPDS_NO_NEW_BOOKS);
    state = BrowserState::BULK_DONE;
    requestUpdate();
    return;
  }

  bulkTotalCount = static_cast<int>(count);

  // Confirmation body: "<N> books" plus an approximate total size when the feed
  // advertised one (prefixed with ~ if any entry omitted its length).
  char body[64];
  const std::string sizeStr = formatSize(totalBytes);
  if (sizeStr.empty()) {
    snprintf(body, sizeof(body), tr(STR_OPDS_BULK_CONFIRM_COUNT), static_cast<int>(count));
  } else {
    const std::string sized = (sizeApprox ? "~" : "") + sizeStr;
    snprintf(body, sizeof(body), tr(STR_OPDS_BULK_CONFIRM_SIZE), static_cast<int>(count), sized.c_str());
  }

  // ConfirmationActivity overlays this activity; on cancel we resume in BROWSING.
  state = BrowserState::BROWSING;
  richListPainted = false;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DOWNLOAD_ALL), body),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           runBulkDownload();
                         });
}

void OpdsBookBrowserActivity::runBulkDownload() {
  state = BrowserState::BULK_DOWNLOADING;
  bulkCancel = false;
  bulkCurrentIndex = 0;
  bulkOkCount = bulkFailCount = 0;
  downloadProgress = downloadTotal = 0;
  statusMessage.clear();
  requestUpdate(true);

  std::string pageUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::vector<OpdsEntry> pageEntries;
  std::string nextUrl;
  int pages = 0;
  while (!pageUrl.empty() && pages < MAX_BULK_FEED_PAGES && !bulkCancel) {
    if (!fetchFeedPage(pageUrl, pageEntries, nextUrl)) break;
    for (const auto& e : pageEntries) {
      if (bulkCancel) break;
      if (e.type != OpdsEntryType::BOOK) continue;
      // resolveDownloadPath() (not localFilename) so the write and skip check use
      // the same path the folder-fallback would pick, and the folder is created.
      const std::string dest = resolveDownloadPath(e);
      if (Storage.exists(dest.c_str())) continue;  // already present, or grabbed earlier this run

      bulkCurrentIndex++;
      statusMessage = e.title;
      downloadProgress = downloadTotal = 0;
      requestUpdate(true);

      const std::string url = UrlUtils::buildUrl(pageUrl, e.href);
      LOG_DBG("OPDS", "Bulk downloading: %s -> %s", url.c_str(), dest.c_str());

      int lastRenderedPercent = -1;
      unsigned long lastProgressUpdateMs = 0;
      const auto result = HttpDownloader::downloadToFile(
          url, dest,
          [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
            downloadProgress = downloaded;
            downloadTotal = total;
            // Poll for a cancel press without leaving the blocking download.
            mappedInput.update();
            if (mappedInput.isPressed(MappedInputManager::Button::Back)) bulkCancel = true;
            const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
            const unsigned long now = millis();
            if (bulkCancel || percent >= 100 || lastRenderedPercent < 0 ||
                percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
                now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
              lastRenderedPercent = percent;
              lastProgressUpdateMs = now;
              requestUpdate(true);
            }
          },
          &bulkCancel, server.username, server.password);

      if (result == HttpDownloader::OK) {
        clearBookCache(dest);
        bulkOkCount++;
      } else if (result == HttpDownloader::ABORTED) {
        break;  // user cancelled; the downloader already removed the partial file
      } else {
        LOG_ERR("OPDS", "Bulk download failed: %d", static_cast<int>(result));
        bulkFailCount++;
      }
      vTaskDelay(1);  // yield between files so the watchdog is fed
    }
    pageUrl = nextUrl;
    pages++;
  }

  // Refresh the visible page's downloaded badges to reflect the new local copies.
  for (size_t i = 0; i < entries.size() && i < downloadedFlags.size(); i++) {
    if (entries[i].type == OpdsEntryType::BOOK) {
      downloadedFlags[i] = Storage.exists(localFilename(entries[i]).c_str()) ? 1 : 0;
    }
  }
  richListPainted = false;

  char summary[64];
  snprintf(summary, sizeof(summary), tr(STR_OPDS_BULK_DONE), bulkOkCount, bulkFailCount);
  bulkSummary = summary;
  state = BrowserState::BULK_DONE;
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

int OpdsBookBrowserActivity::rowHeight() const {
  // Title (UI_12) + RICH_META_LINES metadata lines (UI_10), each with an added
  // gap, plus top/bottom padding. Matches the per-line advances in renderRichRow().
  return RICH_ROW_PAD * 2 + (renderer.getLineHeight(UI_12_FONT_ID) + RICH_LINE_GAP) +
         RICH_META_LINES * (renderer.getLineHeight(UI_10_FONT_ID) + RICH_LINE_GAP);
}

int OpdsBookBrowserActivity::itemsPerPage() const {
  if (!coversEnabled) return TEXT_PAGE_ITEMS;
  // Reserve the theme's actual button-hint bar height so the last row can't spill
  // into it (the hints are drawn first, so an overspilling row would clip them).
  const int hintBar = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int usable = renderer.getScreenHeight() - RICH_LIST_TOP - hintBar;
  const int rows = usable / rowHeight();
  return rows < 1 ? 1 : rows;
}

void OpdsBookBrowserActivity::thumbSize(int& outW, int& outH) const {
  outH = rowHeight() - 2 * RICH_ROW_PAD;  // fill the row height minus padding
  outW = outH * 2 / 3;                    // typical book-cover aspect ratio
}

std::string OpdsBookBrowserActivity::resolveCoverUrl(const std::string& href) const {
  // Resolve the (possibly relative) cover href against the current feed URL, so
  // sub-navigated feeds keep the correct base. Query string is preserved by buildUrl.
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  return UrlUtils::buildUrl(feedUrl, href);
}

bool OpdsBookBrowserActivity::tryDecodeFirstUnknownCover(int start, int end, bool visibleRange) {
  for (int i = start; i < end; i++) {
    if (coverStates[i] != CoverState::Unknown) continue;

    const auto& e = entries[i];
    if (e.type != OpdsEntryType::BOOK || e.thumbnailUrl.empty()) {
      coverStates[i] = CoverState::None;  // nothing to fetch; keep scanning (cheap)
      continue;
    }

    int tw, th;
    thumbSize(tw, th);
    const std::string absUrl = resolveCoverUrl(e.thumbnailUrl);
    const auto status = OpdsCoverCache::ensure(absUrl, tw, th, server.username, server.password);
    switch (status) {
      case OpdsCoverCache::Status::Ready:
        coverStates[i] = CoverState::Ready;
        // Only repaint for on-screen covers; a prefetched off-screen cover just
        // lands in the cache and will blit from there when scrolled into view.
        if (visibleRange) {
          richListPainted = false;  // a new visible cover must be blitted — force a full repaint
          requestUpdate();          // progressive: redraw so this cover appears now
        }
        break;
      case OpdsCoverCache::Status::NoCover:
        coverStates[i] = CoverState::None;
        break;
      case OpdsCoverCache::Status::Skipped:
        // Low heap / network hiccup — leave it; re-armed later while still in view.
        coverStates[i] = CoverState::Skipped;
        break;
    }
    return true;  // did one fetch/decode this loop
  }
  return false;
}

void OpdsBookBrowserActivity::loadNextCover() {
  if (entries.empty() || coverStates.size() != entries.size()) return;

  // Stay out of the way while the user is actively navigating; only fetch/decode
  // once input has settled. A decode can block for seconds, so doing it mid-scroll
  // is what makes paging feel laggy.
  if (millis() - lastInteractionMs < COVER_IDLE_MS) return;

  const int pageItems = itemsPerPage();
  const int total = static_cast<int>(entries.size());
  const int pageStart = selectorIndex / pageItems * pageItems;
  int pageEnd = pageStart + pageItems;
  if (pageEnd > total) pageEnd = total;

  // Priority order: visible rows, then prefetch below (the likely scroll
  // direction), then above. One fetch per loop keeps the loop responsive.
  if (tryDecodeFirstUnknownCover(pageStart, pageEnd, true)) return;
  if (tryDecodeFirstUnknownCover(pageEnd, total, false)) return;
  if (tryDecodeFirstUnknownCover(0, pageStart, false)) return;

  // Nothing left to fetch. If visible covers were skipped transiently, re-arm them
  // for another attempt after a short delay — heap usually recovers once the feed's
  // TLS buffers are freed. Bounded so a genuinely dead cover URL doesn't loop forever.
  if (coverRetryRounds >= MAX_COVER_RETRY_ROUNDS || millis() < nextCoverRetryMs) return;
  bool rearmed = false;
  for (int i = pageStart; i < pageEnd; i++) {
    if (coverStates[i] == CoverState::Skipped) {
      coverStates[i] = CoverState::Unknown;
      rearmed = true;
    }
  }
  if (rearmed) {
    coverRetryRounds++;
    nextCoverRetryMs = millis() + COVER_RETRY_INTERVAL_MS;
  }
}
