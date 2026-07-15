#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
constexpr size_t MAX_SERIES_CHARS = 120;
constexpr size_t MAX_CATEGORY_CHARS = 48;
constexpr size_t MAX_PUBLISHED_CHARS = 24;  // ISO 8601 date; only the year is shown
constexpr size_t MAX_MEDIA_TYPE_CHARS = 64;
}  // namespace

OpdsParser::OpdsParser(size_t maxEntries) : maxEntries(maxEntries) {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  // +2 leaves room for the browser's injected prev/next navigation rows.
  entries.reserve(maxEntries + 2);
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  inPublished = inSeries = inSeriesName = false;
  currentThumbIsThumbnail = false;
  collectCurrentEntry = false;
  feedTruncated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::assignBounded(std::string& target, const char* value, const size_t maxLen) {
  if (!value) {
    target.clear();
    return;
  }
  target.assign(value, strnlen(value, maxLen));
}

void OpdsParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->collectCurrentEntry = self->entries.size() < self->maxEntries;
    self->feedTruncated = self->feedTruncated || !self->collectCurrentEntry;
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    self->inPublished = self->inSeries = self->inSeriesName = false;
    self->currentThumbIsThumbnail = false;
    return;
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      }

      if (self->inEntry && self->collectCurrentEntry) {
        // Cover art: prefer an explicit thumbnail rel; otherwise accept a full-size
        // image. Stored verbatim (query string intact) so any &preset= survives.
        const bool isThumbnailRel = rel && strstr(rel, "opds-spec.org/image/thumbnail") != nullptr;
        const bool isImageRel = rel && strstr(rel, "opds-spec.org/image") != nullptr;
        if (isThumbnailRel) {
          assignBounded(self->currentEntry.thumbnailUrl, href, MAX_HREF_CHARS);
          self->currentThumbIsThumbnail = true;
        } else if (isImageRel && !self->currentThumbIsThumbnail) {
          assignBounded(self->currentEntry.thumbnailUrl, href, MAX_HREF_CHARS);
        }

        if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
            strcmp(type, "application/epub+zip") == 0) {
          // Prefer plain EPUB links over derived formats when multiple
          // acquisition links are present for one entry.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
            assignBounded(self->currentEntry.mediaType, type, MAX_MEDIA_TYPE_CHARS);
            const char* length = findAttribute(atts, "length");
            self->currentEntry.fileSizeBytes = length ? strtoull(length, nullptr, 10) : 0;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
          }
        }
      }
    }
  }

  if (!self->inEntry || !self->collectCurrentEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "published") == 0 || strstr(name, ":published") != nullptr || strcmp(name, "issued") == 0 ||
             strstr(name, ":issued") != nullptr) {
    // Atom <published> or dcterms:issued — publication date (year is shown).
    self->inPublished = true;
    self->currentText.clear();
  } else if (strcmp(name, "category") == 0 || strstr(name, ":category") != nullptr) {
    // Atom <category term=".." label=".."/> — keep the first one's label (or term).
    if (self->currentEntry.category.empty()) {
      const char* label = findAttribute(atts, "label");
      if (!label) label = findAttribute(atts, "term");
      if (label) assignBounded(self->currentEntry.category, label, MAX_CATEGORY_CHARS);
    }
  } else if (strcmp(name, "Series") == 0 || strstr(name, ":Series") != nullptr) {
    // schema:Series is usually an empty element carrying the name as an attribute
    // (schema:name / name); some feeds put it in the element text instead.
    self->inSeries = true;
    const char* seriesName = findAttribute(atts, "schema:name");
    if (!seriesName) seriesName = findAttribute(atts, "name");
    if (seriesName) assignBounded(self->currentEntry.series, seriesName, MAX_SERIES_CHARS);
    self->currentText.clear();
  } else if (self->inSeries && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inSeriesName = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (self->collectCurrentEntry && !self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
    self->collectCurrentEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inSeriesName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      if (self->currentEntry.series.empty()) self->currentEntry.series = self->currentText;
      self->inSeriesName = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    } else if (self->inPublished && (strcmp(name, "published") == 0 || strstr(name, ":published") != nullptr ||
                                     strcmp(name, "issued") == 0 || strstr(name, ":issued") != nullptr)) {
      if (self->currentEntry.published.empty()) self->currentEntry.published = self->currentText;
      self->inPublished = false;
    } else if (strcmp(name, "Series") == 0 || strstr(name, ":Series") != nullptr) {
      if (self->currentEntry.series.empty() && !self->currentText.empty())
        self->currentEntry.series = self->currentText;
      self->inSeries = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (!self->collectCurrentEntry) return;
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inSeriesName) {
    appendBounded(self->currentText, s, len, MAX_SERIES_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  } else if (self->inPublished) {
    appendBounded(self->currentText, s, len, MAX_PUBLISHED_CHARS);
  }
}
