#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    DOWNLOADING,
    BULK_DOWNLOADING,  // Scanning the feed, then downloading every not-yet-present book
    BULK_DONE,         // Post-bulk summary screen (dismiss with any key)
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  // --- Bulk "download all in this feed" (long-press Confirm) ---
  bool bulkLongPressFired = false;  // Hold fired; swallow the release so it doesn't also single-download
  bool bulkCancel = false;          // Set from the progress callback (Back); passed as downloadToFile cancelFlag
  int bulkTotalCount = 0;           // Not-yet-present books found by the scan pass (0 while scanning)
  int bulkCurrentIndex = 0;         // 1-based index of the book currently downloading
  int bulkOkCount = 0;              // Books downloaded OK this run
  int bulkFailCount = 0;            // Books that failed (non-cancel) this run
  std::string bulkSummary;          // Message shown on the BULK_DONE screen

  // Per-entry cover-fetch state (parallel to `entries`), used only when cover
  // thumbnails are enabled. Reset whenever the entry list changes.
  enum class CoverState : uint8_t {
    Unknown,  // Not yet attempted this page visit
    Ready,    // Decoded BMP is cached and renderable
    None,     // No cover / undecodable — draw placeholder, do not retry
    Skipped   // Deferred (low heap / network) — placeholder, retry on next page visit
  };
  bool coversEnabled = false;
  std::vector<CoverState> coverStates;
  // Per-entry "already on the SD card" flag (parallel to `entries`), computed once
  // per page from the deterministic download path. Books present locally get a
  // downloaded checkmark (a cover-corner chip in rich mode, a gutter check in the
  // text list).
  std::vector<uint8_t> downloadedFlags;
  // Timestamp of the last user interaction while browsing. Cover fetch/decode
  // (which can block on network/SD) is deferred until input has been idle for a
  // short window, so scrolling stays responsive and covers fill in once you pause.
  unsigned long lastInteractionMs = 0;
  // Bounded auto-retry for covers that were skipped transiently (low heap right
  // after a feed fetch, or a flaky download), so they appear without the user
  // having to leave and re-enter the page.
  unsigned long nextCoverRetryMs = 0;
  int coverRetryRounds = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  // Fetch+parse a single feed page (no UI side effects, does not touch `entries`).
  // Returns false on fetch/parse error. outNextPageUrl is the absolute next-page
  // URL, or empty on the last page.
  bool fetchFeedPage(const std::string& pageUrl, std::vector<OpdsEntry>& outEntries, std::string& outNextPageUrl);
  void promptBulkDownload();  // Scan the feed's pages for missing books, then confirm the count
  void runBulkDownload();     // Download every not-yet-present book across the feed's pages, with cancel
  std::string localFilename(const OpdsEntry& book) const;         // SD path the downloader writes to
  void drawCheck(int x, int y, int size, bool state);             // Bare checkmark (two strokes) within a size box
  void drawDownloadedBadge(int x, int y, int size, bool invert);  // Chip + check for the cover corner
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }

  // --- Rich (cover thumbnail) browsing ---
  int itemsPerPage() const;                                    // Rows per page for the active layout
  int rowHeight() const;                                       // Rich-row height derived from font metrics
  void thumbSize(int& outW, int& outH) const;                  // Row-thumbnail pixel size
  std::string resolveCoverUrl(const std::string& href) const;  // Absolute cover URL vs feed base
  void loadNextCover();  // Fetch/decode one pending cover per loop: visible first, then prefetch off-screen
  // Decode the first still-Unknown cover in [start,end). Returns true if it did a
  // fetch/decode (caller stops for this loop). visibleRange gates the repaint so
  // prefetched off-screen covers don't trigger a needless full re-render.
  bool tryDecodeFirstUnknownCover(int start, int end, bool visibleRange);
  void renderRichRow(int entryIndex, int rowY, int rowHeight, bool selected);
  void renderTextList();
  void renderRichList();
  bool hintLabelsSame(int a, int b) const;  // Do the two selections yield identical button hints?

  // Fast-path repaint state: true when the framebuffer currently holds a fully
  // painted rich list, so a same-page selection move can repaint only the outline.
  bool richListPainted = false;
  int paintedPageStart = -1;
  int paintedSelector = -1;
};
