// tbc - load the graphics plugin before the command line is read.
//
// The runtime reads command-line settings (and tbc.toml) at startup, but only loads the
// graphics plugin (rexgpu-<name>*.dll) later. Settings that belong to the plugin, such as
// --resolution_scale, --vsync and --swap_post_effect, weren't registered yet at that
// point and were dropped, so the game always rendered at 1280x720. Loading the plugin
// here, during static initialization (before main), registers its settings first.
// The runtime's own load later finds the DLL already loaded and reuses it.
//
// Only the plugin named by --gpu_plugin (default xenos) is loaded: the plugins define
// some of the same settings, and loading two would register them twice.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cwctype>
#include <string>
#include <string_view>

#pragma comment(lib, "shell32.lib")

namespace {

// --gpu_plugin=<name> or --gpu_plugin <name>; "native" when absent (TbcApp::OnPreSetup).
std::wstring PluginName() {
  std::wstring name = L"native";
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return name;
  }
  constexpr std::wstring_view kFlag = L"--gpu_plugin";
  for (int i = 1; i < argc; ++i) {
    std::wstring_view arg(argv[i]);
    if (arg == kFlag && i + 1 < argc) {
      name = argv[++i];
    } else if (arg.size() > kFlag.size() && arg.starts_with(kFlag) && arg[kFlag.size()] == L'=') {
      name = std::wstring(arg.substr(kFlag.size() + 1));
    }
  }
  LocalFree(argv);
  // A plugin name is a plain word; anything else is left to the runtime to reject.
  for (wchar_t c : name) {
    if (!std::iswalnum(c) && c != L'_' && c != L'-') {
      return L"native";
    }
  }
  return name.empty() ? L"native" : name;
}

HMODULE PreloadGpuPlugin() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return nullptr;
  }
  std::wstring dir(buf, n);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  const std::wstring base = dir + L"rexgpu-" + PluginName();
  // Build-type suffixes: rd = RelWithDebInfo, none = Release, d = Debug.
  for (const wchar_t* suffix : {L"rd.dll", L".dll", L"d.dll"}) {
    std::wstring path = base + suffix;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return LoadLibraryW(path.c_str());
    }
  }
  return nullptr;
}

const HMODULE g_gpu_plugin = PreloadGpuPlugin();

}  // namespace
#endif  // _WIN32

#if defined(__linux__)
// Linux: the same, with dlopen. The command line from /proc/self/cmdline; the plugin is
// librexgpu-<name>[rd|d].so next to the executable (the SDK's loader looks there too, and gets
// this same handle back).
#include <dlfcn.h>
#include <unistd.h>

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

std::string PluginName() {
  std::string name = "native";
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::string> argv;
  for (size_t start = 0; start < all.size();) {
    const size_t end = all.find('\0', start);
    argv.emplace_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  const std::string flag = "--gpu_plugin";
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& arg = argv[i];
    if (arg == flag && i + 1 < argv.size()) {
      name = argv[++i];
    } else if (arg.size() > flag.size() && arg.compare(0, flag.size(), flag) == 0 && arg[flag.size()] == '=') {
      name = arg.substr(flag.size() + 1);
    }
  }
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return "native";
  }
  return name.empty() ? "native" : name;
}

void* PreloadGpuPlugin() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return nullptr;
  std::string dir(buf, size_t(n));
  dir.resize(dir.find_last_of('/') + 1);
  const std::string base = dir + "librexgpu-" + PluginName();
  for (const char* suffix : {"rd.so", ".so", "d.so"}) {
    const std::string path = base + suffix;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  }
  return nullptr;
}

void* const g_gpu_plugin = PreloadGpuPlugin();

}  // namespace
#endif  // __linux__
