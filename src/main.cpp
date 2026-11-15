#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef LIST_MODULES_ALL
#define LIST_MODULES_ALL 0x03
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "1.2.0";
constexpr const char* kGameExe = "PPJ2DD.EXE";
constexpr const char* kD3DDll = "HGL_D3D.DLL";
constexpr const char* kIni = "VCOP2.INI";
constexpr const char* kSettingsFile = "VC2HD.ini";
constexpr const char* kD3DBackup = "VC2HD.d3d.bak";
constexpr const char* kIniBackup = "VC2HD.ini.bak";
constexpr const char* kLogFile = "VC2HD.log";

constexpr uintptr_t kGamePreferredBase = 0x00400000;
constexpr uintptr_t kHeightCmpRva = 0x61CA;
constexpr uintptr_t kWidthCmpRva = 0x61DD;
constexpr uint64_t kModeTableOffset = 0xE430;

constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
constexpr uint32_t kMinWidth = 320;
constexpr uint32_t kMinHeight = 240;
constexpr uint32_t kMaxWidth = 7680;
constexpr uint32_t kMaxHeight = 4320;
constexpr uint32_t kColor = 32;

struct Resolution {
  uint32_t width;
  uint32_t height;
};

fs::path g_dir;
std::string g_logPath;
Resolution g_requested{kDefaultWidth, kDefaultHeight};
Resolution g_effective{kDefaultWidth, kDefaultHeight};
std::vector<Resolution> g_modes;
Resolution g_current{0, 0};
bool g_haveCurrent = false;
bool g_autoDetect = false;

bool sameResolution(const Resolution& a, const Resolution& b) {
  return a.width == b.width && a.height == b.height;
}

std::string resolutionText(const Resolution& value) {
  return std::to_string(value.width) + "x" + std::to_string(value.height);
}

bool validWidth(uint32_t width) {
  return width >= kMinWidth && width <= kMaxWidth && width % 2 == 0;
}

bool validHeight(uint32_t height) {
  return height >= kMinHeight && height <= kMaxHeight && height % 2 == 0;
}

std::string trim(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

void logLine(const std::string& message) {
  if (g_logPath.empty()) return;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  std::ofstream out(g_logPath, std::ios::app);
  if (!out) return;
  char stamp[32]{};
  std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond);
  out << "[" << stamp << "] " << message << "\n";
}

void showError(const std::string& message) {
  logLine("ERROR: " + message);
  MessageBoxA(nullptr, message.c_str(), "VC2HD", MB_OK | MB_ICONERROR);
}

std::vector<uint8_t> readAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeAll(const fs::path& path, const std::vector<uint8_t>& data) {
  fs::path temp = path;
  temp += ".vc2hd.tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return false;
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

bool containsMode(const Resolution& value) {
  return std::any_of(g_modes.begin(), g_modes.end(), [&](const Resolution& mode) {
    return mode.width == value.width && mode.height == value.height;
  });
}

Resolution largestUsableMode() {
  Resolution best{0, 0};
  uint64_t bestPixels = 0;
  for (const auto& mode : g_modes) {
    if (mode.width < 640 || mode.height < 480) continue;
    const uint64_t pixels = static_cast<uint64_t>(mode.width) * mode.height;
    if (pixels > bestPixels) {
      best = mode;
      bestPixels = pixels;
    }
  }
  if (best.width != 0) return best;
  for (const auto& mode : g_modes) {
    const uint64_t pixels = static_cast<uint64_t>(mode.width) * mode.height;
    if (pixels > bestPixels) {
      best = mode;
      bestPixels = pixels;
    }
  }
  return best;
}

Resolution chooseDetectedMode() {
  if (g_haveCurrent && containsMode(g_current)) return g_current;
  const Resolution hd{kDefaultWidth, kDefaultHeight};
  if (containsMode(hd)) return hd;
  return largestUsableMode();
}

Resolution chooseFallbackMode(const Resolution& requested) {
  if (containsMode(requested)) return requested;
  if (g_haveCurrent && containsMode(g_current)) return g_current;
  return largestUsableMode();
}

std::vector<Resolution> enumerateModes() {
  std::vector<Resolution> modes;
  std::set<std::pair<uint32_t, uint32_t>> seen;

  const auto add = [&](uint32_t width, uint32_t height) {
    if (!validWidth(width) || !validHeight(height)) return;
    const auto key = std::make_pair(width, height);
    if (seen.insert(key).second) modes.push_back({width, height});
  };

  DEVMODEA mode{};
  mode.dmSize = sizeof(mode);
  for (DWORD index = 0; index < 100000 && EnumDisplaySettingsA(nullptr, index, &mode); ++index) {
    if (mode.dmBitsPerPel == kColor) add(static_cast<uint32_t>(mode.dmPelsWidth),
                                         static_cast<uint32_t>(mode.dmPelsHeight));
  }

  DEVMODEA current{};
  current.dmSize = sizeof(current);
  if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &current) && current.dmBitsPerPel == kColor) {
    const uint32_t width = static_cast<uint32_t>(current.dmPelsWidth);
    const uint32_t height = static_cast<uint32_t>(current.dmPelsHeight);
    if (validWidth(width) && validHeight(height)) {
      g_haveCurrent = true;
      g_current = {width, height};
      add(width, height);
    }
  }

  return modes;
}

