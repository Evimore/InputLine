#include "usbip_attach.h"

#include "log.h"
#include "inputline/version.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
extern char **environ;
#endif

namespace inputline {

  std::string default_usbip_executable() {
#ifdef _WIN32
    for (const char *candidate : {R"(C:\Program Files\USBip\usbip.exe)", R"(C:\Program Files\usbip-win2\usbip.exe)"}) {
      std::error_code error;
      if (std::filesystem::exists(candidate, error)) {
        return candidate;
      }
    }
    return "usbip.exe";
#else
    return "usbip";
#endif
  }

#ifdef _WIN32
  namespace {
    std::wstring widen_path(const std::string &text);
  }

  UsbipCheck check_usbip(const std::string &executable) {
    UsbipCheck check;
    std::wstring path = widen_path(executable.empty() ? default_usbip_executable() : executable);
    std::error_code error;
    if (!std::filesystem::exists(std::filesystem::path(path), error)) {
      // Not where usbip-win2 installs it: maybe on the PATH.
      wchar_t found[MAX_PATH] = {};
      if (SearchPathW(nullptr, path.c_str(), nullptr, MAX_PATH, found, nullptr) == 0) {
        check.state = UsbipCheck::State::kMissing;
        return check;
      }
      path = found;
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<std::uint8_t> info(size);
    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT fixed_size = 0;
    if (size != 0 && GetFileVersionInfoW(path.c_str(), 0, size, info.data()) &&
        VerQueryValueW(info.data(), L"\\", reinterpret_cast<void **>(&fixed), &fixed_size) && fixed != nullptr) {
      check.version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
                      std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
      if (compare_versions(check.version, kMinUsbipVersion) < 0) {
        check.state = UsbipCheck::State::kTooOld;
      }
    }
    return check;
  }
#else
  std::string find_program(const std::string &program) {
    if (program.find('/') != std::string::npos) {
      return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    }
    // A service's PATH can be short: also look where distributions put it.
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = std::min(path.find(':', start), path.size());
      const std::string dir = path.substr(start, end - start);
      if (!dir.empty()) {
        const std::string candidate = dir + "/" + program;
        if (::access(candidate.c_str(), X_OK) == 0) {
          return candidate;
        }
      }
      start = end + 1;
    }
    return {};
  }

  UsbipCheck check_usbip(const std::string &executable) {
    UsbipCheck check;
    if (find_program(executable.empty() ? default_usbip_executable() : executable).empty()) {
      check.state = UsbipCheck::State::kMissing;
      return check;
    }
    std::error_code error;
    if (!std::filesystem::exists("/sys/devices/platform/vhci_hcd.0", error) &&
        !std::filesystem::exists("/sys/bus/platform/drivers/vhci_hcd", error)) {
      check.state = UsbipCheck::State::kNoDriver;
    }
    return check;
  }
#endif

  std::vector<std::string> attach_command(const AttachOptions &options, const std::string &busid, bool with_extras) {
    std::vector<std::string> argv {
      options.executable.empty() ? default_usbip_executable() : options.executable,
      "--tcp-port",
      std::to_string(options.port),
      "attach",
      "-r",
      options.host,
      "-b",
      busid,
    };
#ifdef _WIN32
    if (with_extras) {
      // usbip-win2 0.9.8+: fail fast instead of retrying forever, and favour
      // latency over throughput for this tiny interrupt stream.
      argv.insert(argv.end(), {"--once", "--receive-mode", "low-latency"});
    }
#else
    (void) with_extras;
#endif
    return argv;
  }

  bool run_usbip_attach(const AttachOptions &options, const std::string &busid) {
    if (!options.enabled) {
      log::info("attach: automatic attach disabled; attach ", busid, " yourself");
      return true;
    }

    auto run = [&busid](const std::vector<std::string> &argv) {
      std::string command;
      for (const auto &arg : argv) {
        command += (command.empty() ? "" : " ") + arg;
      }
      log::debug("attach: running ", command);
      std::string output;
      const int code = run_process(argv, &output);
      // usbip's own words are the best clue when attaching fails.
      while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
      }
      if (!output.empty()) {
        if (code != 0) {
          log::warn("attach: usbip said (exit ", code, "): ", output);
        } else {
          log::debug("attach: usbip said: ", output);
        }
      }
      (void) busid;
      return code;
    };

