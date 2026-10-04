/**
 * @file tray.h
 * @brief The InputLine icon in the taskbar's notification area (Windows), or
 *        in the desktop's tray on Linux (StatusNotifierItem; tray_linux.cpp).
 *
 * The service runs in the background without a desktop, so it starts a small
 * helper, 'inputline-host tray', as the signed-in user. The helper shows the
 * service's status (from the status file), the pairing and update
 * notifications, and a menu. When the service stops it signals the helpers
 * to quit and waits for them, so an upgrade can replace inputline-host.exe.
 *
 * On Linux the service starts the icon for users signed in at the time, and an
 * XDG autostart entry at later sign-ins. Other platforms get harmless stubs.
 */
#pragma once

#include <string>

namespace inputline::tray {

  /** Service: start the icon for the signed-in user now, and at every later sign-in. */
  void start_agents();

  /** Service: a Windows session changed (SERVICE_CONTROL_SESSIONCHANGE). */
  void session_changed(unsigned long event, unsigned long session_id);

  /** Service: close the icons and other helpers, and wait until they are gone. */
  void stop_agents();

  /**
   * @brief Service: run 'inputline-host <arguments>' as the user signed in at
   *        the PC, in their session. stop_agents() waits for it too.
   * @return false if nobody is signed in or it could not start.
   */
  bool launch_in_user_session(const std::string &arguments);

  /**
   * @brief Run this thread's window messages until PostQuitMessage, or until
   *        the service asks its helpers to quit.
   */
  void run_message_loop();

  /**
   * @brief The hidden 'tray' command: the icon itself. @p show brings it back
   *        after the user chose "Hide this icon" (the Start menu shortcut).
   * @return Process exit code.
   */
  int run_tray(bool show);

  /**
   * @brief Show a notification on the icon of a tray running in this session.
   * @param url Opened when the notification is clicked; empty shows @p text again instead.
   * @return false if no tray runs here.
   */
  bool forward_notification(const std::wstring &title, const std::wstring &text, const std::wstring &url);

}  // namespace inputline::tray