bool parseUnsigned(const std::string& text, uint32_t& value) {
  if (text.empty()) return false;
  uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  value = parsed;
  return true;
}

bool writeDefaultSettings(const Resolution& mode) {
  const std::string text = "; VC2HD display settings\r\n"
                           "; AutoDetect=1 uses the current Windows desktop resolution each launch.\r\n"
                           "; Set AutoDetect=0 to force Width/Height below.\r\n"
                           "[Display]\r\n"
                           "AutoDetect=1\r\n"
                           "Width=" + std::to_string(mode.width) + "\r\n"
                           "Height=" + std::to_string(mode.height) + "\r\n";
  return writeAll(g_dir / kSettingsFile,
                  std::vector<uint8_t>(text.begin(), text.end()));
}

bool ensureSettings() {
  const fs::path settingsPath = g_dir / kSettingsFile;
  std::error_code ec;
  const bool exists = fs::exists(settingsPath, ec);

  g_modes = enumerateModes();
  if (g_modes.empty()) {
    showError("No 32-bit display mode was reported by Windows. Set the desktop to a standard "
              "32-bit resolution and try again.");
    return false;
  }

  bool haveWidth = false;
  bool haveHeight = false;
  g_autoDetect = false;

  if (!exists) {
    g_autoDetect = true;
    const Resolution detected = chooseDetectedMode();
    if (detected.width == 0) {
      showError("VC2HD could not detect a usable display mode.");
      return false;
    }
    if (!writeDefaultSettings(detected)) {
      showError("Could not create VC2HD.ini. The game folder must be writable.");
      return false;
    }
    g_requested = detected;
    logLine("Created VC2HD.ini with AutoDetect=1 (" + resolutionText(detected) + ").");
  } else {
    const auto bytes = readAll(settingsPath);
    if (bytes.empty()) {
      showError("VC2HD.ini is empty. Delete it to recreate defaults, or set AutoDetect=1.");
      return false;
    }

    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
      text.erase(0, 3);
    }

    size_t start = 0;
    while (start <= text.size()) {
      const size_t end = text.find('\n', start);
      std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string stripped = trim(line);
      if (!stripped.empty() && stripped.front() != ';' && stripped.front() != '#' &&
          stripped.front() != '[') {
        const size_t equals = stripped.find('=');
        if (equals != std::string::npos) {
          const std::string key = lower(trim(stripped.substr(0, equals)));
          const std::string value = trim(stripped.substr(equals + 1));
          if (key == "autodetect") {
            uint32_t parsed = 0;
            if (!parseUnsigned(value, parsed) || parsed > 1) {
              showError("VC2HD.ini has an invalid AutoDetect value. Use AutoDetect=1 or AutoDetect=0.");
              return false;
            }
            g_autoDetect = parsed != 0;
          } else if (key == "width" || key == "height") {
            uint32_t parsed = 0;
            const bool valid = parseUnsigned(value, parsed) &&
                               (key == "width" ? validWidth(parsed) : validHeight(parsed));
            if (!valid) {
              showError("VC2HD.ini has an invalid " + std::string(key == "width" ? "Width" : "Height") +
                        ". Use even values: Width 320-7680, Height 240-4320.");
              return false;
            }
            if (key == "width") {
              g_requested.width = parsed;
              haveWidth = true;
            } else {
              g_requested.height = parsed;
              haveHeight = true;
            }
          }
        }
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (!g_autoDetect && (!haveWidth || !haveHeight)) {
      showError("VC2HD.ini must contain Width= and Height= when AutoDetect=0.");
      return false;
    }
  }

  if (g_autoDetect) {
    const Resolution detected = chooseDetectedMode();
    if (detected.width == 0) {
      showError("VC2HD could not detect a usable display mode.");
      return false;
    }
    g_requested = detected;
    g_effective = detected;
    logLine("Auto-detected display mode " + resolutionText(detected) + ".");
  } else {
    g_effective = chooseFallbackMode(g_requested);
    if (g_effective.width == 0) {
      showError("No usable 32-bit display mode was found for " + resolutionText(g_requested) + ".");
      return false;
    }
    if (!sameResolution(g_effective, g_requested)) {
      logLine("Requested " + resolutionText(g_requested) + " is not reported by the display driver; using " +
              resolutionText(g_effective) + ".");
    }
  }
  logLine("Using " + resolutionText(g_effective) + "x" + std::to_string(kColor) + ".");
  return true;
}

bool restoreD3D() {
  const fs::path backup = g_dir / kD3DBackup;
  std::error_code ec;
  if (!fs::exists(backup, ec)) return true;
  const auto original = readAll(backup);
  if (original.size() != 12) {
    logLine("Stored HGL_D3D backup has an unexpected size.");
    return false;
  }
  const fs::path dllPath = g_dir / kD3DDll;
  auto dll = readAll(dllPath);
  if (dll.size() < kModeTableOffset + 12) {
    logLine("HGL_D3D.DLL is missing or too small; removing stale backup.");
    fs::remove(backup, ec);
    return true;
  }
  std::memcpy(dll.data() + kModeTableOffset, original.data(), original.size());
  if (!writeAll(dllPath, dll)) return false;
  fs::remove(backup, ec);
  logLine("Restored HGL_D3D.DLL.");
  return true;
}

bool restoreIni() {
  const fs::path backup = g_dir / kIniBackup;
  std::error_code ec;
  if (!fs::exists(backup, ec)) return true;
  const auto original = readAll(backup);
  if (original.empty()) {
    logLine("Stored VCOP2.INI backup is empty.");
    return false;
  }
  if (!writeAll(g_dir / kIni, original)) return false;
  fs::remove(backup, ec);
  logLine("Restored VCOP2.INI.");
  return true;
}

bool restoreAll() {
  const bool ini = restoreIni();
  const bool d3d = restoreD3D();
  return ini && d3d;
}

bool prepareIni() {
  const fs::path iniPath = g_dir / kIni;
  const auto original = readAll(iniPath);
  if (original.empty()) {
    logLine("VCOP2.INI is missing or empty.");
    return false;
  }
  if (!writeAll(g_dir / kIniBackup, original)) {
    logLine("Could not create the VCOP2.INI backup.");
    return false;
  }

  std::string text(original.begin(), original.end());
  const bool crlf = text.find("\r\n") != std::string::npos;
  const bool trailingNewline = !text.empty() && text.back() == '\n';

  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!lines.empty() && lines.back().empty() && trailingNewline) lines.pop_back();

  bool inGraphicDevice = false;
  std::set<std::string> seen;
  for (auto& line : lines) {
    const std::string stripped = trim(line);
    if (stripped.size() >= 2 && stripped.front() == '[' && stripped.back() == ']') {
      inGraphicDevice = lower(stripped.substr(1, stripped.size() - 2)) == "graphicdevice";
      continue;
    }
    if (!inGraphicDevice) continue;

    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = lower(trim(line.substr(0, equals)));
    std::string value;
    if (key == "width") value = std::to_string(g_effective.width);
    else if (key == "height") value = std::to_string(g_effective.height);
    else if (key == "mode") value = "1";
    else if (key == "color") value = "32";
    else if (key == "d3d") value = "7";
    else if (key == "dd") value = "0";
    else continue;
    line = line.substr(0, equals + 1) + value;
    seen.insert(key);
  }

  if (seen.size() != 6) {
    logLine("VCOP2.INI does not contain all required [GraphicDevice] keys.");
    return false;
  }

  std::string patched;
  for (size_t i = 0; i < lines.size(); ++i) {
    patched += lines[i];
    if (i + 1 < lines.size()) patched += crlf ? "\r\n" : "\n";
  }
  if (trailingNewline) patched += crlf ? "\r\n" : "\n";

  std::vector<uint8_t> bytes(patched.begin(), patched.end());
  if (!writeAll(iniPath, bytes)) {
    logLine("Could not write the patched VCOP2.INI.");
    return false;
  }
  logLine("Configured VCOP2.INI for " + resolutionText(g_effective) + "x" + std::to_string(kColor) + ".");
  return true;
}

