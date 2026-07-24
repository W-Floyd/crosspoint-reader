# Dictionary

Look up words while reading an EPUB using an offline StarDict dictionary stored on the SD card.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder must contain:

- `.idx` — word index (required, **must be uncompressed** — a `.idx.gz` will not work; decompress it on your computer with `gzip -d` first)
- `.dict` or `.dict.dz` — definition data. A plain `.dict` is read directly and is the most robust choice; if you have only a compressed `.dict.dz`, you can decompress it on your computer (`gzip -dc x.dict.dz > x.dict`) and drop the `.dict` in next to the `.idx`. A `.dict.dz` also works on its own: on first load the reader decompresses it once to a `.ddec` sidecar (with a progress bar) and reads from that afterwards, so lookups never inflate on the fly. (Inflating a definition on the fly needs a ~32 KB contiguous buffer that can be unavailable once a long reading session has fragmented the heap — which would make lookups quietly stop finding words until a restart; the `.ddec` sidecar avoids that.)
- `.syn` — synonym index (optional; maps alternate spellings and irregular forms to their headword)
- `.ifo` — metadata (optional)

Not supported: dictionaries with 64-bit index offsets (`idxoffsetbits=64` in the `.ifo` — rare, and rejected with an error), and HTML-formatted definitions render as raw markup rather than styled text.

## Setting Up a Dictionary

1. Copy your dictionary folder(s) to `/dictionaries/` on the SD card — one dictionary per folder, e.g. `/dictionaries/webster/webster.idx` + `webster.dict.dz`. A hidden `/.dictionaries/` folder (dot-prefixed) works the same way, for keeping it out of the file browser.
2. Open **Settings → Reader → Dictionary** on the device.
3. Select a dictionary from the list, or **None** to disable lookups.

The Dictionary setting only appears when at least one usable dictionary folder exists. Folders containing more than one dictionary (multiple `.idx` stems) are skipped as ambiguous.

## Looking Up a Word

Two ways to start a lookup while reading:

- Open the reader menu (**Confirm**) and choose **Look Up**.
- Or set **Settings → Controls → Long-press Menu** to "Dictionary", then hold **Confirm** (~0.4s) on the reading page.

One word on the page becomes highlighted:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

On the very first lookup with a dictionary (and again whenever a source file changes), the reader shows *"Indexing dictionary…"* while it builds small sidecar files next to them — a `.qidx` for the word index, a `.sidx` when a `.syn` synonym file is present, and, for a `.dict.dz` with no plain `.dict`, a fully decompressed `.ddec`. The `.qidx`/`.sidx` build in a few seconds; the `.ddec` decompression is longer (tens of seconds for a large dictionary) and shows a real progress bar. Each sidecar is rebuilt independently, only when its own source changes. All sidecars can be deleted safely at any time — they will simply be rebuilt. (The `.ddec` is roughly the uncompressed dictionary size, so it uses more SD space than the `.dict.dz` alone; if space is tight, ship a plain `.dict` instead and no `.ddec` is created.)

### How Lookup Works

1. **Direct match** — the word is found as-is (case-insensitive) in the dictionary index. Surrounding punctuation is ignored.
2. **Synonyms** — on a miss, if the dictionary ships a `.syn` file, alternate spellings and irregular forms recorded there are resolved to their headword (e.g. `oxen` → `ox`, `colour` → `color`). This step is skipped if the `.sidx` sidecar could not be built (e.g. transient low memory during indexing); the dictionary otherwise stays usable, and the build is retried the next time it is opened.
3. **Stemming** — still no match: common English word forms are retried automatically: possessives and plurals (`dogs` → `dog`, `stories` → `story`) and verb endings (`walked` → `walk`, `running` → `run`, `making` → `make`).
4. **Not found** — a short popup appears and you return to word selection.

## The Definition Screen

When a word is found, the definition screen shows the matched headword at the top and the definition text below, with a page counter for long definitions.

- **Left/Right** or side **Up/Down** — previous / next page
- **Back** — return to word selection
