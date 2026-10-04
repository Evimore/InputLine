#include "desktop.h"

#include "log.h"
#include "tray.h"
#include "update_check.h"
#include "usbip_attach.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
  #include <windows.h>

  #include <aclapi.h>
  #include <sddl.h>
  #include <shellapi.h>
  #include <wtsapi32.h>
#else
  #include <fstream>
  #include <pwd.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <utility>
  #include <vector>
#endif

namespace inputline::desktop {

#ifdef _WIN32

  namespace {
    std::atomic<bool> g_service_mode {false};
  }  // namespace

  std::wstring widen(const std::string &text) {
    if (text.empty()) {
      return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
  }

  std::string narrow(const std::wstring &text) {
    if (text.empty()) {
      return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
  }

  std::string executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
      const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length < buffer.size()) {
        buffer.resize(length);
        return narrow(buffer);
      }
      buffer.resize(buffer.size() * 2);
    }
  }

  namespace {
    /** A PowerShell single-quoted string literal. */
    std::string ps_quote(const std::string &text) {
      std::string out = "'";
      for (char c : text) {
        out.push_back(c);
        if (c == '\'') {
          out.push_back('\'');
        }
      }
      out.push_back('\'');
      return out;
    }
  }  // namespace

  /** One argument, quoted for the Windows command line if needed (CommandLineToArgvW rules). */
  std::string arg_quote(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
      return arg;
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
      if (c == '\\') {
        ++backslashes;
        continue;
      }
      out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
      backslashes = 0;
      out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
  }

  namespace {
    /** Run a PowerShell script in this console, passed as -EncodedCommand to avoid quoting issues. */
    int run_powershell(const std::string &script) {
      const std::wstring wide = widen(script);
      static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string bytes;
      for (wchar_t c : wide) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
      }
      std::string encoded;
      for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        encoded.push_back(kAlphabet[(triple >> 18) & 63]);
        encoded.push_back(kAlphabet[(triple >> 12) & 63]);
        encoded.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 63] : '=');
        encoded.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 63] : '=');
      }
      const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;
      std::fflush(stdout);
      std::fflush(stderr);
      return std::system(command.c_str());
    }

    const char *const kCommonScript =
      "$ErrorActionPreference = 'Stop'\n"
      "$name = 'InputLine'\n"
      "$dir = Join-Path $env:ProgramFiles 'InputLine'\n"
      "$exe = Join-Path $dir 'inputline-host.exe'\n"
      "function Stop-Installed {\n"
      "  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue\n"
      "  Get-Process inputline-host -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force\n"
      "  Start-Sleep -Milliseconds 500\n"
      "}\n";
  }  // namespace

  namespace {
    constexpr UINT kNotifyIconMessage = WM_APP + 1;
    constexpr int kPairingSeconds = 120;  // how long a pairing code works

    /** The printable part of a name a device sent us, kept short. */
    std::string printable_name(const std::string &name) {
      std::string out;
      for (unsigned char c : name) {
        if (c >= 0x20 && c != 0x7F) {
          out.push_back(static_cast<char>(c));
        }
      }
      if (out.size() > 60) {
        out.resize(60);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
          out.pop_back();  // drop a cut-off UTF-8 sequence
        }
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) {
          out.pop_back();
        }
        out += "...";
      }
      return out.empty() ? "A device" : out;
    }

    /**
     * Start 'inputline-host notify' in the signed-in user's session (from the
     * service) or next to us, without waiting for it.
     */
    bool start_notifier(const std::string &title, const std::string &text, const std::string &url, int seconds) {
      std::string arguments = "notify " + arg_quote(title) + " " + arg_quote(text) + " " + std::to_string(seconds);
      if (!url.empty()) {
        arguments += " " + arg_quote(url);
      }
      if (g_service_mode) {
        // A service has no desktop of its own: run it as the user signed in at the PC.
        return tray::launch_in_user_session(arguments);
      }
      std::wstring command_line = widen(arg_quote(executable_path()) + " " + arguments);
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process {};
      if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        log::debug("cannot start the notification (error ", GetLastError(), ")");
        return false;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
    }

    struct NotifierState {
      std::wstring text;
      std::wstring url;
    };

    /** Only ever open InputLine's own pages and usbip-win2's download page. */
    bool safe_url(const std::wstring &url) {
      for (const std::string &allowed : {std::string(update::kRepositoryUrl) + "/", std::string(kUsbipDownloadUrl)}) {
        const std::wstring prefix = widen(allowed);
        if (url.compare(0, prefix.size(), prefix) == 0) {
          return true;
        }
      }
      return false;
    }

    /** Window of the notifier: the tray icon's messages and its timer. */
    LRESULT CALLBACK notifier_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      if (message == WM_TIMER) {
        PostQuitMessage(0);
        return 0;
      }
      if (message == kNotifyIconMessage) {
        // Clicking the icon or the notification opens its page or shows the text again.
        const UINT event = LOWORD(lparam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == NIN_BALLOONUSERCLICK) {
          const auto *state = reinterpret_cast<const NotifierState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
          if (!state->url.empty() && safe_url(state->url)) {
            ShellExecuteW(nullptr, L"open", state->url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          } else {
            MessageBoxW(window, state->text.c_str(), L"InputLine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
          }
        }
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
  }  // namespace

  bool service_mode() {
    return g_service_mode;
  }

  bool show_notification(const std::string &title, const std::string &text, const std::string &url) {
    return start_notifier(title, text, url, 30);
  }

  void show_pairing_code(const std::string &client_name, const std::string &code) {
    std::string spaced = code;
    if (spaced.size() == 6) {
      spaced.insert(3, " ");
    }
    const std::string name = printable_name(client_name);
    // A Windows notification, like other background apps show. Windows
    // limits its title to 63 characters and its text to 255.
    const std::string title = "InputLine pairing code: " + spaced;
    const std::string text = name + " wants to connect a Steam Controller to this PC. Enter the code in InputLine on it. "
                                    "It works for 2 minutes. Didn't ask for this? Ignore it.";
    if (start_notifier(title, text, {}, kPairingSeconds)) {
      return;
    }

    // No notification: fall back to a message box.
    const std::wstring box = widen(
      name + " wants to connect a Steam Controller to this PC with InputLine.\n\n"
             "Enter this code on it:\n\n"
             "        " +
      spaced +
      "\n\n"
      "The code works for 2 minutes. If you did not ask for this, click OK and ignore it."
    );
    if (g_service_mode) {
      // Ask Windows to show it in the signed-in user's session, without waiting for OK.
      const DWORD session = WTSGetActiveConsoleSessionId();
      if (session == 0xFFFFFFFF) {
        log::warn("pairing: nobody is signed in to this PC to see the code");
        return;
      }
      std::wstring title_w = L"InputLine";
      std::wstring message = box;
      DWORD response = 0;
      if (!WTSSendMessageW(
            WTS_CURRENT_SERVER_HANDLE, session, title_w.data(), static_cast<DWORD>(title_w.size() * sizeof(wchar_t)),
            message.data(), static_cast<DWORD>(message.size() * sizeof(wchar_t)),
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND, kPairingSeconds, &response, FALSE
          )) {
        log::warn("pairing: could not show the code on screen (error ", GetLastError(), ")");
      }
      return;
    }
    std::thread([box] {
      MessageBoxW(nullptr, box.c_str(), L"InputLine", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
  }

  int run_notifier() {
    // inputline-host notify TITLE TEXT SECONDS [URL], read as UTF-16 so names
    // in any language survive.
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr || count < 4) {
      if (argv != nullptr) {
        LocalFree(argv);
      }
      return 2;
    }
    const std::wstring title = argv[2];
    NotifierState state;
    state.text = argv[3];
    const int seconds = count > 4 ? std::max(5, std::min(600, _wtoi(argv[4]))) : kPairingSeconds;
    state.url = count > 5 && safe_url(argv[5]) ? argv[5] : L"";
    LocalFree(argv);

    // With the InputLine icon showing, the notification comes from it.
    if (tray::forward_notification(title, state.text, state.url)) {
      return 0;
    }
    const std::wstring &text = state.text;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = notifier_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"InputLineNotifier";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"InputLine", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
      return 1;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    NOTIFYICONDATAW icon {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO | NIF_SHOWTIP;
    icon.uCallbackMessage = kNotifyIconMessage;
    const auto load_icon = [instance](int size_metric) {
      const int size = GetSystemMetrics(size_metric);
      return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    icon.hIcon = load_icon(SM_CXSMICON);
    icon.hBalloonIcon = load_icon(SM_CXICON);
    wcsncpy_s(icon.szTip, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
    icon.dwInfoFlags = icon.hBalloonIcon != nullptr ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO;
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
      DestroyWindow(window);
      return 1;
    }
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);

    // Keep the icon (and so the notification in the notification centre)
    // for as long as the code works.
    // Also quit when the service stops, so an upgrade can replace this program.
    SetTimer(window, 1, static_cast<UINT>(seconds) * 1000, nullptr);
    tray::run_message_loop();
    Shell_NotifyIconW(NIM_DELETE, &icon);
    DestroyWindow(window);
    for (HICON handle : {icon.hIcon, icon.hBalloonIcon}) {
      if (handle != nullptr) {
        DestroyIcon(handle);
      }
    }
    return 0;
  }

  void hide_console() {
    FreeConsole();
  }

  bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
  }

  std::string data_dir() {
    const char *base = std::getenv("ProgramData");
    return std::string(base && *base ? base : "C:\\ProgramData") + "\\InputLine";
  }

  namespace {
    /**
     * Replace a folder's permissions with `sddl`, not inherited from above.
     * Files inside pick them up too.
     */
    bool secure_directory(const std::string &path, const wchar_t *sddl) {
      std::error_code error;
      std::filesystem::create_directories(path, error);
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
      }
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present;
      if (ok) {
        std::wstring wide = widen(path);
        ok = SetNamedSecurityInfoW(
               wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
               nullptr, nullptr, dacl, nullptr
             ) == ERROR_SUCCESS;
      }
      LocalFree(descriptor);
      return ok;
    }

    // Windows (SYSTEM) and administrators: full control; users: read (the log).
    const wchar_t *const kDataDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)";
    // Pairing keys: Windows and administrators only.
    const wchar_t *const kPairingDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  }  // namespace

  bool prepare_data_dirs() {
    if (!is_elevated()) {
      return false;
    }
    const std::string dir = data_dir();
    const bool ok = secure_directory(dir, kDataDirSddl) && secure_directory(dir + "\\pairing", kPairingDirSddl);
    if (!ok) {
      log::warn("could not set the permissions of ", dir, " (error ", GetLastError(), ")");
    }
    return ok;
  }

  bool service_installed() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
      return false;
    }
    const std::wstring name = widen(kServiceName);
    SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_QUERY_STATUS);
    if (service != nullptr) {
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return service != nullptr;
  }

  namespace {
    std::function<int()> g_serve;
    std::function<void()> g_stop;
    SERVICE_STATUS_HANDLE g_status_handle = nullptr;
    SERVICE_STATUS g_status {};

    void report_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
      g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
      g_status.dwCurrentState = state;
      g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
      g_status.dwWin32ExitCode = exit_code == NO_ERROR ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
      g_status.dwServiceSpecificExitCode = exit_code;
      g_status.dwWaitHint = wait_hint;
      SetServiceStatus(g_status_handle, &g_status);
    }

    DWORD WINAPI service_control(DWORD control, DWORD event_type, LPVOID event_data, LPVOID) {
      switch (control) {
        case SERVICE_CONTROL_SESSIONCHANGE:
          if (event_data != nullptr) {
            tray::session_changed(event_type, static_cast<const WTSSESSION_NOTIFICATION *>(event_data)->dwSessionId);
          }
          return NO_ERROR;
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
          report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
          g_stop();
          return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
          return NO_ERROR;
        default:
          return ERROR_CALL_NOT_IMPLEMENTED;
      }
    }

    /** Restart after a crash or a failed start, so the PC stays reachable. */
    void configure_recovery() {
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (manager == nullptr) {
        return;
      }
      const std::wstring name = widen(kServiceName);
      SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START);
      if (service != nullptr) {
        SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 60000}};
        SERVICE_FAILURE_ACTIONSW failure {};
        failure.dwResetPeriod = 24 * 60 * 60;
        failure.cActions = 3;
        failure.lpsaActions = actions;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
        SERVICE_FAILURE_ACTIONS_FLAG flag {};
        flag.fFailureActionsOnNonCrashFailures = TRUE;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
        CloseServiceHandle(service);
      }
      CloseServiceHandle(manager);
    }

    void WINAPI service_main(DWORD, LPWSTR *) {
      const std::wstring name = widen(kServiceName);
      g_status_handle = RegisterServiceCtrlHandlerExW(name.c_str(), service_control, nullptr);
      if (g_status_handle == nullptr) {
        return;
      }
      report_status(SERVICE_START_PENDING, NO_ERROR, 5000);
      configure_recovery();
      report_status(SERVICE_RUNNING);
      const int code = g_serve();
      report_status(SERVICE_STOPPED, static_cast<DWORD>(code));
    }
  }  // namespace

  int run_service(const std::function<int()> &serve, const std::function<void()> &stop) {
    g_serve = serve;
    g_stop = stop;
    g_service_mode = true;
    std::wstring name = widen(kServiceName);
    SERVICE_TABLE_ENTRYW table[] = {{name.data(), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
      g_service_mode = false;
      if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::fprintf(stderr, "'inputline-host service' is started by Windows (the InputLine installer sets it up). In a terminal, use 'inputline-host run'.\n");
      }
      return 1;
    }
    return 0;
  }

  int install(const InstallOptions &options) {
    if (service_installed()) {
      std::fprintf(stderr, "InputLine is installed with its installer and already runs in the background. Nothing to do.\n");
      return 1;
    }
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'inputline-host install' from an administrator terminal (Terminal (Admin)).\n");
      return 1;
    }

    prepare_data_dirs();
    const std::string log_dir = data_dir();
    std::string arguments = "run --hide-console --log " + arg_quote(log_dir + "\\inputline-host.log");
    for (const auto &arg : options.run_arguments) {
      arguments += " " + arg_quote(arg);
    }

    std::string script = kCommonScript;
    script += "$source = " + ps_quote(executable_path()) + "\n";
    script += "$arguments = " + ps_quote(arguments) + "\n";
    script += "$port = " + std::to_string(options.port) + "\n";
    script +=
      "$user = \"$env:USERDOMAIN\\$env:USERNAME\"\n"
      "Stop-Installed\n"
      "New-Item -ItemType Directory -Force -Path $dir | Out-Null\n"
      "if ((Resolve-Path $source).Path -ne $exe) { Copy-Item -Force $source $exe }\n"
      "Write-Host \"Installed to $exe\"\n"
      // Only the local network and Tailscale can reach the link port.
      "Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule\n"
      "New-NetFirewallRule -DisplayName $name -Direction Inbound -Protocol UDP -LocalPort $port -Program $exe "
      "-RemoteAddress LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48 -Profile Any -Action Allow | Out-Null\n"
      "Write-Host \"Firewall: UDP $port allowed from the local network and Tailscale\"\n"
      "$action = New-ScheduledTaskAction -Execute $exe -Argument $arguments -WorkingDirectory $dir\n"
      "$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user\n"
      "$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest\n"
      "$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries "
      "-DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew\n"
      "Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null\n"
      "Start-ScheduledTask -TaskName $name\n"
      "Write-Host \"Started. It will start by itself whenever $user logs on.\"\n";

    const int code = run_powershell(script);
    if (code != 0) {
      std::fprintf(stderr, "Install failed (PowerShell exit %d).\n", code);
      return 1;
    }
    std::printf(
      "\nAll set. Nothing else to run on this PC.\n"
      "To pair an iPad or iPhone: open InputLine on it, enter this PC's address and tap Connect.\n"
      "A 6-digit code then pops up on this PC's screen; type it into InputLine.\n"
      "Log: %s\\inputline-host.log\n",
      log_dir.c_str()
    );
    return 0;
  }

  int uninstall() {
    if (service_installed()) {
      std::fprintf(stderr, "InputLine was installed with its installer: remove it in Settings > Apps > Installed apps.\n");
      return 1;
    }
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'inputline-host uninstall' from an administrator terminal (Terminal (Admin)).\n");
      return 1;
    }
    std::string script = kCommonScript;
    script +=
      "Stop-Installed\n"
      "Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue\n"
      "Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule\n"
      "Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue\n"
      "Write-Host 'Removed the logon task, the firewall rule and the installed copy. Paired devices are kept.'\n";
    return run_powershell(script) == 0 ? 0 : 1;
  }

