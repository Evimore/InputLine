#include "tray.h"

#include "desktop.h"
#include "log.h"
#include "status_file.h"
#include "update_check.h"
#include "tray_badge.h"
#include "usbip_attach.h"

#ifdef _WIN32
  #include <windows.h>

  #include <sddl.h>
  #include <shellapi.h>
  #include <userenv.h>
  #include <wtsapi32.h>

  #include <algorithm>
  #include <cmath>
  #include <cstdint>
  #include <ctime>
  #include <map>
  #include <mutex>
  #include <vector>
#endif

namespace inputline::tray {

#ifdef _WIN32

  namespace {
    /** Signalled by the service when it stops: every helper quits. */
    const wchar_t *const kQuitEventName = L"Global\\InputLineQuit";
    const wchar_t *const kWindowClass = L"InputLineTray";
    const wchar_t *const kSettingsKey = L"Software\\InputLine";
    const wchar_t *const kHiddenValue = L"HideTrayIcon";
    constexpr ULONG_PTR kNotificationMagic = 0x494C4E31;  // "ILN1"
    constexpr std::int64_t kStaleAfterSeconds = 15;

    // ---- Service side --------------------------------------------------------

    std::mutex g_agents_mutex;
    bool g_agents_started = false;
    HANDLE g_quit = nullptr;
    std::map<DWORD, HANDLE> g_trays;  ///< by session
    std::vector<HANDLE> g_helpers;    ///< notifiers and trays, to wait for on stop

    /** Close handles of helpers that already exited. */
    void reap_locked() {
      g_helpers.erase(std::remove_if(g_helpers.begin(), g_helpers.end(), [](HANDLE process) {
                        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                          for (auto it = g_trays.begin(); it != g_trays.end(); ++it) {
                            if (it->second == process) {
                              g_trays.erase(it);
                              break;
                            }
                          }
                          CloseHandle(process);
                          return true;
                        }
                        return false;
                      }),
                      g_helpers.end());
    }