bool prepareD3D() {
  const fs::path dllPath = g_dir / kD3DDll;
  auto dll = readAll(dllPath);
  if (dll.size() < kModeTableOffset + 12) {
    logLine("HGL_D3D.DLL is missing or too small.");
    return false;
  }

  const uint8_t expected[12] = {0x80, 0x02, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00,
                                0x10, 0x00, 0x00, 0x00};
  if (std::memcmp(dll.data() + kModeTableOffset, expected, sizeof(expected)) != 0) {
    logLine("HGL_D3D.DLL does not match the supported build.");
    return false;
  }

  std::vector<uint8_t> backup(dll.begin() + static_cast<std::ptrdiff_t>(kModeTableOffset),
                              dll.begin() + static_cast<std::ptrdiff_t>(kModeTableOffset + 12));
  if (!writeAll(g_dir / kD3DBackup, backup)) {
    logLine("Could not create the HGL_D3D backup.");
    return false;
  }

  const uint32_t values[3] = {g_effective.width, g_effective.height, kColor};
  std::memcpy(dll.data() + kModeTableOffset, values, sizeof(values));
  if (!writeAll(dllPath, dll)) {
    std::error_code ec;
    fs::remove(g_dir / kD3DBackup, ec);
    logLine("Could not write the patched HGL_D3D.DLL.");
    return false;
  }
  logLine("Added " + resolutionText(g_effective) + "x" + std::to_string(kColor) +
          " to the HGL_D3D mode table.");
  return true;
}

