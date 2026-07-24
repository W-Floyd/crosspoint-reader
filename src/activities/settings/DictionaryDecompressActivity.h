#pragma once

#include <I18n.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/Dictionary.h"

// Settings action: decompress the currently-selected StarDict .dict.dz to a
// plain <stem>.ddec sidecar so lookups read definitions directly (no per-entry
// 32KB inflate window, which fails under heap fragmentation). Handles the cases
// where there is no dictionary, it is already a plain .dict, or a valid .ddec
// already exists — then just reports status. The decompress is slow, so it is
// only ever run from here, never from a lookup.
class DictionaryDecompressActivity final : public Activity {
 public:
  explicit DictionaryDecompressActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionaryDecompress", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // don't sleep mid-decompress
  void render(RenderLock&&) override;

  // Progress bar repaint during the decompress; called via a C trampoline from
  // buildDecompressedSidecar. Public for the trampoline; self-throttles.
  void onProgress(uint32_t done, uint32_t total);

 private:
  enum class State : uint8_t { Prompt, Working, Done, Failed, Info };

  void beginDecompress();
  void goBack() { finish(); }

  Dictionary dict;
  State state = State::Info;
  StrId infoMsg = StrId::STR_DICT_NO_DICT_SET;  // shown in the Info state
  OptionPopup confirmPopup;
  unsigned long lastProgressDrawMs = 0;
};
