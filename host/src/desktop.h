/**
 * @file desktop.h
 * @brief The parts of inputline-host that touch the user's desktop: the pairing
 *        code popup, running in the background (a Windows service, or a
 *        systemd service on Linux), and where it keeps its files.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace inputline::desktop {

  /** Name of the Windows service the installer sets up. */
  inline constexpr const char *kServiceName = "InputLine";

  /**
   * @brief Put a pairing code on the PC's screen without blocking the caller.
   *
   * The user types it into the InputLine app on the iPad or iPhone. It shows as
   * a Windows notification (from the service, on the signed-in user's screen),
   * or in a message box if that fails.
   */
  void show_pairing_code(const std::string &client_name, const std::string &code);

  /**
   * @brief A Windows notification from InputLine (from the service, on the
   *        signed-in user's screen). Clicking it opens @p url, if given.
   * @return false if it could not be shown.
   */
  bool show_notification(const std::string &title, const std::string &text, const std::string &url = {});

  /**
   * @brief The hidden 'notify TITLE TEXT SECONDS [URL]' command: show a Windows
   *        notification with the InputLine tray icon, and keep the icon for
   *        SECONDS. show_pairing_code() starts it in the user's session.
   * @return Process exit code.
   */
  int run_notifier();

  /** Detach from the console window (for the logon task). */
  void hide_console();

  /** Whether the process runs with administrator rights. */
  bool is_elevated();

  /**
   * @brief Where the background copy keeps its files: C:\ProgramData\InputLine
   *        on Windows (the log, options.txt, and pairings in its 'pairing'
   *        folder), /var/lib/inputline on Linux (status, and pairings in
   *        'pairing'; the log goes to the journal).
   */
  std::string data_dir();

  /** A file in data_dir(), or empty if there is none. */
  std::string data_file(const std::string &name);

  /** The service's options file: data_dir()\options.txt on Windows, /etc/inputline/options.txt on Linux. */
  std::string options_path();

  /**
   * @brief Create data_dir() with safe permissions: only administrators and
   *        Windows can change it, users can read the log, and the pairing
   *        folder is readable by administrators and Windows only. Needs
   *        administrator rights. No-op elsewhere.
   */
  bool prepare_data_dirs();

  /** Whether the installer's InputLine service exists. */
  bool service_installed();

  /**
   * @brief Run as the Windows service: serve() runs until stop() makes it
   *        return, which happens when Windows stops the service.
   * @return Process exit code; 1 when not started by Windows.
   */
  int run_service(const std::function<int()> &serve, const std::function<void()> &stop);

  struct InstallOptions {
    /** Extra arguments for the installed 'run' command (port, config...). */
    std::vector<std::string> run_arguments;
    unsigned short port = 0;
  };

  /**
   * @brief Windows, without the installer: copy this executable to Program
   *        Files, allow the link port through the firewall for the local
   *        network and Tailscale, and start it at every logon with
   *        administrator rights.
   *
   * Linux (as root): copy it to /usr/local/bin, set it up as the 'inputline'
   * systemd service, load vhci-hcd now and at boot, add a udev rule so Steam
   * can open the virtual controller, and allow the port in ufw or firewalld.
   * @return Process exit code.
   */
  int install(const InstallOptions &options);

  /** Undo install(). Paired devices are kept. */
  int uninstall();

#ifndef _WIN32
  enum class Package {
    kUsbip,
    kAvahi,
    kNotifications,
  };

  /** How to install @p package on this distribution (pacman, apt or dnf). */
  std::string install_hint(Package package);
#endif

#ifdef _WIN32
  /** UTF-8 to UTF-16 and back. */
  std::wstring widen(const std::string &text);
  std::string narrow(const std::wstring &text);

  /** This program's full path. */
  std::string executable_path();

  /** One argument, quoted for a Windows command line if needed. */
  std::string arg_quote(const std::string &arg);

  /** Whether this process is the InputLine service. */
  bool service_mode();
#endif

}  // namespace inputline::desktop
