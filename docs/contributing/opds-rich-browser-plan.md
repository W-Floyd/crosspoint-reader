# Design Plan: Rich OPDS Browser (cover thumbnails + metadata)

## Context / motivation

The OPDS browser (`src/activities/browser/OpdsBookBrowserActivity.cpp`) is currently a
**text-only list** — each entry shows title (and, for books, author) on a 30 px row. The
parser throws away everything else the feed offers: cover/thumbnail links, summary, series,
categories, format, size.

Goal: render each catalog entry as a **row/card** — a cover thumbnail on the left and richer
metadata on the right (title, author, series, a short summary, format/size) — so browsing a
remote library feels like the home screen rather than a flat text menu.

This also makes device-friendly server covers pay off: a companion server change already
serves baseline, single-component grayscale covers via `?preset=` (see grimmory
`#1490`-related work), which is exactly the easiest input for the on-device JPEG decoder.

> Not a revival of the closed PR **#1891** ("OPDS cover image support"). That PR mixed in
> handling for bad/embedded EPUB images and a broader cover-file concept. This plan is
> narrowly scoped to the **browser UI + feed metadata**, reusing the cover decode/cache
> machinery that already exists.

## Current state (what we build on)

- **Browser** — `OpdsBookBrowserActivity`: `std::vector<OpdsEntry> entries`, `PAGE_ITEMS`
  rows at 30 px from y=60, selector via `fillRect`, `ButtonNavigator` for movement,
  pagination from the parser's `nextPageUrl`/`prevPageUrl`. WiFi + auth already wired
  (`OpdsServerStore`, `HttpDownloader`). Text-only `render()`.
- **Parser** — `lib/OpdsParser/OpdsParser` (Expat, streaming via `Print::write`): produces
  `OpdsEntry { type, title, author, href, id }`. Extracts only the
  `opds-spec.org/acquisition` link; ignores `image`/`image/thumbnail`, `summary`,
  `category`, series, dcterms.
