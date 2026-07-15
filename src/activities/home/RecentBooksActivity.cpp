#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

// Cover-card layout. Row height is derived from the title (UI_12) + author (UI_10)
// line heights so text never overlaps; the cover fills the row minus padding.
constexpr int CARD_PAD = 6;
constexpr int CARD_SIDE = 12;      // Left/right row margin
constexpr int CARD_TEXT_GAP = 12;  // Gap between cover and text column
constexpr int CARD_LINE_GAP = 4;   // Extra per-line spacing (UI fonts render tall)

// Uppercase file extension as a format label, e.g. "/book.epub" -> "EPUB".
std::string formatFromPath(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return "";
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return ext;
}

// Human-readable file size, e.g. "1.4 MB". Empty when size is unknown.
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
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks = RECENT_BOOKS.getBooks();
  // Stat file sizes once for the card facts line, kept parallel to recentBooks so
  // scrolling (and reloads after a removal) never re-stat the SD per render.
  fileSizes.assign(recentBooks.size(), 0);
  for (size_t i = 0; i < recentBooks.size(); i++) {
    HalFile file = Storage.open(recentBooks[i].path.c_str(), O_RDONLY);
    if (file) fileSizes[i] = file.fileSize64();
  }
}

int RecentBooksActivity::rowHeight() const {
  // Title (UI_12) + two metadata lines (author, format+size) in UI_10.
  return CARD_PAD * 2 + (renderer.getLineHeight(UI_12_FONT_ID) + CARD_LINE_GAP) +
         2 * (renderer.getLineHeight(UI_10_FONT_ID) + CARD_LINE_GAP);
}

int RecentBooksActivity::contentTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + m.verticalSpacing;
}

int RecentBooksActivity::itemsPerPage() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int usable = renderer.getScreenHeight() - contentTop() - m.buttonHintsHeight - m.verticalSpacing;
  const int rows = usable / rowHeight();
  return rows < 1 ? 1 : rows;
}

void RecentBooksActivity::thumbSize(int& outW, int& outH) const {
  outH = rowHeight() - 2 * CARD_PAD;
  outW = outH * 2 / 3;  // typical book-cover aspect ratio
}

void RecentBooksActivity::ensureCovers(int thumbHeight) {
  bool showingPopup = false;
  Rect popup{};
  const int total = static_cast<int>(recentBooks.size());
  for (int i = 0; i < total; i++) {
    RecentBook& book = recentBooks[i];
    if (book.coverBmpPath.empty()) continue;  // known to have no cover
    if (Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight).c_str())) continue;

    const auto showProgress = [&] {
      if (!showingPopup) {
        showingPopup = true;
        popup = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popup, total > 0 ? (i * 100 / total) : 0);
    };

    // Local files only, so heap is ample (no WiFi) — generate like the home screen.
    bool handled = false;
    bool ok = false;
    if (FsHelpers::hasEpubExtension(book.path)) {
      handled = true;
      Epub epub(book.path, "/.crosspoint");
      epub.load(false, true);  // metadata only
      showProgress();
      ok = epub.generateThumbBmp(thumbHeight);
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      handled = true;
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        showProgress();
        ok = xtc.generateThumbBmp(thumbHeight);
      }
    }
    // Mark undecodable/absent covers so we don't retry them every visit (matches
    // the home screen); TXT and other coverless types just render a placeholder.
    if (handled && !ok) {
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
      book.coverBmpPath = "";
    }
  }
}

void RecentBooksActivity::renderCard(int index, int rowY, int rowH, bool selected) {
  const RecentBook& book = recentBooks[index];
  const int pageWidth = renderer.getScreenWidth();

  // Selection highlight via the active theme (grey/black, rounded/square + invert).
  const int hlX = CARD_SIDE / 2;
  const int hlW = pageWidth - CARD_SIDE;
  const bool invert = GUI.drawListRowSelection(renderer, Rect{hlX, rowY, hlW, rowH}, selected);

  int tw, th;
  thumbSize(tw, th);
  const int thumbX = CARD_SIDE;
  const int thumbY = rowY + CARD_PAD;

  bool coverDrawn = false;
  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, th);
    HalFile file;
    if (Storage.openFileForRead("RBA", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bitmap, thumbX, thumbY, tw, th);
        renderer.drawRect(thumbX, thumbY, tw, th, !invert);
        coverDrawn = true;
      }
    }
  }
  if (!coverDrawn) {
    renderer.drawRect(thumbX, thumbY, tw, th, !invert);  // placeholder box (TXT / no cover)
  }

  const int textX = thumbX + tw + CARD_TEXT_GAP;
  const int textW = pageWidth - textX - CARD_SIDE;
  const int metaLineH = renderer.getLineHeight(UI_10_FONT_ID) + CARD_LINE_GAP;
  int lineTop = rowY + CARD_PAD;

  // Title (bold).
  auto title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, lineTop, title.c_str(), !invert, EpdFontFamily::BOLD);
  lineTop += renderer.getLineHeight(UI_12_FONT_ID) + CARD_LINE_GAP;

  // Author (advance even if empty so the facts line stays on the same row).
  if (!book.author.empty()) {
    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, textX, lineTop, author.c_str(), !invert);
  }
  lineTop += metaLineH;

  // Facts: format + file size.
  std::string facts = formatFromPath(book.path);
  const std::string size = (index < static_cast<int>(fileSizes.size())) ? formatSize(fileSizes[index]) : std::string{};
  if (!size.empty()) {
    if (!facts.empty()) facts += " \xE2\x80\xA2 ";  // bullet
    facts += size;
  }
  if (!facts.empty()) {
    auto line = renderer.truncatedText(UI_10_FONT_ID, facts.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, textX, lineTop, line.c_str(), !invert);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data (also stats file sizes for the facts line)
  loadRecentBooks();

  // Generate any missing cover thumbnails up front (bounded: MAX_RECENT_BOOKS,
  // local files, no WiFi) so the cards render straight from cache.
  int tw, th;
  thumbSize(tw, th);
  ensureCovers(th);

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  fileSizes.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = itemsPerPage();

  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int top = contentTop();

  // Cover cards
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    const int pageItems = itemsPerPage();
    const int rowH = rowHeight();
    const int pageStart = static_cast<int>(selectorIndex) / pageItems * pageItems;
    for (int i = pageStart; i < static_cast<int>(recentBooks.size()) && i < pageStart + pageItems; i++) {
      renderCard(i, top + (i % pageItems) * rowH, rowH, i == static_cast<int>(selectorIndex));
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
