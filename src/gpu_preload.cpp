// tbc - load the graphics plugin before the command line is read.
//
// The runtime reads command-line settings (and tbc.toml) at startup, but only loads the
// graphics plugin (rexgpu-xenos*.dll) later. Settings that belong to the plugin, such as
// --resolution_scale, --vsync and --swap_post_effect, weren't registered yet at that
// point and were dropped, so the game always rendered at 1280x720. Loading the plugin
// here, during static initialization (before main), registers its settings first.
// The runtime's own load later finds the DLL already loaded and reuses it.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace {

HMODULE PreloadGpuPlugin() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return nullptr;
  }
  std::wstring dir(buf, n);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  // Build-type suffixes: rd = RelWithDebInfo, d = Debug, none = Release.
  for (const wchar_t* name : {L"rexgpu-xenosrd.dll", L"rexgpu-xenos.dll", L"rexgpu-xenosd.dll"}) {
    std::wstring path = dir + name;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return LoadLibraryW(path.c_str());
    }
  }
  return nullptr;
}

const HMODULE g_gpu_plugin = PreloadGpuPlugin();

}  // namespace
#endif  // _WIN32
