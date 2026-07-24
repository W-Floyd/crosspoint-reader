#pragma once
// No-op logging for the fuzz target: keep the crash signal clean (only ASan
// reports), and avoid pulling the firmware's Logging.cpp (esp_rom_sys /
// BoardConfig deps). Flip to fprintf if you want to watch a specific repro.
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