#else

  namespace {
    namespace fs = std::filesystem;

    constexpr const char *kDataDir = "/var/lib/inputline";
    constexpr const char *kConfigDir = "/etc/inputline";
    constexpr const char *kInstalledBinary = "/usr/local/bin/inputline-host";
    constexpr const char *kUnitPath = "/etc/systemd/system/inputline.service";
    constexpr const char *kModulesLoadPath = "/etc/modules-load.d/inputline.conf";
    constexpr const char *kUdevRulePath = "/etc/udev/rules.d/70-inputline.rules";
    constexpr const char *kServiceUnit = "inputline";
    /** Where InputLine apps reach this PC from: private networks, Tailscale, IPv6 local addresses. */
    const char *const kAllowedSources[] = {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10", "fc00::/7", "fe80::/10"};
    constexpr unsigned short kMdnsPort = 5353;

    bool write_file(const std::string &path, const std::string &content, fs::perms perms) {
      std::error_code error;
      fs::create_directories(fs::path(path).parent_path(), error);
      std::ofstream out(path, std::ios::trunc);
      out << content;
      out.close();
      if (!out) {
        std::fprintf(stderr, "  cannot write %s\n", path.c_str());
        return false;
      }
      fs::permissions(path, perms, error);
      return true;
    }

    /** Run a system tool by name; -1 if it isn't installed. */
    int run_tool(std::vector<std::string> argv, std::string *output = nullptr) {
      const std::string path = find_program(argv[0]);
      if (path.empty()) {
        return -1;
      }
      argv[0] = path;
      return run_process(argv, output);
    }

    /** Signed-in users with a desktop session bus: (uid, user name). */
    std::vector<std::pair<uid_t, std::string>> session_users() {
      std::vector<std::pair<uid_t, std::string>> users;
      std::error_code error;
      for (const auto &entry : fs::directory_iterator("/run/user", error)) {
        const std::string name = entry.path().filename().string();
        char *end = nullptr;
        const unsigned long uid = std::strtoul(name.c_str(), &end, 10);
        if (end == name.c_str() || *end != '\0' || uid < 1000 || !fs::exists(entry.path() / "bus", error)) {
          continue;
        }
        if (const passwd *user = ::getpwuid(static_cast<uid_t>(uid))) {
          users.emplace_back(static_cast<uid_t>(uid), user->pw_name);
        }
      }
      return users;
    }

    /**
     * A desktop notification (freedesktop notify-send) on every signed-in
     * user's screen. The service runs as root, so it asks for each user's
     * session bus. Never blocks the caller.
     */
    void notify(const std::string &title, const std::string &body, bool urgent, int timeout_ms) {
      std::thread([title, body, urgent, timeout_ms] {
        const std::string notify_send = find_program("notify-send");
        if (notify_send.empty()) {
          static std::atomic<bool> warned {false};
          if (!warned.exchange(true)) {
            log::warn("notify-send isn't installed, so notifications (like pairing codes) only go to the log. Install it: ",
                      install_hint(Package::kNotifications));
          }
          return;
        }
        const std::vector<std::string> notification {
          notify_send, "--app-name=InputLine", "--icon=input-gaming", urgent ? "--urgency=critical" : "--urgency=normal",
          "--expire-time=" + std::to_string(timeout_ms), title, body,
        };
        if (::geteuid() != 0) {
          run_process(notification);
          return;
        }
        const std::string runuser = find_program("runuser");
        for (const auto &[uid, name] : session_users()) {
          if (runuser.empty()) {
            break;
          }
          std::vector<std::string> argv {runuser, "-u", name, "--", "env", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + std::to_string(uid) + "/bus"};
          argv.insert(argv.end(), notification.begin(), notification.end());
          std::string output;
          if (run_process(argv, &output) != 0) {
            log::debug("notification for ", name, " failed: ", output);
          }
        }
      }).detach();
    }

    std::string os_release_id() {
      std::ifstream in("/etc/os-release");
      std::string line;
      while (std::getline(in, line)) {
        if (line.rfind("ID=", 0) == 0) {
          std::string id = line.substr(3);
          id.erase(std::remove(id.begin(), id.end(), '"'), id.end());
          return id;
        }
      }
      return {};
    }

    enum class Firewall {
      kNone,
      kUfw,
      kFirewalld,
    };

    Firewall active_firewall() {
      std::string output;
      if (run_tool({"ufw", "status"}, &output) == 0 && output.find("Status: active") != std::string::npos) {
        return Firewall::kUfw;
      }
      if (run_tool({"firewall-cmd", "--state"}) == 0) {
        return Firewall::kFirewalld;
      }
      return Firewall::kNone;
    }

    /** Allow (or stop allowing) the link port, from the local network and Tailscale only. */
    void change_firewall(unsigned short port, bool allow) {
      const std::string link_port = std::to_string(port);
      switch (active_firewall()) {
        case Firewall::kUfw:
          // ufw already lets mDNS (discovery) in by default.
          for (const char *source : kAllowedSources) {
            if (allow) {
              run_tool({"ufw", "allow", "from", source, "to", "any", "port", link_port, "proto", "udp", "comment", "InputLine"});
            } else {
              run_tool({"ufw", "delete", "allow", "from", source, "to", "any", "port", link_port, "proto", "udp"});
            }
          }
          std::printf("  %s ufw: UDP %s from the local network and Tailscale\n", allow ? "Allowed in" : "Removed from", link_port.c_str());
          break;
        case Firewall::kFirewalld:
          for (const char *source : kAllowedSources) {
            const std::string family = std::string(source).find(':') != std::string::npos ? "ipv6" : "ipv4";
            for (const unsigned short each : {port, kMdnsPort}) {
              const std::string rule = "rule family=\"" + family + "\" source address=\"" + source + "\" port port=\"" + std::to_string(each) +
                                       "\" protocol=\"udp\" accept";
              run_tool({"firewall-cmd", "--permanent", (allow ? "--add-rich-rule=" : "--remove-rich-rule=") + rule});
            }
          }
          run_tool({"firewall-cmd", "--reload"});
          std::printf("  %s firewalld: UDP %s and mDNS from the local network and Tailscale\n", allow ? "Allowed in" : "Removed from",
                      link_port.c_str());
          break;
        case Firewall::kNone:
          if (allow) {
            std::printf("  No active ufw or firewalld found. If another firewall runs, allow UDP %s from the local network.\n", link_port.c_str());
          }
          break;
      }
    }

    void check_line(bool ok, const std::string &what, const std::string &fix) {
      std::printf("  [%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), ok ? "" : ": ", ok ? "" : fix.c_str());
    }
  }  // namespace

  std::string install_hint(Package package) {
    const bool pacman = !find_program("pacman").empty();
    const bool apt = !find_program("apt-get").empty();
    const bool dnf = !find_program("dnf").empty();
    switch (package) {
      case Package::kUsbip:
        return pacman ? "sudo pacman -S --needed usbip"
               : apt  ? (os_release_id() == "ubuntu" ? "sudo apt install linux-tools-generic" : "sudo apt install usbip")
               : dnf  ? "sudo dnf install usbip"
                      : "install the usbip tool with your package manager";
      case Package::kAvahi:
        return pacman ? "sudo pacman -S --needed avahi && sudo systemctl enable --now avahi-daemon"
               : apt  ? "sudo apt install avahi-daemon avahi-utils"
               : dnf  ? "sudo dnf install avahi avahi-tools && sudo systemctl enable --now avahi-daemon"
                      : "install Avahi (avahi-daemon and avahi-publish) with your package manager";
      case Package::kNotifications:
        return pacman ? "sudo pacman -S --needed libnotify"
               : apt  ? "sudo apt install libnotify-bin"
               : dnf  ? "sudo dnf install libnotify"
                      : "install notify-send (libnotify) with your package manager";
    }
    return {};
  }

  void show_pairing_code(const std::string &client_name, const std::string &code) {
    const std::string spaced = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
    notify("InputLine pairing code: " + spaced, "Type it into InputLine on " + client_name + ". It works for 2 minutes.", true, 120000);
  }

  bool show_notification(const std::string &title, const std::string &text, const std::string &url) {
    notify(title, url.empty() ? text : text + "\n" + url, false, 20000);
    return true;
  }

  int run_notifier() {
    return 1;
  }

  void hide_console() {}

  bool is_elevated() {
    return ::geteuid() == 0;
  }

  std::string data_dir() {
    return kDataDir;
  }

  bool prepare_data_dirs() {
    if (::geteuid() != 0) {
      return false;
    }
    // Readable by users (status, for 'inputline-host status'); the pairing
    // keys in 'pairing' by root only.
    std::error_code error;
    fs::create_directories(fs::path(kDataDir) / "pairing", error);
    fs::permissions(kDataDir, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    error);
    fs::permissions(fs::path(kDataDir) / "pairing", fs::perms::owner_all, error);
    return !error;
  }

  bool service_installed() {
    std::error_code error;
    return fs::exists(kUnitPath, error);
  }

  int run_service(const std::function<int()> &serve, const std::function<void()> &) {
    return serve();  // systemd stops it with SIGTERM
  }

  int install(const InstallOptions &options) {
    if (::geteuid() != 0) {
      std::fprintf(stderr, "Run it as root: sudo inputline-host install\n");
      return 1;
    }
    std::printf("Installing InputLine as a background service\n");
    std::error_code error;

    // The program, where the service runs it from. Renamed into place, so
    // this also updates a copy that is running.
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (!error && self != fs::path(kInstalledBinary)) {
      const std::string temporary = std::string(kInstalledBinary) + ".new";
      fs::create_directories(fs::path(kInstalledBinary).parent_path(), error);
      if (fs::copy_file(self, temporary, fs::copy_options::overwrite_existing, error)) {
        fs::rename(temporary, kInstalledBinary, error);
      }
      if (error) {
        std::fprintf(stderr, "  cannot copy the program to %s: %s\n", kInstalledBinary, error.message().c_str());
        return 1;
      }
      fs::permissions(kInstalledBinary, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                      error);
    }
    std::printf("  Program: %s\n", kInstalledBinary);

    // Settings and data.
    const std::string options_file = options_path();
    if (!fs::exists(options_file, error)) {
      std::string content =
        "# Options for the InputLine service, written as on the command line\n"
        "# (see 'inputline-host --help'). After a change: sudo systemctl restart inputline\n"
        "#\n"
        "# Log report timing every 10 s:\n"
        "# --stats\n";
      if (!options.run_arguments.empty()) {
        std::string line;
        for (const auto &argument : options.run_arguments) {
          line += (line.empty() ? "" : " ") + argument;
        }
        content += "\n" + line + "\n";
      }
      write_file(options_file, content, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
    }
    std::printf("  Options: %s\n", options_file.c_str());
    prepare_data_dirs();

    // The USB/IP virtual host controller the controller plugs into, now and at every boot.
    write_file(kModulesLoadPath, "# InputLine: the USB/IP virtual host controller its virtual Steam Controller plugs into\nvhci-hcd\n",
               fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
    run_tool({"modprobe", "vhci-hcd"});

    // Let the signed-in user's Steam open the virtual controller.
    write_file(kUdevRulePath,
               "# InputLine's virtual Steam Controller: Steam, running as the signed-in user, opens it\n"
               "KERNEL==\"hidraw*\", ATTRS{idVendor}==\"28de\", ATTRS{idProduct}==\"1302\", MODE=\"0660\", TAG+=\"uaccess\"\n"
               "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"28de\", ATTRS{idProduct}==\"1302\", MODE=\"0660\", TAG+=\"uaccess\"\n",
               fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
    run_tool({"udevadm", "control", "--reload-rules"});

    change_firewall(options.port, true);
    {
      std::ofstream port_file((fs::path(kDataDir) / "firewall-port").string(), std::ios::trunc);
      port_file << options.port << "\n";
    }

    // The service: starts with the system, restarts if it stops.
    const std::string modprobe = find_program("modprobe");
    std::string unit =
      "[Unit]\n"
      "Description=InputLine: Steam Controller from an iPad or iPhone, with full Steam Input\n"
      "Documentation=" + std::string(update::kRepositoryUrl) + "\n"
      "Wants=network-online.target\n"
      "After=network-online.target avahi-daemon.service\n"
      "\n"
      "[Service]\n"
      "Type=simple\n";
    if (!modprobe.empty()) {
      unit += "ExecStartPre=-" + modprobe + " vhci-hcd\n";
    }
    unit +=
      "ExecStart=" + std::string(kInstalledBinary) + " service\n"
      "Restart=on-failure\n"
      "RestartSec=3\n"
      "\n"
      "[Install]\n"
      "WantedBy=multi-user.target\n";
    if (!write_file(kUnitPath, unit, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read)) {
      return 1;
    }
    run_tool({"systemctl", "daemon-reload"});
    run_tool({"systemctl", "enable", kServiceUnit});
    const int started = run_tool({"systemctl", "restart", kServiceUnit});
    std::printf("  Service: %s, starts with the system\n", started == 0 ? "running" : "installed, but it did not start (journalctl -u inputline)");

    // What else it needs.
    std::printf("\nChecks:\n");
    const UsbipCheck usbip = check_usbip({});
    check_line(usbip.state != UsbipCheck::State::kMissing, "usbip (plugs the virtual controller in)", install_hint(Package::kUsbip));
    check_line(usbip.state != UsbipCheck::State::kNoDriver, "vhci-hcd kernel module",
               "your kernel doesn't have it; InputLine can't plug controllers in without it");
    const bool avahi = !find_program("avahi-publish").empty() && run_tool({"systemctl", "is-active", "--quiet", "avahi-daemon"}) == 0;
    check_line(avahi, "Avahi (InputLine finds this PC on the network)", install_hint(Package::kAvahi) + ", or type this PC's address in InputLine");
    check_line(!find_program("notify-send").empty(), "notify-send (shows the pairing code)",
               install_hint(Package::kNotifications) + "; the code is also in: journalctl -u inputline");
    std::printf("\nDone. Open InputLine on your iPad or iPhone and tap this PC; the pairing code pops up here.\n"
                "Status: inputline-host status    Log: journalctl -u inputline -f\n");
    return 0;
  }

  int uninstall() {
    if (::geteuid() != 0) {
      std::fprintf(stderr, "Run it as root: sudo inputline-host uninstall\n");
      return 1;
    }
    std::error_code error;
    run_tool({"systemctl", "disable", "--now", kServiceUnit});
    fs::remove(kUnitPath, error);
    run_tool({"systemctl", "daemon-reload"});
    std::printf("Removed the InputLine service\n");

    unsigned short port = 48150;
    {
      std::ifstream port_file((fs::path(kDataDir) / "firewall-port").string());
      unsigned int value = 0;
      if (port_file >> value && value > 0 && value <= 65535) {
        port = static_cast<unsigned short>(value);
      }
    }
    change_firewall(port, false);
    fs::remove(fs::path(kDataDir) / "firewall-port", error);
    fs::remove(kModulesLoadPath, error);
    fs::remove(kUdevRulePath, error);
    run_tool({"udevadm", "control", "--reload-rules"});
    fs::remove(kInstalledBinary, error);
    std::printf("Removed %s\n", kInstalledBinary);
    std::printf("Kept your paired devices in %s and your options in %s; delete them to remove those too.\n", kDataDir, kConfigDir);
    return 0;
  }

#endif

  std::string data_file(const std::string &name) {
    const std::string dir = data_dir();
    return dir.empty() ? std::string() : (std::filesystem::path(dir) / name).string();
  }

  std::string options_path() {
#ifdef _WIN32
    return data_file("options.txt");
#else
    return "/etc/inputline/options.txt";
#endif
  }

}  // namespace inputline::desktop
