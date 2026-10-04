#include "update_check.h"

#include "log.h"
#include "inputline/version.h"

#include <cctype>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
  #include <windows.h>

  #include <winhttp.h>
#else
  #include "usbip_attach.h"  // run_process, find_program
#endif

namespace inputline::update {

  namespace {
    constexpr int kMaxDepth = 32;
    constexpr std::size_t kMaxResponse = 4 * 1024 * 1024;

    /** Just enough JSON to read GitHub's release list; rejects anything malformed. */
    class Scanner {
    public:
      explicit Scanner(const std::string &text):
          s_(text) {}

      std::vector<Release> releases() {
        std::vector<Release> out;
        space();
        if (!take('[')) {
          return {};
        }
        space();
        if (take(']')) {
          return out;
        }
        for (;;) {
          space();
          if (peek() == '{') {
            Release release;
            bool draft = false;
            if (!release_object(release, draft)) {
              return {};
            }
            if (!draft && is_version(release.version)) {
              out.push_back(release);
            }
          } else if (!skip_value(1)) {
            return {};
          }
          space();
          if (take(']')) {
            return out;
          }
          if (!take(',')) {
            return {};
          }
        }
      }

    private:
      char peek() const {
        return i_ < s_.size() ? s_[i_] : '\0';
      }

      bool take(char c) {
        if (peek() != c) {
          return false;
        }
        ++i_;
        return true;
      }

      void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) {
          ++i_;
        }
      }

      static void put_utf8(std::string &out, std::uint32_t code) {
        if (code < 0x80) {
          out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (code >> 6)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | (code >> 12)));
          out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
      }

      bool string(std::string *out) {
        if (!take('"')) {
          return false;
        }
        while (i_ < s_.size()) {
          const char c = s_[i_++];
          if (c == '"') {
            return true;
          }
          if (static_cast<unsigned char>(c) < 0x20) {
            return false;
          }
          if (c != '\\') {
            if (out) {
              out->push_back(c);
            }
            continue;
          }
          if (i_ >= s_.size()) {
            return false;
          }
          const char e = s_[i_++];
          char plain = 0;
          switch (e) {
            case '"': plain = '"'; break;
            case '\\': plain = '\\'; break;
            case '/': plain = '/'; break;
            case 'b': plain = '\b'; break;
            case 'f': plain = '\f'; break;
            case 'n': plain = '\n'; break;
            case 'r': plain = '\r'; break;
            case 't': plain = '\t'; break;
            case 'u': {
              if (i_ + 4 > s_.size()) {
                return false;
              }
              std::uint32_t code = 0;
              for (int k = 0; k < 4; ++k) {
                const char h = s_[i_++];
                code <<= 4;
                if (h >= '0' && h <= '9') {
                  code |= static_cast<std::uint32_t>(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                  code |= static_cast<std::uint32_t>(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                  code |= static_cast<std::uint32_t>(h - 'A' + 10);
                } else {
                  return false;
                }
              }
              if (out) {
                put_utf8(*out, code >= 0xD800 && code <= 0xDFFF ? 0xFFFD : code);
              }
              continue;
            }
            default:
              return false;
          }
          if (out) {
            out->push_back(plain);
          }
        }
        return false;
      }

      bool literal(const char *word) {
        std::size_t k = 0;
        while (word[k] != '\0') {
          if (i_ + k >= s_.size() || s_[i_ + k] != word[k]) {
            return false;
          }
          ++k;
        }
        i_ += k;
        return true;
      }

      bool boolean(bool &out) {
        if (literal("true")) {
          out = true;
          return true;
        }
        if (literal("false")) {
          out = false;
          return true;
        }
        return false;
      }

      bool skip_value(int depth) {
        if (depth > kMaxDepth) {
          return false;
        }
        space();
        const char c = peek();
        if (c == '"') {
          return string(nullptr);
        }
        if (c == '{' || c == '[') {
          const char close = c == '{' ? '}' : ']';
          ++i_;
          space();
          if (take(close)) {
            return true;
          }
          for (;;) {
            if (c == '{') {
              space();
              if (!string(nullptr)) {
                return false;
              }
              space();
              if (!take(':')) {
                return false;
              }
            }
            if (!skip_value(depth + 1)) {
              return false;
            }
            space();
            if (take(close)) {
              return true;
            }
            if (!take(',')) {
              return false;
            }
          }
        }
        if (literal("true") || literal("false") || literal("null")) {
          return true;
        }
        const std::size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) {
          ++i_;
        }
        return i_ > start;
      }

      bool release_object(Release &release, bool &draft) {
        if (!take('{')) {
          return false;
        }
        space();
        if (take('}')) {
          return true;
        }
        for (;;) {
          space();
          std::string key;
          if (!string(&key)) {
            return false;
          }
          space();
          if (!take(':')) {
            return false;
          }
          space();
          bool ok = false;
          if ((key == "tag_name" || key == "html_url") && peek() == '"') {
            ok = string(key == "tag_name" ? &release.version : &release.url);
          } else if (key == "prerelease" && (peek() == 't' || peek() == 'f')) {
            ok = boolean(release.prerelease);
          } else if (key == "draft" && (peek() == 't' || peek() == 'f')) {
            ok = boolean(draft);
          } else {
            ok = skip_value(2);
          }
          if (!ok) {
            return false;
          }
          space();
          if (take('}')) {
            break;
          }
          if (!take(',')) {
            return false;
          }
        }
        if (!release.version.empty() && (release.version[0] == 'v' || release.version[0] == 'V')) {
          release.version.erase(0, 1);
        }
        // Only ever point people at InputLine's own release pages.
        const std::string expected = std::string(kRepositoryUrl) + "/releases/";
        if (release.url.compare(0, expected.size(), expected) != 0) {
          release.url = std::string(kRepositoryUrl) + "/releases";
        }
        return true;
      }

      const std::string &s_;
      std::size_t i_ = 0;
    };
  }  // namespace

  std::vector<Release> parse_releases(const std::string &json) {
    if (json.size() > kMaxResponse) {
      return {};
    }
    return Scanner(json).releases();
  }

  std::optional<Release> newer_release(const std::vector<Release> &releases, const std::string &current) {
    const bool want_prereleases = is_prerelease(current);
    std::optional<Release> best;
    for (const auto &release : releases) {
      if (release.prerelease && !want_prereleases) {
        continue;
      }
      if (compare_versions(release.version, current) <= 0) {
        continue;
      }
      if (!best || compare_versions(release.version, best->version) > 0) {
        best = release;
      }
    }
    return best;
  }