uintptr_t moduleBase(HANDLE process) {
  HMODULE modules[1024]{};
  DWORD needed = 0;
  if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
    MODULEINFO info{};
    if (GetModuleInformation(process, modules[0], &info, sizeof(info))) {
      return reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    }
  }
  return kGamePreferredBase;
}

bool patchGameMemory(HANDLE process) {
  const uintptr_t base = moduleBase(process);
  uint8_t mz[2]{};
  SIZE_T transferred = 0;
  if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base), mz, sizeof(mz), &transferred) ||
      transferred != sizeof(mz) || mz[0] != 'M' || mz[1] != 'Z') {
    logLine("Could not read the PPJ2DD.EXE image header.");
    return false;
  }

  const auto patch = [&](uintptr_t rva, const uint8_t (&expected)[4], const uint8_t (&replacement)[4]) {
    uint8_t current[4]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + rva), current, sizeof(current), &read) ||
        read != sizeof(current) || std::memcmp(current, expected, sizeof(expected)) != 0) {
      return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, reinterpret_cast<LPVOID>(base + rva), replacement, sizeof(replacement),
                            &written) ||
        written != sizeof(replacement)) {
      return false;
    }
    FlushInstructionCache(process, reinterpret_cast<LPCVOID>(base + rva), sizeof(replacement));
    return true;
  };

  const uint8_t heightExpected[4] = {0xE0, 0x01, 0x00, 0x00};
  uint8_t heightReplacement[4]{};
  const uint32_t height = g_effective.height;
  std::memcpy(heightReplacement, &height, sizeof(height));

  const uint8_t widthExpected[4] = {0x80, 0x02, 0x00, 0x00};
  uint8_t widthReplacement[4]{};
  const uint32_t width = g_effective.width;
  std::memcpy(widthReplacement, &width, sizeof(width));

  if (!patch(kHeightCmpRva, heightExpected, heightReplacement)) {
    logLine("The height clamp did not match the supported PPJ2DD.EXE build.");
    return false;
  }
  if (!patch(kWidthCmpRva, widthExpected, widthReplacement)) {
    logLine("The width clamp did not match the supported PPJ2DD.EXE build.");
    return false;
  }
  logLine("Patched the PPJ2DD.EXE resolution clamps for " + resolutionText(g_effective) + ".");
  return true;
}

}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR commandLine, int) {
  wchar_t modulePath[MAX_PATH]{};
  GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
  g_dir = fs::path(modulePath).parent_path();
  g_logPath = (g_dir / kLogFile).string();
  SetCurrentDirectoryW(g_dir.c_str());

  try {
    const std::string args = commandLine ? lower(commandLine) : std::string();
    if (args.find("--restore") != std::string::npos) {
      const bool restored = restoreAll();
      MessageBoxA(nullptr, restored ? "Previous VC2HD changes were restored."
                                    : "VC2HD could not restore its backups. Check VC2HD.log.",
                  "VC2HD", MB_OK | (restored ? MB_ICONINFORMATION : MB_ICONERROR));
      return restored ? 0 : 1;
    }

    HANDLE mutex = CreateMutexA(nullptr, TRUE, "VC2HD_Loader");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBoxA(nullptr, "VC2HD is already running.", "VC2HD", MB_OK | MB_ICONINFORMATION);
      return 1;
    }

    logLine(std::string("VC2HD ") + kVersion + " starting.");

    if (!fs::exists(g_dir / kGameExe) || !fs::exists(g_dir / kD3DDll) || !fs::exists(g_dir / kIni)) {
      showError("Place VC2HD.exe in the Virtua Cop 2 folder next to PPJ2DD.EXE, HGL_D3D.DLL and VCOP2.INI.");
      return 1;
    }

    if (!restoreAll()) {
      showError("VC2HD found an unfinished previous run but could not restore its backups. Check VC2HD.log.");
      return 1;
    }

    if (!ensureSettings()) return 1;

    if (!prepareIni() || !prepareD3D()) {
      restoreAll();
      showError("This copy of Virtua Cop 2 is not the supported PC build, or the folder is not writable. "
                "Close the game and check VC2HD.log.");
      return 1;
    }

    const std::string gamePath = (g_dir / kGameExe).string();
    std::string command = "\"" + gamePath + "\"";
    std::vector<char> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::string workingDirectory = g_dir.string();
    if (!CreateProcessA(gamePath.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, workingDirectory.c_str(), &startup, &process)) {
      const DWORD error = GetLastError();
      restoreAll();
      showError("Could not start PPJ2DD.EXE (Windows error " + std::to_string(error) + ").");
      return 1;
    }

    if (!patchGameMemory(process.hProcess)) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 5000);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      restoreAll();
      showError("Could not apply the " + resolutionText(g_effective) +
                " patch to PPJ2DD.EXE. This loader supports the 1997 PC build only. See VC2HD.log.");
      return 1;
    }

    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    logLine("Launched PPJ2DD.EXE at " + resolutionText(g_effective) + ".");

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    logLine("PPJ2DD.EXE exited with code " + std::to_string(exitCode) + ".");

    if (!restoreAll()) {
      showError("The game exited, but VC2HD could not restore VCOP2.INI or HGL_D3D.DLL. Check VC2HD.log.");
      return 1;
    }

    if (mutex) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
    }
    return 0;
  } catch (const std::exception& error) {
    restoreAll();
    showError(std::string("VC2HD failed: ") + error.what());
    return 1;
  } catch (...) {
    restoreAll();
    showError("VC2HD failed with an unknown error. Check VC2HD.log.");
    return 1;
  }
}
