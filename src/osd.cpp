// tbc - short on-screen messages, drawn by the native renderer.
//
// The renderer is a plugin (rexgpu-native) and draws in its present pass; it finds this
// executable's export tbc_osd_message (GetProcAddress on the main module) and calls it once a
// frame. A new message (its serial number goes up) shows for 1.5 s in the achievement pop-up's
// style, grey, low in the middle (native-plugin/patches/0013-osd-message.patch). A renderer
// without that patch never asks, so nothing shows.

#include "osd.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_mutex;
std::string g_message;  // "title\ntext"
uint64_t g_serial = 0;
}  // namespace

namespace tbc::osd {
void Show(const std::string& title, const std::string& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_message = title + "\n" + text;
  ++g_serial;
}
}  // namespace tbc::osd

#if defined(_WIN32)
#define TBC_EXPORT extern "C" __declspec(dllexport)
#else
#define TBC_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Copies the latest message (0-terminated, cut to fit) into text and its serial number into serial.
TBC_EXPORT int tbc_osd_message(char* text, int size, uint64_t* serial) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (serial) *serial = g_serial;
  if (!text || size <= 0) return 0;
  const size_t n = std::min(g_message.size(), size_t(size - 1));
  std::memcpy(text, g_message.data(), n);
  text[n] = '\0';
  return int(n);
}