    /** Start 'inputline-host <arguments>' in @p session as its user. Lock held. */
    HANDLE launch_locked(DWORD session, const std::string &arguments) {
      HANDLE token = nullptr;
      if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) {
        return nullptr;  // nobody signed in there
      }
      void *environment = nullptr;
      if (!CreateEnvironmentBlock(&environment, token, FALSE)) {
        environment = nullptr;
      }
      std::wstring command_line = desktop::widen(desktop::arg_quote(desktop::executable_path()) + " " + arguments);
      std::wstring winsta = L"winsta0\\default";
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      startup.lpDesktop = winsta.data();
      PROCESS_INFORMATION process {};
      const DWORD flags = CREATE_NO_WINDOW | (environment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0);
      const BOOL started = CreateProcessAsUserW(token, nullptr, command_line.data(), nullptr, nullptr, FALSE, flags, environment, nullptr, &startup, &process);
      const DWORD error = GetLastError();
      if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
      }
      CloseHandle(token);
      if (!started) {
        log::debug("tray: cannot start a helper in session ", session, " (error ", error, ")");
        return nullptr;
      }
      CloseHandle(process.hThread);
      g_helpers.push_back(process.hProcess);
      return process.hProcess;
    }

    void start_tray_locked(DWORD session) {
      reap_locked();
      if (g_trays.count(session) != 0) {
        return;
      }
      if (HANDLE process = launch_locked(session, "tray")) {
        g_trays[session] = process;
      }
    }

    // ---- The tray icon -------------------------------------------------------

    constexpr UINT kIconMessage = WM_APP + 1;
    enum MenuId : UINT {
      kMenuOpenLog = 1,
      kMenuGuide,
      kMenuUpdate,
      kMenuHide,
      kMenuUsbip,
    };

    struct TrayState {
      NOTIFYICONDATAW icon {};
      UINT taskbar_created = 0;
      ServiceStatus status;
      bool running = false;
      std::wstring summary;
      std::wstring notification_text;
      std::wstring notification_url;
      HICON normal_icon = nullptr;
      HICON warning_icon = nullptr;     ///< with a red X badge: something needs the user
      HICON app_icon = nullptr;         ///< with a white badge, green play: the app is connected
      HICON controller_icon = nullptr;  ///< with a green badge, white play: a controller is plugged in
      std::wstring problem;             ///< what the badge is about, for the menu and tooltip
    };

    TrayState *g_tray = nullptr;

    /** Only ever open InputLine's own pages and usbip-win2's download page. */
    bool safe_url(const std::wstring &url) {
      for (const std::string &allowed : {std::string(update::kRepositoryUrl) + "/", std::string(kUsbipDownloadUrl)}) {
        const std::wstring prefix = desktop::widen(allowed);
        if (url.compare(0, prefix.size(), prefix) == 0) {
          return true;
        }
      }
      return false;
    }

    /** @p base with a small round badge in its top left corner. */
    HICON badged_icon(HICON base, int size, Badge badge) {
      BITMAPINFO info {};
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = size;
      info.bmiHeader.biHeight = -size;  // top-down
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr;
      HDC screen = GetDC(nullptr);
      HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      ReleaseDC(nullptr, screen);
      if (color == nullptr || bits == nullptr) {
        return nullptr;
      }
      // Copy the icon's own pixels, alpha included (drawing it can lose the alpha).
      auto *pixels = static_cast<std::uint32_t *>(bits);
      ICONINFO base_info {};
      if (!GetIconInfo(base, &base_info)) {
        DeleteObject(color);
        return nullptr;
      }
      HDC dc = CreateCompatibleDC(nullptr);
      const bool copied = GetDIBits(dc, base_info.hbmColor, 0, static_cast<UINT>(size), pixels, &info, DIB_RGB_COLORS) != 0;
      bool any_alpha = false;
      for (int i = 0; copied && i < size * size; ++i) {
        any_alpha = any_alpha || (pixels[i] >> 24) != 0;
      }
      if (copied && !any_alpha) {
        // An icon without alpha: its mask says which pixels show.
        std::vector<std::uint32_t> mask_pixels(static_cast<std::size_t>(size) * size);
        if (GetDIBits(dc, base_info.hbmMask, 0, static_cast<UINT>(size), mask_pixels.data(), &info, DIB_RGB_COLORS) != 0) {
          for (int i = 0; i < size * size; ++i) {
            pixels[i] = (mask_pixels[i] & 0xFFFFFF) == 0 ? pixels[i] | 0xFF000000 : 0;
          }
        }
      }
      DeleteDC(dc);
      DeleteObject(base_info.hbmColor);
      DeleteObject(base_info.hbmMask);
      if (!copied) {
        DeleteObject(color);
        return nullptr;
      }
      draw_badge(pixels, size, badge);
      HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
      ICONINFO icon_info {};
      icon_info.fIcon = TRUE;
      icon_info.hbmColor = color;
      icon_info.hbmMask = mask;
      HICON icon = CreateIconIndirect(&icon_info);
      DeleteObject(color);
      DeleteObject(mask);
      return icon;
    }

    void open_url(const std::wstring &url) {
      if (safe_url(url)) {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
    }

    bool hidden_by_user() {
      DWORD value = 0;
      DWORD size = sizeof(value);
      return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kHiddenValue, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
    }

    void set_hidden_by_user(bool hidden) {
      HKEY key = nullptr;
      if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
      }
      const DWORD value = hidden ? 1 : 0;
      RegSetValueExW(key, kHiddenValue, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
      RegCloseKey(key);
    }

    std::wstring join_devices(const std::vector<std::string> &devices) {
      std::string joined;
      for (const auto &device : devices) {
        joined += (joined.empty() ? "" : ", ") + device;
      }
      return desktop::widen(joined);
    }

    /** Read the service's status file and update the tooltip. */
    void refresh(TrayState &tray) {
      const auto status = read_status_file(desktop::data_dir() + "\\status.txt");
      const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
      tray.running = status.has_value() && now - status->time <= kStaleAfterSeconds && now >= status->time - kStaleAfterSeconds;
      tray.status = status.value_or(ServiceStatus {});
      if (!tray.running) {
        tray.summary = L"InputLine isn't running";
      } else if (tray.status.devices.empty()) {
        tray.summary = L"Waiting for the InputLine app";
      } else {
        tray.summary = L"Connected: " + join_devices(tray.status.devices);
        if (tray.status.controllers > 0) {
          tray.summary += tray.status.controllers == 1 ? L" (1 controller)" : L" (" + std::to_wstring(tray.status.controllers) + L" controllers)";
        }
      }
      // usbip-win2 missing or too old: a badge, and the reason first in the tooltip and menu.
      tray.problem.clear();
      if (tray.running && tray.status.usbip == "missing") {
        tray.problem = L"usbip-win2 isn't installed";
      } else if (tray.running && tray.status.usbip == "old") {
        tray.problem = L"usbip-win2 " + desktop::widen(tray.status.usbip_version) + L" is too old";
      }
      const std::wstring tip = L"InputLine: " + (tray.problem.empty() ? tray.summary : tray.problem);
      // A problem first; then a plugged-in controller; then a connected app.
      HICON wanted = !tray.problem.empty() ? tray.warning_icon
                   : tray.running && tray.status.controllers > 0 ? tray.controller_icon
                   : tray.running && !tray.status.devices.empty() ? tray.app_icon
                   : nullptr;
      if (wanted == nullptr) {
        wanted = tray.normal_icon;
      }
      if (wcsncmp(tray.icon.szTip, tip.c_str(), ARRAYSIZE(tray.icon.szTip) - 1) != 0 || tray.icon.hIcon != wanted) {
        tray.icon.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_ICON;
        tray.icon.hIcon = wanted;
        wcsncpy_s(tray.icon.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray.icon);
      }
    }

    void add_icon(TrayState &tray) {
      tray.icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
      Shell_NotifyIconW(NIM_ADD, &tray.icon);
      tray.icon.uVersion = NOTIFYICON_VERSION_4;
      Shell_NotifyIconW(NIM_SETVERSION, &tray.icon);
    }

    void show_menu(HWND window, TrayState &tray) {
      refresh(tray);
      HMENU menu = CreatePopupMenu();
      const std::wstring title = L"InputLine " + desktop::widen(tray.running ? tray.status.version : std::string());
      AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title.c_str());
      std::wstring problem;
      if (!tray.problem.empty()) {
        problem = L"\u26A0 " + tray.problem + L": click to download it";
        AppendMenuW(menu, MF_STRING, kMenuUsbip, problem.c_str());
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"InputLine needs usbip-win2 " + desktop::widen(kMinUsbipVersion) + L" or newer to plug the controller into Windows").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      }
      AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, tray.summary.c_str());
      std::wstring update;
      if (tray.running && !tray.status.update_version.empty()) {
        update = L"Update available: InputLine " + desktop::widen(tray.status.update_version) + L"...";
        AppendMenuW(menu, MF_STRING, kMenuUpdate, update.c_str());
      }
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"Open the log");
      AppendMenuW(menu, MF_STRING, kMenuGuide, L"Setup guide and troubleshooting");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, kMenuHide, L"Hide this icon");

      POINT cursor {};
      GetCursorPos(&cursor);
      SetForegroundWindow(window);  // so the menu closes when clicking elsewhere
      const UINT chosen = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, window, nullptr));
      PostMessageW(window, WM_NULL, 0, 0);
      DestroyMenu(menu);

      switch (chosen) {
        case kMenuOpenLog: {
          const std::wstring log_path = desktop::widen(desktop::data_dir() + "\\inputline-host.log");
          ShellExecuteW(nullptr, L"open", L"notepad.exe", log_path.c_str(), nullptr, SW_SHOWNORMAL);
          break;
        }
        case kMenuGuide:
          open_url(desktop::widen(std::string(update::kRepositoryUrl) + "/blob/main/docs/host-setup-windows.md"));
          break;
        case kMenuUpdate:
          open_url(desktop::widen(tray.status.update_url));
          break;
        case kMenuUsbip:
          open_url(desktop::widen(kUsbipDownloadUrl));
          break;
        case kMenuHide:
          set_hidden_by_user(true);
          MessageBoxW(window,
                      L"The InputLine icon is hidden. InputLine keeps running in the background.\n\n"
                      L"To show the icon again, open InputLine from the Start menu.",
                      L"InputLine", MB_OK | MB_ICONINFORMATION);
          PostQuitMessage(0);
          break;
        default:
          break;
      }
    }

    void show_balloon(TrayState &tray, const std::wstring &title, const std::wstring &text, const std::wstring &url) {
      tray.notification_text = text;
      tray.notification_url = safe_url(url) ? url : L"";
      tray.icon.uFlags = NIF_INFO;
      wcsncpy_s(tray.icon.szInfoTitle, title.c_str(), _TRUNCATE);
      wcsncpy_s(tray.icon.szInfo, text.c_str(), _TRUNCATE);
      tray.icon.dwInfoFlags = tray.icon.hBalloonIcon != nullptr ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO;
      Shell_NotifyIconW(NIM_MODIFY, &tray.icon);
    }

    LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      TrayState *tray = g_tray;
      if (tray == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
      }
      if (message == WM_TIMER) {
        refresh(*tray);
        return 0;
      }
      if (message == tray->taskbar_created && message != 0) {
        add_icon(*tray);  // Explorer restarted
        return 0;
      }
      if (message == WM_COPYDATA) {
        const auto *copy = reinterpret_cast<const COPYDATASTRUCT *>(lparam);
        if (copy == nullptr || copy->dwData != kNotificationMagic || copy->cbData % sizeof(wchar_t) != 0 || copy->cbData > 8192) {
          return FALSE;
        }
        // title \0 text \0 url
        const std::wstring all(static_cast<const wchar_t *>(copy->lpData), copy->cbData / sizeof(wchar_t));
        const auto first = all.find(L'\0');
        const auto second = first == std::wstring::npos ? std::wstring::npos : all.find(L'\0', first + 1);
        if (second == std::wstring::npos) {
          return FALSE;
        }
        std::wstring url = all.substr(second + 1);
        url = url.substr(0, url.find(L'\0'));
        show_balloon(*tray, all.substr(0, first), all.substr(first + 1, second - first - 1), url);
        return TRUE;
      }
      if (message == kIconMessage) {
        switch (LOWORD(lparam)) {
          case WM_CONTEXTMENU:
          case NIN_SELECT:
          case NIN_KEYSELECT:
            show_menu(window, *tray);
            break;
          case NIN_BALLOONUSERCLICK:
            if (!tray->notification_url.empty()) {
              open_url(tray->notification_url);
            } else if (!tray->notification_text.empty()) {
              MessageBoxW(window, tray->notification_text.c_str(), L"InputLine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
            break;
          default:
            break;
        }
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
  }  // namespace

  void start_agents() {
    std::lock_guard lock(g_agents_mutex);
    if (g_agents_started) {
      return;
    }
    // Everyone may wait on the event; only Windows and administrators may set it.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) {
      attributes.lpSecurityDescriptor = descriptor;
    }
    g_quit = CreateEventW(descriptor != nullptr ? &attributes : nullptr, TRUE, FALSE, kQuitEventName);
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    if (g_quit != nullptr) {
      ResetEvent(g_quit);
    }
    g_agents_started = true;
    start_tray_locked(WTSGetActiveConsoleSessionId());
  }

  void session_changed(unsigned long event, unsigned long session_id) {
    std::lock_guard lock(g_agents_mutex);
    if (!g_agents_started) {
      return;
    }
    if (event == WTS_SESSION_LOGON || event == WTS_CONSOLE_CONNECT || event == WTS_REMOTE_CONNECT || event == WTS_SESSION_UNLOCK) {
      start_tray_locked(static_cast<DWORD>(session_id));
    }
  }

  void stop_agents() {
    std::lock_guard lock(g_agents_mutex);
    if (!g_agents_started) {
      return;
    }
    g_agents_started = false;
    if (g_quit != nullptr) {
      SetEvent(g_quit);
    }
    for (HANDLE process : g_helpers) {
      if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 1000);
      }
      CloseHandle(process);
    }
    g_helpers.clear();
    g_trays.clear();
    if (g_quit != nullptr) {
      CloseHandle(g_quit);
      g_quit = nullptr;
    }
  }

  bool launch_in_user_session(const std::string &arguments) {
    std::lock_guard lock(g_agents_mutex);
    reap_locked();
    return launch_locked(WTSGetActiveConsoleSessionId(), arguments) != nullptr;
  }

  void run_message_loop() {
    HANDLE quit = OpenEventW(SYNCHRONIZE, FALSE, kQuitEventName);
    for (;;) {
      if (quit == nullptr) {
        // A helper started by hand before the service: pick the event up once it exists.
        quit = OpenEventW(SYNCHRONIZE, FALSE, kQuitEventName);
      }
      const DWORD count = quit != nullptr ? 1 : 0;
      const DWORD woke = MsgWaitForMultipleObjects(count, count != 0 ? &quit : nullptr, FALSE, count != 0 ? INFINITE : 5000, QS_ALLINPUT);
      if (count != 0 && woke == WAIT_OBJECT_0) {
        break;
      }
      MSG message;
      bool done = false;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          done = true;
          break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (done) {
        break;
      }
    }
    if (quit != nullptr) {
      CloseHandle(quit);
    }
  }

  int run_tray(bool show) {
    FreeConsole();  // started from the Start menu, a console program gets a window
    if (show) {
      set_hidden_by_user(false);
    } else if (hidden_by_user()) {
      return 0;
    }
    // One icon per session.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\InputLineTray");
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(single);
      return 0;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = tray_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, kWindowClass, L"InputLine", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
      return 1;
    }

    TrayState tray;
    tray.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray.icon.cbSize = sizeof(tray.icon);
    tray.icon.hWnd = window;
    tray.icon.uID = 1;
    tray.icon.uCallbackMessage = kIconMessage;
    const auto load_icon = [instance](int size_metric) {
      const int size = GetSystemMetrics(size_metric);
      return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    tray.normal_icon = load_icon(SM_CXSMICON);
    if (tray.normal_icon != nullptr) {
      const int size = GetSystemMetrics(SM_CXSMICON);
      tray.warning_icon = badged_icon(tray.normal_icon, size, Badge::kProblem);
      tray.app_icon = badged_icon(tray.normal_icon, size, Badge::kApp);
      tray.controller_icon = badged_icon(tray.normal_icon, size, Badge::kController);
    }
    tray.icon.hIcon = tray.normal_icon;
    tray.icon.hBalloonIcon = load_icon(SM_CXICON);
    wcsncpy_s(tray.icon.szTip, L"InputLine", _TRUNCATE);
    g_tray = &tray;
    add_icon(tray);
    refresh(tray);
    SetTimer(window, 1, 2000, nullptr);

    run_message_loop();

    g_tray = nullptr;
    Shell_NotifyIconW(NIM_DELETE, &tray.icon);
    DestroyWindow(window);
    for (HICON handle : {tray.normal_icon, tray.warning_icon, tray.app_icon, tray.controller_icon, tray.icon.hBalloonIcon}) {
      if (handle != nullptr) {
        DestroyIcon(handle);
      }
    }
    if (single != nullptr) {
      CloseHandle(single);
    }
    return 0;
  }

  bool forward_notification(const std::wstring &title, const std::wstring &text, const std::wstring &url) {
    HWND window = FindWindowW(kWindowClass, nullptr);
    if (window == nullptr) {
      return false;
    }
    std::wstring payload = title;
    payload.push_back(L'\0');
    payload += text;
    payload.push_back(L'\0');
    payload += url;
    COPYDATASTRUCT copy {};
    copy.dwData = kNotificationMagic;
    copy.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    copy.lpData = payload.data();
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG, 2000, &result) != 0 && result == TRUE;
  }

#elif !defined(__linux__)  // Linux: tray_linux.cpp

  void start_agents() {}
  void session_changed(unsigned long, unsigned long) {}
  void stop_agents() {}
  bool launch_in_user_session(const std::string &) {
    return false;
  }
  void run_message_loop() {}
  int run_tray(bool) {
    return 1;
  }
  bool forward_notification(const std::wstring &, const std::wstring &, const std::wstring &) {
    return false;
  }

#endif

}  // namespace inputline::tray