#ifdef _WIN32
  std::optional<std::string> fetch_releases_json(const std::string &user_agent) {
    const std::wstring agent(user_agent.begin(), user_agent.end());
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
      return std::nullopt;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 20000);
    std::optional<std::string> result;
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection != nullptr) {
      const std::string path = std::string("/repos/") + kRepository + "/releases?per_page=20";
      const std::wstring wide_path(path.begin(), path.end());
      HINTERNET request = WinHttpOpenRequest(connection, L"GET", wide_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
      if (request != nullptr) {
        const wchar_t *headers = L"Accept: application/vnd.github+json\r\n";
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
          std::string body;
          for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
              break;
            }
            const std::size_t old = body.size();
            if (old + available > kMaxResponse) {
              body.clear();
              break;
            }
            body.resize(old + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + old, available, &read)) {
              body.clear();
              break;
            }
            body.resize(old + read);
          }
          if (!body.empty()) {
            result = std::move(body);
          }
        } else {
          log::debug("update check: GitHub answered ", status, " (error ", GetLastError(), ")");
        }
        WinHttpCloseHandle(request);
      }
      WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return result;
  }
#else
  std::optional<std::string> fetch_releases_json(const std::string &user_agent) {
    // curl is on practically every Linux system (pacman itself needs it).
    const std::string curl = find_program("curl");
    if (curl.empty()) {
      return std::nullopt;
    }
    const std::string url = std::string("https://api.github.com/repos/") + kRepository + "/releases?per_page=20";
    std::string output;
    const int code = run_process({curl, "--silent", "--fail", "--location", "--max-time", "30", "--user-agent", user_agent, "--header",
                                  "Accept: application/vnd.github+json", url},
                                 &output, 4 * 1024 * 1024);
    if (code != 0 || output.empty()) {
      return std::nullopt;
    }
    return output;
  }
#endif

  Checker::Checker(std::string current_version):
      current_(std::move(current_version)) {}

  Checker::~Checker() {
    stop();
  }

  void Checker::start() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) {
      return;
    }
    stopping_ = false;
    thread_ = std::thread(&Checker::run, this);
  }

  void Checker::stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::optional<Release> Checker::available() const {
    std::lock_guard lock(mutex_);
    return available_;
  }

  void Checker::run() {
    // A minute after start (the network may not be up yet at boot), then daily.
    auto wait = std::chrono::minutes(1);
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        if (wake_.wait_for(lock, wait, [this] { return stopping_; })) {
          return;
        }
      }
      wait = std::chrono::hours(24);
      const auto json = fetch_releases_json("InputLine/" + current_);
      if (!json) {
        log::debug("update check: could not reach GitHub; trying again tomorrow");
        continue;
      }
      const auto newer = newer_release(parse_releases(*json), current_);
      std::lock_guard lock(mutex_);
      available_ = newer;
    }
  }

}  // namespace inputline::update
