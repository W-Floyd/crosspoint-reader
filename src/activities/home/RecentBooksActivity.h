#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  // File size in bytes per book (parallel to recentBooks), stat'd once on entry
  // for the card facts line so scrolling doesn't re-stat the SD.
  std::vector<uint64_t> fileSizes;

  // Data loading
  void loadRecentBooks();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

  // Cover-card layout (all derived from font metrics + theme metrics).
  int rowHeight() const;
  int contentTop() const;
  int itemsPerPage() const;
  void thumbSize(int& outW, int& outH) const;
  // Generate any missing cover thumbnails at the card size (idempotent; shows a
  // loading popup like the home screen). No network here, so heap is ample.
  void ensureCovers(int thumbHeight);
  void renderCard(int index, int rowY, int rowH, bool selected);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
