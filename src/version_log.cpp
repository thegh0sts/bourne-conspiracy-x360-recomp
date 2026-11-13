// tbc - the first lines of every log: which build this is and how it was started.
//
// The release (TBC_VERSION, from project() in CMakeLists.txt), when this exe was built, and the
// options it was started with, so a log someone sends shows which build and settings it came
// from. Folder paths are left out, since they hold the Windows user name.

#include "generated/default/tbc_pch.h"

#include <string>

#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#ifndef TBC_VERSION
#define TBC_VERSION "dev"
#endif

namespace tbc {

void LogVersion() {
  REXLOG_INFO("The Bourne Conspiracy PC {} (built {} {})", TBC_VERSION, __DATE__, __TIME__);
#ifdef _WIN32
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;
  std::string options;
  for (int i = 1; i < argc; ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string arg(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), n, nullptr, nullptr);
    if (arg.rfind("--", 0) != 0) {
      // The value of "--flag value"; a path if it has a slash or a drive letter.
      if (arg.find_first_of("\\/:") != std::string::npos) arg = "<path>";
    } else if (const auto eq = arg.find('='); eq != std::string::npos &&
               arg.find_first_of("\\/:", eq) != std::string::npos) {
      arg = arg.substr(0, eq + 1) + "<path>";
    }
    options += ' ';
    options += arg;
  }
  LocalFree(argv);
  REXLOG_INFO("Options:{}", options.empty() ? " (none)" : options);
#endif
}

}  // namespace tbc
