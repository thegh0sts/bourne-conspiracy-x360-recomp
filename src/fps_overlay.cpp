// tbc - FPS counter in its own small window over the game.
//
// The SDK's ImGui overlays make its presenter repaint and present the same frame again
// on every display refresh while any dialog is open. Skullburglar measured that at about
// 20% of the frame rate with an FPS counter on (its SDK patch 0030). This counter is a
// separate Win32 window instead: a tiny, click-through, always-on-top popup drawn with
// GDI and moved to follow the game window, so the game's own rendering is untouched.
// The Table Tennis port keeps its frame statistics in a separate native window for the
// same reason.
//
// The window is shown only while the game window has focus. It follows the game in
// windowed and borderless fullscreen; with exclusive fullscreen it may not be visible.

#include "fps_overlay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <atomic>
#include <cstdio>
#include <thread>

#include "frame_pacing.h"

namespace tbc::overlay {
namespace {

constexpr wchar_t kClassName[] = L"TbcFpsOverlay";
constexpr int kWidth = 92;
constexpr int kHeight = 30;
constexpr int kMargin = 10;
constexpr UINT_PTR kTimerId = 1;

std::thread g_thread;
std::atomic<HWND> g_window{nullptr};
HFONT g_font = nullptr;

// The game's main window: the largest visible top-level window of this process.
struct FindGame {
  HWND best = nullptr;
  long long area = 0;
  HWND self = nullptr;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
  auto* f = reinterpret_cast<FindGame*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || hwnd == f->self || !IsWindowVisible(hwnd)) return TRUE;
  RECT r;
  if (!GetWindowRect(hwnd, &r)) return TRUE;
  const long long a = (long long)(r.right - r.left) * (r.bottom - r.top);
  if (a > f->area) {
    f->area = a;
    f->best = hwnd;
  }
  return TRUE;
}

void Update(HWND self) {
  FindGame f;
  f.self = self;
  EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&f));
  if (!f.best || GetForegroundWindow() != f.best || IsIconic(f.best)) {
    ShowWindow(self, SW_HIDE);
    return;
  }
  POINT origin{0, 0};
  ClientToScreen(f.best, &origin);
  SetWindowPos(self, HWND_TOPMOST, origin.x + kMargin, origin.y + kMargin, kWidth, kHeight,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(self, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      Update(hwnd);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT r;
      GetClientRect(hwnd, &r);
      HBRUSH bg = CreateSolidBrush(RGB(16, 18, 22));
      FillRect(dc, &r, bg);
      DeleteObject(bg);
      wchar_t text[32];
      const double fps = tbc::pacing::CurrentFps();
      if (fps > 0) {
        swprintf(text, 32, L"%.0f FPS", fps);
      } else {
        swprintf(text, 32, L"-- FPS");
      }
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, fps >= 55 ? RGB(120, 220, 120) : fps >= 28 ? RGB(235, 200, 90) : RGB(235, 90, 90));
      HGDIOBJ old = SelectObject(dc, g_font);
      DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kTimerId);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void ThreadMain() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  g_font = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  // Layered + transparent: click-through and slightly see-through. Tool window: no taskbar
  // button. No-activate: never takes focus from the game.
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_NOACTIVATE,
                              kClassName, L"FPS", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
                              inst, nullptr);
  if (!hwnd) return;
  SetLayeredWindowAttributes(hwnd, 0, 210, LWA_ALPHA);
  g_window = hwnd;
  SetTimer(hwnd, kTimerId, 250, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g_window = nullptr;
  if (g_font) DeleteObject(g_font);
  g_font = nullptr;
}

}  // namespace

void StartFpsOverlay() {
  if (g_thread.joinable()) return;
  g_thread = std::thread(ThreadMain);
}

void StopFpsOverlay() {
  if (!g_thread.joinable()) return;
  // The window may not exist yet if the thread has only just started.
  for (int i = 0; i < 50 && !g_window.load(); ++i) Sleep(10);
  if (HWND hwnd = g_window.load()) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    g_thread.join();
  } else {
    g_thread.detach();  // the window never appeared; don't block shutdown on it
  }
}

}  // namespace tbc::overlay
