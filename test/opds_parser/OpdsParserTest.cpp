#include <gtest/gtest.h>

#include <string>

#include "OpdsParser.h"

namespace {

// Feed an OPDS document into the parser (OpdsParser is non-copyable/non-movable,
// so the caller owns it). When chunk > 0 the body is written in chunk-sized
// slices to exercise the streaming Print::write path (Expat must not depend on
// element boundaries aligning with buffer boundaries).
void parseFeed(OpdsParser& parser, const std::string& xml, size_t chunk = 0) {
  if (chunk == 0) {
    parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  } else {
    for (size_t off = 0; off < xml.size(); off += chunk) {
      const size_t n = std::min(chunk, xml.size() - off);
      parser.write(reinterpret_cast<const uint8_t*>(xml.data() + off), n);
    }
  }
  parser.flush();
}

const OpdsEntry* findByTitle(const OpdsParser& parser, const std::string& title) {
  for (const auto& e : parser.getEntries()) {
    if (e.title == title) return &e;
  }
  return nullptr;
}

// A representative OPDS 1.2 acquisition feed with rich per-entry metadata.
const char* kRichFeed = R"FEED(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
      xmlns:dc="http://purl.org/dc/terms/"
      xmlns:schema="http://schema.org/"
      xmlns:opds="http://opds-spec.org/2010/catalog">
  <title>Library</title>
  <entry>
    <title>The Great Book</title>
    <id>urn:uuid:book-1</id>
    <author><name>Jane Author</name></author>
    <summary>A sweeping tale of adventure and memory.</summary>
    <schema:Series schema:name="The Great Saga" schema:position="2"/>
    <link rel="http://opds-spec.org/image/thumbnail"
          href="/api/v1/opds/9/cover?ts=1&amp;preset=X3" type="image/jpeg"/>
    <link rel="http://opds-spec.org/image"
          href="/api/v1/opds/9/cover-full.jpg" type="image/jpeg"/>
    <link rel="http://opds-spec.org/acquisition"
          href="/download/9.epub" type="application/epub+zip" length="1048576"/>
  </entry>
  <entry>
    <title>Image Only</title>
    <id>urn:uuid:book-2</id>
    <author><name>Bob Writer</name></author>
    <content type="text">Content used when no summary is present.</content>
    <link rel="http://opds-spec.org/image"
          href="http://example.com/full.jpg" type="image/jpeg"/>
    <link rel="http://opds-spec.org/acquisition/open-access"
          href="/download/2.epub" type="application/epub+zip"/>
  </entry>
  <entry>
    <title>A Category</title>
    <id>urn:uuid:nav-1</id>
    <link rel="subsection" href="/catalog/sci-fi" type="application/atom+xml;profile=opds-catalog"/>
  </entry>
</feed>
)FEED";

TEST(OpdsParserRich, PrefersThumbnailOverImage) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "The Great Book");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->type, OpdsEntryType::BOOK);
  // Thumbnail rel wins over the full-size image rel.
  EXPECT_EQ(book->thumbnailUrl, "/api/v1/opds/9/cover?ts=1&preset=X3");
}

TEST(OpdsParserRich, PreservesCoverQueryStringVerbatim) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "The Great Book");
  ASSERT_NE(book, nullptr);
  // The &preset= parameter must survive so preset-scoped grayscale covers work.
  EXPECT_NE(book->thumbnailUrl.find("preset=X3"), std::string::npos);
}

TEST(OpdsParserRich, FallsBackToImageWhenNoThumbnail) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "Image Only");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->thumbnailUrl, "http://example.com/full.jpg");
}

TEST(OpdsParserRich, ExtractsSummary) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "The Great Book");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->summary, "A sweeping tale of adventure and memory.");
}

TEST(OpdsParserRich, FallsBackToContentForSummary) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "Image Only");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->summary, "Content used when no summary is present.");
}

TEST(OpdsParserRich, ExtractsSeriesFromAttribute) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "The Great Book");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->series, "The Great Saga");
}

TEST(OpdsParserRich, ExtractsAcquisitionLengthAndType) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* book = findByTitle(parser, "The Great Book");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->mediaType, "application/epub+zip");
  EXPECT_EQ(book->fileSizeBytes, 1048576u);
}

TEST(OpdsParserRich, NavigationEntryHasNoBookMetadata) {
  OpdsParser parser;
  parseFeed(parser, kRichFeed);
  const OpdsEntry* nav = findByTitle(parser, "A Category");
  ASSERT_NE(nav, nullptr);
  EXPECT_EQ(nav->type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(nav->href, "/catalog/sci-fi");
  EXPECT_EQ(nav->fileSizeBytes, 0u);
}

TEST(OpdsParserRich, StreamingChunkedWriteMatchesWhole) {
  OpdsParser whole;
  parseFeed(whole, kRichFeed);
  // Small odd chunk size splits tags/attributes across buffer boundaries.
  OpdsParser chunked;
  parseFeed(chunked, kRichFeed, 7);
  ASSERT_EQ(whole.getEntries().size(), chunked.getEntries().size());
  const OpdsEntry* a = findByTitle(whole, "The Great Book");
  const OpdsEntry* b = findByTitle(chunked, "The Great Book");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->thumbnailUrl, b->thumbnailUrl);
  EXPECT_EQ(a->summary, b->summary);
  EXPECT_EQ(a->series, b->series);
  EXPECT_EQ(a->fileSizeBytes, b->fileSizeBytes);
}

TEST(OpdsParserRich, SeriesFromElementText) {
  const char* feed = R"FEED(<?xml version="1.0"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:schema="http://schema.org/">
  <entry>
    <title>Text Series Book</title>
    <id>x</id>
    <schema:Series><schema:name>Textual Series</schema:name></schema:Series>
    <link rel="http://opds-spec.org/acquisition" href="/b.epub" type="application/epub+zip"/>
  </entry>
</feed>
)FEED";
  OpdsParser parser;
  parseFeed(parser, feed);
  const OpdsEntry* book = findByTitle(parser, "Text Series Book");
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->series, "Textual Series");
}

TEST(OpdsParserRich, SummaryTruncatedToBound) {
  std::string longText(500, 'x');
  std::string feed =
      "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><entry>"
      "<title>Long</title><id>x</id><summary>" +
      longText +
      "</summary>"
      "<link rel=\"http://opds-spec.org/acquisition\" href=\"/b.epub\" type=\"application/epub+zip\"/>"
      "</entry></feed>";
  OpdsParser parser;
  parseFeed(parser, feed);
  const OpdsEntry* book = findByTitle(parser, "Long");
  ASSERT_NE(book, nullptr);
  EXPECT_LE(book->summary.size(), 96u);
  EXPECT_GT(book->summary.size(), 0u);
}

}  // namespace
