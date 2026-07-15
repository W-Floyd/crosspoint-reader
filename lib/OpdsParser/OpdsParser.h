#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 *
 * The richer metadata fields (thumbnail, series, summary, format, size) are all
 * best-effort and bounded at parse time — a whole feed page of these structs
 * lives in RAM at once, so every string is length-capped in the parser.
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  std::string thumbnailUrl;    // Cover/thumbnail link (thumbnail preferred over full image),
                               // stored verbatim (query string intact, e.g. &preset=...),
                               // resolved to an absolute URL by the consumer.
  std::string series;          // Series/collection name, if advertised
  std::string category;        // First category/genre label, if advertised
  std::string published;       // Publication date (ISO 8601), if advertised
  std::string mediaType;       // Acquisition link MIME type (e.g. application/epub+zip)
  uint64_t fileSizeBytes = 0;  // Acquisition link `length`, 0 if unknown
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  // Default cap on collected entries per feed page (excludes the injected
  // prev/next navigation rows). The parsed entries are moved into the browser and
  // held for the whole browse session, so this bounds fixed browse-time RAM
  // (~272 bytes/entry). Callers that need headroom for other work on the same
  // screen (e.g. decoding cover thumbnails with WiFi up) may pass a smaller cap.
  static constexpr size_t DEFAULT_MAX_ENTRIES = 62;

  explicit OpdsParser(size_t maxEntries = DEFAULT_MAX_ENTRIES);
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);
  static void assignBounded(std::string& target, const char* value, size_t maxLen);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  XML_Parser parser = nullptr;
  size_t maxEntries;  // Cap on collected entries (excludes injected prev/next nav rows)
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
  bool inPublished = false;
  bool inSeries = false;
  bool inSeriesName = false;
  bool currentThumbIsThumbnail = false;  // Prefer an explicit thumbnail link over a full-size image
  bool collectCurrentEntry = false;

  bool errorOccured = false;
  bool feedTruncated = false;
};
