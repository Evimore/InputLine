/**
 * @file update_check.h
 * @brief Whether a newer InputLine is out, from GitHub's list of releases.
 *
 * The service asks once a day (turn it off with --no-update-check). Only the
 * public release list is fetched; nothing about the PC is sent.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace inputline::update {

  /** Where InputLine is published. */
  inline constexpr const char *kRepository = "Evimore/InputLine";
  inline constexpr const char *kRepositoryUrl = "https://github.com/Evimore/InputLine";

  struct Release {
    std::string version;  ///< tag without the leading 'v'
    std::string url;      ///< the release's page
    bool prerelease = false;
  };

  /** Releases in GitHub's JSON ("GET /repos/{repo}/releases"); drafts and bad entries are skipped. */
  std::vector<Release> parse_releases(const std::string &json);

  /**
   * The newest release newer than @p current, if any. Pre-releases count only
   * when @p current is one too, so beta testers hear about betas and everyone
   * else only about releases.
   */
  std::optional<Release> newer_release(const std::vector<Release> &releases, const std::string &current);

  /** Download the release list (WinHTTP on Windows, curl elsewhere); empty if it can't. */
  std::optional<std::string> fetch_releases_json(const std::string &user_agent);

  /** Checks in the background: shortly after start, then once a day. */
  class Checker {
  public:
    explicit Checker(std::string current_version);
    ~Checker();

    void start();
    void stop();

    /** The newer release found by the last check, if any. */
    std::optional<Release> available() const;

  private:
    void run();

    std::string current_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::optional<Release> available_;
    std::thread thread_;
  };

}  // namespace inputline::update
