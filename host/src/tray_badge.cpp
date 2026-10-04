#include "tray_badge.h"

#include <algorithm>
#include <cmath>

namespace inputline::tray {

  void draw_badge(std::uint32_t *pixels, int size, Badge badge) {
    // A coloured circle with a white ring and a symbol, top left, drawn
    // with 4x4 samples per pixel so it stays readable at 16 pixels.
    const double radius = size * (size < 20 ? 0.23 : 0.19);
    const double ring = radius + std::max(0.9, size / 22.0);
    const double arm = radius * 0.52;
    const double half_stroke = std::max(0.55, size / 38.0);
    // The play triangle, nudged right so it looks centred.
    const double play_left = -radius * 0.36;
    const double play_right = radius * 0.62;
    const double play_half_height = radius * 0.58;
    const std::uint32_t colour = badge == Badge::kProblem ? 0xE53935 : 0x2E9E44;
    // The app badge swaps them: a white circle with a coloured symbol.
    const bool inverted = badge == Badge::kApp;
    // 0: outside, 1: white, 2: colour
    auto sample = [&](double x, double y) {
      const double dx = x - ring;
      const double dy = y - ring;
      const double distance = std::sqrt(dx * dx + dy * dy);
      if (distance > ring) {
        return 0;
      }
      if (distance > radius) {
        return 1;
      }
      bool symbol = false;
      if (badge == Badge::kProblem) {
        const double to_diagonal = std::min(std::abs(dx - dy), std::abs(dx + dy)) / std::sqrt(2.0);
        symbol = to_diagonal <= half_stroke && std::max(std::abs(dx), std::abs(dy)) <= arm;
      } else {
        symbol = dx >= play_left && std::abs(dy) <= play_half_height * (play_right - dx) / (play_right - play_left);
      }
      return symbol != inverted ? 1 : 2;
    };
    const int limit = static_cast<int>(std::ceil(2 * ring)) + 1;
    for (int y = 0; y < std::min(size, limit); ++y) {
      for (int x = 0; x < std::min(size, limit); ++x) {
        int covered = 0;
        double red = 0, green = 0, blue = 0;
        for (int sy = 0; sy < 4; ++sy) {
          for (int sx = 0; sx < 4; ++sx) {
            const int kind = sample(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
            if (kind == 0) {
              continue;
            }
            ++covered;
            red += kind == 1 ? 255 : (colour >> 16) & 0xFF;
            green += kind == 1 ? 255 : (colour >> 8) & 0xFF;
            blue += kind == 1 ? 255 : colour & 0xFF;
          }
        }
        if (covered == 0) {
          continue;
        }
        const double coverage = covered / 16.0;
        std::uint32_t &pixel = pixels[y * size + x];
        const double old_alpha = (pixel >> 24) & 0xFF;
        const auto mix = [&](double sum, int shift) {
          return static_cast<std::uint32_t>(sum / covered * coverage + ((pixel >> shift) & 0xFF) * (1 - coverage)) & 0xFF;
        };
        const auto alpha = static_cast<std::uint32_t>(std::max(old_alpha, 255 * coverage));
        pixel = (alpha << 24) | (mix(red, 16) << 16) | (mix(green, 8) << 8) | mix(blue, 0);
      }
    }
  }

}  // namespace inputline::tray