- **Cover decode/cache already exists** (reuse, don't reinvent):
  - `lib/JpegToBmpConverter/JpegToBmpConverter.h`:
    `jpegFileToBmpStreamWithSize(HalFile& jpeg, Print& bmpOut, maxW, maxH)` (and a 1-bit
    variant) — decodes (JPEGDEC) + downscales a JPEG file to a grayscale BMP sink.
  - `src/network/HttpDownloader.{h,cpp}`: `downloadToFile(url, destPath, …, user, pass)`
    streaming download with auth; also chunked `fetchUrl`.
  - `lib/GfxRenderer` bitmap drawing + partial refresh; the home screen already caches BMP
    cover thumbnails (`HomeActivity::loadRecentCovers` → `Epub::generateThumbBmp` →
    `coverBmpPath`).
- **Constraints**: RAM-starved ESP32. `JpegToBmpConverter` needs ≈20 KB decoder + a
  ≈32 KB free-heap floor. **OOM is the dominant risk class in this repo** — the design must
  decode at most one cover at a time and gate on free heap.

## Design

### 1. Extend the feed model + parser

Add to `OpdsEntry` (keep it lean; this vector holds a whole page+):
- `thumbnailUrl` — from `<link rel="…/image/thumbnail">`, falling back to `…/image`;
  resolved to an absolute URL against the feed base (reuse existing href resolution).
  Keep the **query string intact** — it may already carry `&preset=…` (see §2).
- `series`, `summary` (truncated at parse time, e.g. ≤200 chars, to bound memory),
  `mediaType`/format, `fileSizeBytes` (from acquisition `<link length=…>`), optional
  `published`.

`OpdsParser` changes (stay streaming — never buffer the whole feed):
- In `startElement`, capture the thumbnail/image link hrefs and acquisition `length`/`type`.
- Capture `<summary>`/`<content>` text (truncated), `<category>`, and series
  (`schema:Series` / `belongs-to-collection` meta / dcterms) into the current entry.
- Be lenient across OPDS 1.2 (Atom) feeds and namespace/prefix variance.
- Preserve the `OpdsBook` alias for backward compatibility.

### 2. Cover fetch → decode → cache pipeline

- Cache dir `/.crosspoint/opds_covers/`, file keyed by a hash of the absolute cover URL plus
  the target thumb size, storing a **pre-rendered grayscale BMP** at row-thumb size
  (mirrors the home-cover thumb approach).
- Miss path: `HttpDownloader::downloadToFile(thumbnailUrl, tmp.jpg, …creds…)` →
  `JpegToBmpConverter::jpegFileToBmpStreamWithSize(tmp.jpg, cache.bmp, thumbW, thumbH)` →
  delete `tmp.jpg`. On decode failure, write a small **"no-cover" sentinel** so we don't
  refetch every visit.
- Credentials: reuse the `OpdsServer` the browser already authenticates the feed/download
  with.
- **Device-friendly covers come for free:** the feed already emits the preset on the cover
  link when browsing a preset-scoped catalog — e.g. grimmory returns
  `/api/v1/opds/9/cover?<ts>&preset=X3`. Because we fetch the advertised `thumbnailUrl`
  **verbatim** (query string intact), a preset-scoped browse automatically receives a
  baseline, single-component grayscale cover — the most decode-friendly input — with **no
  client-side preset logic**. Non-preset servers simply return their normal cover.

### 3. Rendering — row/card layout

- Replace 30 px text rows with taller rows sized to the thumbnail (e.g. thumb ≈ screen
  height / N; recompute `PAGE_ITEMS` from row height and screen size). Layout: thumbnail
  left (fixed W×H), metadata right — title (bold, truncated), author, series line, 1–2 line
  summary snippet, format/size footer. Keep the `fillRect` selection highlight around the
  active row.
- Draw the thumbnail from the cached BMP via `GfxRenderer`; when absent/pending, draw a
  placeholder box (or fall back to the text row). Keep pagination + `ButtonNavigator`;
  respect orientation.

### 4. Async / lazy load + memory safety (the crux)

- Fetch covers only for the **currently visible page**, **sequentially, one decode at a
  time** (never hold >1 decode buffer).
- Gate each decode on free heap / `ESP.getMaxAllocHeap()` ≥ threshold (reuse the
  `JpegToBmpConverter` floor); under pressure, **skip the cover (placeholder) rather than
  OOM**.
- Progressive render: draw the page text immediately, then **partial-refresh each thumbnail
  region** as its cover finishes — no blank wait, no full-screen flicker. Cache hits render
  instantly.
- **Cancel** in-flight cover work on page change / back / exit.
- Cache eviction: cap `opds_covers/` size (LRU by mtime) to bound SD usage.

### 5. Settings

- New toggle **"OPDS cover thumbnails"** (Controls/OPDS category), persisted. Off → the
  existing fast text list (that render path is retained as the fallback). Room later for
  "covers on Wi-Fi only" / size options.

### 6. Testing / verification

- **Native unit tests** (`test/`, CMake — the parser is pure and already unit-tested):
  fixtures with thumbnail/image links, summary, series, chunked `write()` → assert
  `OpdsEntry` fields, relative-URL resolution, summary truncation, streaming.
- Cache-key hashing + hit/miss logic unit-tested where separable from hardware.
- **On-device**: browse a grimmory catalog with covers; confirm thumbnails render, heap
  stays bounded (serial heap logs), page nav cancels fetches. Test both a
  device-friendly server (grimmory `?preset=` baseline grayscale) and one serving large /
  progressive / ICC covers (placeholder fallback path). Exercise low-heap paths with the
  heap-arena simulator (#2455) if available.

## Reused components

`lib/OpdsParser/*`, `lib/JpegToBmpConverter/JpegToBmpConverter.h`,
`src/network/HttpDownloader.*`, `lib/GfxRenderer` (bitmap + partial refresh),
`src/activities/browser/OpdsBookBrowserActivity.*`, `OpdsServerStore`, and the home-screen
cover-thumb caching pattern.

## Phasing (PR-sized steps)

1. **Parser + model** — thumbnail/image URL + metadata extraction, relative-URL resolution,
   summary truncation; native tests. No UI change.
2. **Cover pipeline** — fetch → BMP → cache with heap guards and sentinel, behind a hidden
   flag.
3. **Row/card rendering** — thumbnails + metadata; recompute `PAGE_ITEMS`.
4. **Async lazy load** — visible-only sequential fetch, partial-refresh per thumb, fetch
   cancellation, heap gating.
5. **Setting toggle + docs.**

(Preset-aware covers need no step — the feed already carries `&preset=` on the cover link,
and we fetch it verbatim.)

## Risks / open questions

- **OOM (dominant):** decode heap pressure; mitigated by one-at-a-time + heap gating +
  skip-on-low. Should land on top of the OOM-hardening PRs already in flight.
- **E-ink latency/flicker:** per-thumb partial refresh + persistent cache across sessions.
- **Network cost:** per-cover fetch; bounded by visible-only + cache (Wi-Fi already
  required to browse).
- **Cover format variance:** servers serve large / progressive / ICC covers → decode
  failures; placeholder fallback + optional preset param. (On-device decoder is JPEGDEC;
  see the `#2136`-class cover-decode fragility.)
- **Feed variance:** OPDS 1.2 vs 2.0, relative URLs, differing thumbnail rels — parser must
  be lenient.
- **Coordination:** overlaps in-flight OPDS PRs (#2561 download speed/XTC, #2546 author
  folders); rebase/coordinate.

## End-to-end verification

- Native: build + run the `test/` suite over parser fixtures.
- Firmware: `pio run -e default`, flash, browse an OPDS catalog with covers, watch serial
  heap logs while paging; confirm placeholder fallback on undecodable covers and that
  `?preset=` covers render cleanly.
