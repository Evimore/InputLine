// Small pieces of inputline-host: the update check's release parsing and the
// status file the tray icon reads.

#include "check.h"
#include "status_file.h"
#include "tray_badge.h"
#include "update_check.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace inputline;

namespace {

  // Shaped like GitHub's answer: nested objects with their own html_url.
  const char *const kReleases = R"json([
    {
      "url": "https://api.github.com/repos/Evimore/InputLine/releases/3",
      "html_url": "https://github.com/Evimore/InputLine/releases/tag/v0.4.0-beta.1",
      "author": {"login": "Evimore", "html_url": "https://github.com/Evimore"},
      "tag_name": "v0.4.0-beta.1",
      "draft": false,
      "prerelease": true,
      "assets": [{"name": "x.msi", "size": 123, "uploader": {"html_url": "https://example.com"}}],
      "body": "Fixes \"quotes\", a tab\t and é 😀"
    },
    {
      "html_url": "https://github.com/Evimore/InputLine/releases/tag/v0.3.0",
      "tag_name": "v0.3.0", "draft": false, "prerelease": false, "reactions": null, "id": 12.5e3
    },
    {"tag_name": "v9.9.9", "draft": true, "prerelease": false, "html_url": "https://github.com/Evimore/InputLine/releases/tag/v9.9.9"},
    {"tag_name": "not-a-version", "draft": false, "prerelease": false},
    {"tag_name": "v0.3.1", "draft": false, "prerelease": false, "html_url": "https://evil.example/download"}
  ])json";

  void test_parse_releases() {
    const auto releases = update::parse_releases(kReleases);
    CHECK(releases.size() == 3);  // the draft and the non-version are skipped
    if (releases.size() == 3) {
      CHECK(releases[0].version == "0.4.0-beta.1" && releases[0].prerelease);
      CHECK(releases[0].url == "https://github.com/Evimore/InputLine/releases/tag/v0.4.0-beta.1");
      CHECK(releases[1].version == "0.3.0" && !releases[1].prerelease);
      // Never a link outside InputLine's releases.
      CHECK(releases[2].url == "https://github.com/Evimore/InputLine/releases");
    }
    CHECK(update::parse_releases("").empty());
    CHECK(update::parse_releases("{}").empty());
    CHECK(update::parse_releases("[").empty());
    CHECK(update::parse_releases("[{\"tag_name\": \"v1.0.0\"").empty());
    CHECK(update::parse_releases(std::string(100, '[')).empty());  // too deep
    CHECK(update::parse_releases("[]").empty());
  }

  void test_newer_release() {
    const auto releases = update::parse_releases(kReleases);
    const auto stable = update::newer_release(releases, "0.2.0");
    CHECK(stable && stable->version == "0.3.1");  // releases only
    const auto beta = update::newer_release(releases, "0.3.0-beta.2");
    CHECK(beta && beta->version == "0.4.0-beta.1");  // beta testers hear about betas
    CHECK(!update::newer_release(releases, "0.3.1"));
    CHECK(!update::newer_release(releases, "1.0.0"));
  }

  void test_status_file() {
    ServiceStatus status;
    status.version = "0.3.0";
    status.time = 1'800'000'000;
    status.controllers = 2;
    status.devices = {"Evi's iPad", "Line\nbreak"};
    status.update_version = "0.3.1";
    status.update_url = "https://github.com/Evimore/InputLine/releases/tag/v0.3.1";
    status.usbip = "old";
    status.usbip_version = "0.9.6.0";
    const auto parsed = parse_status(format_status(status));
    CHECK(parsed.usbip == "old" && parsed.usbip_version == "0.9.6.0");
    CHECK(parse_status("version=1\n").usbip == "ok");  // older services didn't say
    CHECK(parsed.version == "0.3.0" && parsed.time == 1'800'000'000 && parsed.controllers == 2);
    CHECK(parsed.devices.size() == 2 && parsed.devices[0] == "Evi's iPad" && parsed.devices[1] == "Line break");
    CHECK(parsed.update_version == "0.3.1" && parsed.update_url == status.update_url);

    const auto path = (std::filesystem::temp_directory_path() / ("inputline-status-" + std::to_string(std::random_device {}()) + ".txt")).string();
    CHECK(write_status_file(path, status));
    const auto read = read_status_file(path);
    CHECK(read && read->devices.size() == 2);
    status.devices.clear();
    CHECK(write_status_file(path, status));  // replaces the old one
    const auto again = read_status_file(path);
    CHECK(again && again->devices.empty());
    std::filesystem::remove(path);
    CHECK(!read_status_file(path));
  }

}  // namespace

namespace {

  void test_badges() {
    using inputline::tray::Badge;
    using inputline::tray::draw_badge;
    for (const int size : {16, 24, 32, 64}) {
      for (const Badge badge : {Badge::kProblem, Badge::kApp, Badge::kController}) {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size * size), 0);
        draw_badge(pixels.data(), size, badge);
        // The badge is opaque in the top left; the opposite corner is untouched.
        const int radius = static_cast<int>(size * (size < 20 ? 0.23 : 0.19));
        const std::uint32_t edge = pixels[static_cast<std::size_t>(radius / 4 + 1) * size + radius + 1];
        CHECK((edge >> 24) == 0xFF);
        CHECK(pixels.back() == 0);
        // Its colour: red for a problem, green otherwise (white app badge with a green symbol).
        bool red = false;
        bool green = false;
        for (const auto pixel : pixels) {
          const int r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
          red = red || (r > 200 && g < 100 && b < 100);
          green = green || (g > 130 && r < 100 && b < 100);
        }
        CHECK(red == (badge == Badge::kProblem));
        CHECK(green == (badge != Badge::kProblem));
      }
    }
  }

}  // namespace

int main() {
  test_badges();
  test_parse_releases();
  test_newer_release();
  test_status_file();
  return test::report_and_exit_code();
}