    auto argv = attach_command(options, busid, options.use_low_latency_mode);
    int code = run(argv);
#ifdef _WIN32
    if (code != 0 && options.use_low_latency_mode) {
      log::info("attach: retrying without newer usbip-win2 options");
      argv = attach_command(options, busid, false);
      code = run(argv);
    }
#endif
    if (code != 0) {
#ifdef _WIN32
      log::error("attach: '", argv[0], "' failed (exit ", code, "). Is usbip-win2 installed, and does InputLine run as administrator?");
#else
      log::error("attach: '", argv[0], "' failed (exit ", code, "). Is usbip installed, is the vhci-hcd module loaded (sudo modprobe vhci-hcd), "
                 "and does InputLine run as root?");
#endif
      return false;
    }
    log::info("attach: plugged in ", busid);
    return true;
  }

#ifdef _WIN32
  namespace {
    std::wstring widen(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
      return out;
    }

    std::wstring widen_path(const std::string &text) {
      return widen(text);
    }

    /** Quote one argument following the MSVC CommandLineToArgvW rules. */
    std::wstring quote(const std::wstring &arg) {
      if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
      }
      std::wstring out = L"\"";
      std::size_t backslashes = 0;
      for (wchar_t c : arg) {
        if (c == L'\\') {
          ++backslashes;
        } else if (c == L'"') {
          out.append(backslashes * 2 + 1, L'\\');
          out.push_back(c);
          backslashes = 0;
        } else {
          out.append(backslashes, L'\\');
          out.push_back(c);
          backslashes = 0;
        }
      }
      out.append(backslashes * 2, L'\\');
      out.push_back(L'"');
      return out;
    }
  }  // namespace

  int run_process(const std::vector<std::string> &argv, std::string *output, std::size_t max_output) {
    if (argv.empty()) {
      return -1;
    }
    std::wstring command_line;
    for (const auto &arg : argv) {
      if (!command_line.empty()) {
        command_line.push_back(L' ');
      }
      command_line += quote(widen(arg));
    }

    // Collect stdout and stderr through one pipe; stdin reads nothing.
    SECURITY_ATTRIBUTES inherit {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    HANDLE nul = INVALID_HANDLE_VALUE;
    const bool capture = output != nullptr && CreatePipe(&read_end, &write_end, &inherit, 0);
    if (capture) {
      SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
      nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    if (capture) {
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = nul;
      startup.hStdOutput = write_end;
      startup.hStdError = write_end;
    }
    PROCESS_INFORMATION process {};
    const BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, capture ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD start_error = GetLastError();
    if (capture) {
      CloseHandle(write_end);  // our copy: reading ends when the child exits
      if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
      }
    }
    if (!started) {
      if (capture) {
        CloseHandle(read_end);
      }
      log::error("attach: cannot start ", argv[0], " (error ", start_error, ")");
      return -1;
    }
    if (capture) {
      char buffer[512];
      DWORD got = 0;
      while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        if (output->size() < max_output) {
          output->append(buffer, std::min(static_cast<std::size_t>(got), max_output - output->size()));
        }
      }
      CloseHandle(read_end);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
  }
#else
  int run_process(const std::vector<std::string> &argv, std::string *output, std::size_t max_output) {
    if (argv.empty()) {
      return -1;
    }
    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const bool capture = output != nullptr && ::pipe(pipe_fds) == 0;
    if (capture) {
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
      raw.push_back(const_cast<char *>(arg.c_str()));
    }
    raw.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, raw[0], capture ? &actions : nullptr, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture) {
      ::close(pipe_fds[1]);
    }
    if (spawned != 0) {
      if (capture) {
        ::close(pipe_fds[0]);
      }
      log::error("attach: cannot start ", argv[0]);
      return -1;
    }
    if (capture) {
      char buffer[512];
      ssize_t got = 0;
      while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (output->size() < max_output) {
          output->append(buffer, std::min(static_cast<std::size_t>(got), max_output - output->size()));
        }
      }
      ::close(pipe_fds[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }
#endif

}  // namespace inputline
