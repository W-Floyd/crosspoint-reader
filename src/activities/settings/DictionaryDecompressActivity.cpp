#include "DictionaryDecompressActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// C trampoline: Dictionary::buildDecompressedSidecar reports progress here.
void decompressProgress(void* ctx, uint32_t done, uint32_t total) {
  static_cast<DictionaryDecompressActivity*>(ctx)->onProgress(done, total);
}
}  // namespace

void DictionaryDecompressActivity::onEnter() {
  Activity::onEnter();

  const char* name = SETTINGS.dictionaryName;
  if (!name || name[0] == '\0' || !dict.open(name)) {
    state = State::Info;
    infoMsg = StrId::STR_DICT_NO_DICT_SET;
  } else if (!dict.usesCompressedDict()) {
    state = State::Info;
    infoMsg = StrId::STR_DICT_DECOMPRESS_PLAIN;  // already a plain .dict
  } else if (dict.hasDecompressedSidecar()) {
    state = State::Info;
    infoMsg = StrId::STR_DICT_DECOMPRESS_ALREADY;  // valid .ddec already present
  } else {
    state = State::Prompt;
    const char* options[] = {tr(STR_CANCEL), tr(STR_DICT_DECOMPRESS)};
    confirmPopup.show(tr(STR_DICT_DECOMPRESS_PROMPT), options, 2, 0, [this](int idx) {
      if (idx == 1) {
        beginDecompress();
      } else {
        goBack();
      }
    });
  }
  requestUpdate();
}

void DictionaryDecompressActivity::onProgress(uint32_t done, uint32_t total) {
  vTaskDelay(1);  // feed the watchdog during the long decompress
  const unsigned long now = millis();
  if (done < total && now - lastProgressDrawMs < 700) return;  // throttle e-ink refreshes
  lastProgressDrawMs = now;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, tr(STR_DICT_DECOMPRESS));
  renderer.drawCenteredText(UI_10_FONT_ID, h / 2 - 30, tr(STR_DICT_DECOMPRESSING));
  GUI.drawProgressBar(
      renderer, Rect{metrics.contentSidePadding, h / 2, w - metrics.contentSidePadding * 2, metrics.progressBarHeight},
      done, total);
  renderer.displayBuffer();
}

void DictionaryDecompressActivity::beginDecompress() {
  {
    RenderLock lock(*this);
    state = State::Working;
  }
  requestUpdateAndWait();  // paint the initial "Decompressing…" before blocking
  const bool ok = dict.buildDecompressedSidecar(&decompressProgress, this);
  state = ok ? State::Done : State::Failed;
  requestUpdate();
}

void DictionaryDecompressActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, tr(STR_DICT_DECOMPRESS));

  switch (state) {
    case State::Prompt:
      renderer.drawCenteredText(UI_10_FONT_ID, h / 2 - 20, tr(STR_DICT_DECOMPRESS_PROMPT));
      if (confirmPopup.processRender(renderer, mappedInput)) return;
      {
        const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DICT_DECOMPRESS), "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      break;
    case State::Working:
      renderer.drawCenteredText(UI_10_FONT_ID, h / 2, tr(STR_DICT_DECOMPRESSING));
      break;
    case State::Done: {
      renderer.drawCenteredText(UI_10_FONT_ID, h / 2, tr(STR_DICT_DECOMPRESS_DONE), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::Failed: {
      renderer.drawCenteredText(UI_10_FONT_ID, h / 2, tr(STR_DICT_DECOMPRESS_FAILED), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::Info: {
      renderer.drawCenteredText(UI_10_FONT_ID, h / 2, I18N.get(infoMsg));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }
  renderer.displayBuffer();
}

void DictionaryDecompressActivity::loop() {
  if (state == State::Prompt) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) goBack();
    return;
  }
  if (state == State::Done || state == State::Failed || state == State::Info) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) goBack();
  }
}
