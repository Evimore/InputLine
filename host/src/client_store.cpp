#include "client_store.h"

#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace inputline {

  namespace {
    std::string sanitize_name(const std::string &name) {
      std::string out;
      for (char c : name) {
        out.push_back((c == '\n' || c == '\r') ? ' ' : c);
      }
      return out.substr(0, link::kMaxNameLength);
    }
  }  // namespace

  ClientStore::ClientStore(std::string path):
      path_(std::move(path)) {}

#ifndef _WIN32
  namespace {
    /** $XDG_CONFIG_HOME/<name, lowercased>, or ~/.config/<name, lowercased>. */
    std::filesystem::path config_dir(const char *name) {
      namespace fs = std::filesystem;
      const char *xdg = std::getenv("XDG_CONFIG_HOME");
      const char *home = std::getenv("HOME");
      std::string lower(name);
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return xdg && *xdg ? fs::path(xdg) / lower : fs::path(home ? home : ".") / ".config" / lower;
    }
  }  // namespace
#endif

  std::string ClientStore::default_path() {
#ifdef _WIN32
    // Shared by the service and an administrator terminal; see desktop::prepare_data_dirs().
    const char *base = std::getenv("ProgramData");
    return (std::filesystem::path(base && *base ? base : "C:\\ProgramData") / "InputLine" / "pairing" / "inputline-host.conf").string();
#else
    // As root (the service, or sudo): next to the service's other files, so
    // 'sudo inputline-host clients' sees the service's pairings.
    if (::geteuid() == 0) {
      return "/var/lib/inputline/pairing/inputline-host.conf";
    }
    return (config_dir("InputLine") / "inputline-host.conf").string();
#endif
  }

  bool ClientStore::load() {
    std::lock_guard lock(mutex_);
    clients_.clear();
    std::ifstream file(path_);
    if (!file) {
      return true;
    }

    std::string line;
    int line_number = 0;
    int old_pairings = 0;
    while (std::getline(file, line)) {
      ++line_number;
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream stream(line);
      std::string keyword, id_hex, key_hex;
      stream >> keyword >> id_hex >> key_hex;
      std::string name;
      std::getline(stream >> std::ws, name);

      // Pairings from before protocol version 2 sent their key over the
      // network once; those devices pair again with the safer exchange.
      if (keyword == "client") {
        ++old_pairings;
        continue;
      }

      PairedClient client;
      const auto key = link::key_from_hex(key_hex);
      char *end = nullptr;
      const auto id = std::strtoul(id_hex.c_str(), &end, 16);
      if (keyword != "client2" || id_hex.size() != 8 || end == nullptr || *end != '\0' || id == 0 || !key) {
        log::warn("config: ignoring malformed line ", line_number, " in ", path_);
        continue;
      }
      client.client_id = static_cast<std::uint32_t>(id);
      client.key = *key;
      client.name = sanitize_name(name);
      clients_.push_back(client);
    }
    if (old_pairings > 0) {
      log::info("config: ", old_pairings, " device(s) paired with an older version must pair again (pairing is safer now)");
    }
    return true;
  }

  bool ClientStore::save() const {
    namespace fs = std::filesystem;
    std::lock_guard lock(mutex_);
    std::error_code error;
    const fs::path path(path_);
    if (path.has_parent_path()) {
      fs::create_directories(path.parent_path(), error);
    }

    const auto temp = path_ + ".tmp";
    {
      std::ofstream file(temp, std::ios::trunc);
      if (!file) {
        log::error("config: cannot write ", temp);
        return false;
      }
#ifndef _WIN32
      ::chmod(temp.c_str(), S_IRUSR | S_IWUSR);
#endif
      file << "# InputLine host: paired clients. Contains secret keys - do not share.\n";
      for (const auto &client : clients_) {
        char id[9];
        std::snprintf(id, sizeof(id), "%08x", client.client_id);
        file << "client2 " << id << ' ' << link::to_hex(client.key.data(), client.key.size()) << ' ' << client.name << '\n';
      }
      if (!file) {
        return false;
      }
    }
    fs::rename(temp, path, error);
    if (error) {
      log::error("config: cannot replace ", path_, ": ", error.message());
      return false;
    }
    return true;
  }

  std::optional<PairedClient> ClientStore::find(std::uint32_t client_id) const {
    std::lock_guard lock(mutex_);
    for (const auto &client : clients_) {
      if (client.client_id == client_id) {
        return client;
      }
    }
    return std::nullopt;
  }

  void ClientStore::upsert(const PairedClient &client) {
    std::lock_guard lock(mutex_);
    auto sanitized = client;
    sanitized.name = sanitize_name(client.name);
    for (auto &existing : clients_) {
      if (existing.client_id == client.client_id) {
        existing = sanitized;
        return;
      }
    }
    clients_.push_back(sanitized);
  }

  bool ClientStore::remove(std::uint32_t client_id) {
    std::lock_guard lock(mutex_);
    const auto before = clients_.size();
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [client_id](const PairedClient &c) {
      return c.client_id == client_id;
    }), clients_.end());
    return clients_.size() != before;
  }

  std::vector<PairedClient> ClientStore::list() const {
    std::lock_guard lock(mutex_);
    return clients_;
  }

}  // namespace inputline
