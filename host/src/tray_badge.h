/**
 * @file tray_badge.h
 * @brief The small round badge on the tray icon (Windows and Linux): a red X
 *        when something needs the user, a green play when the app is
 *        connected or a controller is plugged in.
 */
#pragma once

#include <cstdint>

namespace inputline::tray {

  enum class Badge {
    kProblem,     ///< white X on red
    kApp,         ///< green play on white: the app is connected
    kController,  ///< white play on green: a controller is plugged in
  };

  /**
   * @brief Draw @p badge in the top left corner of a square image.
   * @param pixels size × size pixels, 0xAARRGGBB, top row first.
   */
  void draw_badge(std::uint32_t *pixels, int size, Badge badge);

}  // namespace inputline::tray
