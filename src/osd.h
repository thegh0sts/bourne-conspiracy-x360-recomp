// tbc - short on-screen messages, drawn by the native renderer (osd.cpp).
#pragma once

#include <string>

namespace tbc::osd {
// Shows "title" over "text" low in the middle of the screen for 1.5 s, replacing any message still showing.
// Native renderer only (native-plugin/patches/0013-osd-message.patch); with another renderer nothing shows.
void Show(const std::string& title, const std::string& text);
}  // namespace tbc::osd
