// inputline-host: receives Steam Controller reports from InputLine clients and
// presents them to Steam as a real wired Steam Controller.

#include "client_store.h"
#include "controller_backend.h"
#include "desktop.h"
#include "discovery.h"
#include "link_server.h"
#include "log.h"
#include "net.h"
#include "status_file.h"
#include "tray.h"
#include "update_check.h"
#include "inputline/feature_responder.h"
#include "inputline/link_client.h"
#include "inputline/test_pattern.h"
#include "usbip_attach.h"
#include "usbip_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>
#endif

#ifndef INPUTLINE_VERSION
  #define INPUTLINE_VERSION "dev"
#endif

using namespace inputline;

namespace {

  std::atomic<bool> g_quit {false};

  void on_signal(int) {
    g_quit = true;
  }

  struct Arguments {
    std::string command = "run";
    std::vector<std::string> positional;
    std::uint16_t port = link::kDefaultPort;
    std::string bind = "::";
    std::uint16_t usbip_port = usbip::kDefaultPort;
    std::string usbip_exe;
    bool attach = true;
    std::string config;
    std::string name;
    std::string pin;
    bool verbose = false;
    bool remote_pairing = true;
    bool discovery = true;
    bool update_check = true;
    bool hide_console = false;
    std::string log_file;
    int stats_seconds = 0;
    std::set<std::uint8_t> blocked_settings {kSettingWirelessPacketVersion};
  };

  void print_usage() {
    std::printf(
      "InputLine host " INPUTLINE_VERSION "\n"
      "\n"
      "Usage: inputline-host [options] [command]\n"
      "\n"
      "Commands:\n"
      "  run              Serve paired clients (default). An unpaired iPad / iPhone\n"
      "                   can ask to pair: a code pops up on this PC's screen.\n"
      "  pair             Like run, and show a pairing code right away.\n"
      "  service          How the background service starts it. Extra options go\n"
      "                   in %s.\n"
#ifdef _WIN32
      "  install          (Without the installer; as administrator) Start at every\n"
      "                   logon in the background, and allow the link through the\n"
      "                   firewall.\n"
#else
      "  install          (As root) Set up the background service: systemd, the\n"
      "                   vhci-hcd module, Steam's access, and the firewall.\n"
#endif
      "  uninstall        Undo install. Paired devices are kept.\n"
      "  status           What the background service is doing.\n"
      "  clients          List paired clients.\n"
      "  forget <id>      Remove a paired client.\n"
      "  demo [seconds]   Plug in one virtual controller driven by a test pattern,\n"
      "                   no client needed. Use it to check Steam recognises it.\n"
      "\n"
      "Options:\n"
      "  --port N         UDP port clients connect to (default %u)\n"
      "  --bind ADDRESS   Address to listen on (default: all)\n"
      "  --usbip-port N   Loopback USB/IP port (default %u)\n"
      "  --usbip-exe PATH usbip executable (default: usbip-win2 install / usbip)\n"
      "  --no-attach      Export devices but do not run 'usbip attach'\n"
      "  --config PATH    Paired-clients file (default: %s)\n"
      "  --name NAME      Name shown to clients (default: computer name)\n"
      "  --pin CODE       With 'pair': use this code instead of a random one\n"
      "  --no-remote-pairing  Only pair through 'inputline-host pair'\n"
      "  --no-discovery   Don't announce this PC on the local network\n"
      "  --no-update-check  (Service) Don't check GitHub for a newer InputLine\n"
      "  --log PATH       Write the log to a file\n"
      "  --stats          Log report timing every 10 s (rate, gaps)\n"
      "  --block-setting N  Never pass controller setting N from Steam to the\n"
      "                   physical controller (repeatable; 49 is blocked by default)\n"
      "  --allow-setting N  Pass setting N even if blocked by default\n"
      "  --hide-console   Run without a console window\n"
      "  --verbose        Debug logging\n"
      "  --version        Print the version\n",
      desktop::options_path().c_str(),
      link::kDefaultPort,
      usbip::kDefaultPort,
      ClientStore::default_path().c_str()
    );
  }

  bool parse_port(const char *text, std::uint16_t &out) {
    char *end = nullptr;
    const auto value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > 65535) {
      return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
  }

  bool parse_arguments(int argc, char **argv, Arguments &args, bool have_command = false) {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto value = [&](const char *&out) {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "%s needs a value\n", arg.c_str());
          return false;
        }
        out = argv[++i];
        return true;
      };
      const char *v = nullptr;
      if (arg == "--help" || arg == "-h") {
        print_usage();
        std::exit(0);
      } else if (arg == "--version") {
        std::printf("%s\n", INPUTLINE_VERSION);
        std::exit(0);
      } else if (arg == "--port") {
        if (!value(v) || !parse_port(v, args.port)) {
          return false;
        }
      } else if (arg == "--usbip-port") {
        if (!value(v) || !parse_port(v, args.usbip_port)) {
          return false;
        }
      } else if (arg == "--bind") {
        if (!value(v)) {
          return false;
        }
        args.bind = v;
      } else if (arg == "--usbip-exe") {
        if (!value(v)) {
          return false;
        }
        args.usbip_exe = v;
      } else if (arg == "--config") {
        if (!value(v)) {
          return false;
        }
        args.config = v;
      } else if (arg == "--name") {
        if (!value(v)) {
          return false;
        }
        args.name = v;
      } else if (arg == "--pin") {
        if (!value(v)) {
          return false;
        }
        args.pin = v;
      } else if (arg == "--log") {
        if (!value(v)) {
          return false;
        }
        args.log_file = v;
      } else if (arg == "--no-attach") {
        args.attach = false;
      } else if (arg == "--no-remote-pairing") {
        args.remote_pairing = false;
      } else if (arg == "--no-discovery") {
        args.discovery = false;
      } else if (arg == "--no-update-check") {
        args.update_check = false;
      } else if (arg == "--hide-console") {
        args.hide_console = true;
      } else if (arg == "--stats") {
        args.stats_seconds = 10;
      } else if (arg == "--block-setting" || arg == "--allow-setting") {
        if (!value(v)) {
          return false;
        }
        char *end = nullptr;
        const auto setting = std::strtoul(v, &end, 10);
        if (end == v || *end != '\0' || setting > 255) {
          std::fprintf(stderr, "%s needs a setting number (0-255)\n", arg.c_str());
          return false;
        }
        if (arg == "--block-setting") {
          args.blocked_settings.insert(static_cast<std::uint8_t>(setting));
        } else {
          args.blocked_settings.erase(static_cast<std::uint8_t>(setting));
        }
      } else if (arg == "--verbose" || arg == "-v") {
        args.verbose = true;
      } else if (!arg.empty() && arg[0] == '-') {
        std::fprintf(stderr, "unknown option %s\n", arg.c_str());
        return false;
      } else if (!have_command) {
        args.command = arg;
        have_command = true;
      } else {
        args.positional.push_back(arg);
      }
    }
    return true;
  }

  /** Options for the service, from options.txt: whitespace-separated, '#' starts a comment. */
  std::vector<std::string> read_options_file(const std::string &path) {
    std::vector<std::string> tokens;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
      line = line.substr(0, line.find('#'));
      std::istringstream words(line);
      std::string word;
      while (words >> word) {
        tokens.push_back(word);
      }
    }
    return tokens;
  }

  bool parse_tokens(const std::vector<std::string> &tokens, Arguments &args) {
    std::vector<std::string> storage {"inputline-host"};
    storage.insert(storage.end(), tokens.begin(), tokens.end());
    std::vector<char *> argv;
    for (auto &token : storage) {
      argv.push_back(token.data());
    }
    return parse_arguments(static_cast<int>(argv.size()), argv.data(), args, true);
  }

  std::string computer_name() {
#ifdef _WIN32
    const char *name = std::getenv("COMPUTERNAME");
    return name && *name ? name : "PC";
#else
    char name[256] = {};
    return ::gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0' ? std::string(name) : "PC";
#endif
  }

  bool valid_pin(const std::string &pin) {
    if (pin.size() != link::kPinDigits) {
      return false;
    }
    for (char c : pin) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  }

  std::string read_first_line(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line.size() > 64 ? std::string() : line;
  }

  void write_first_line(const std::string &path, const std::string &line) {
    std::ofstream out(path, std::ios::trunc);
    out << line << "\n";
  }

  /** Log usbip's state and, if something is wrong, say so on screen. */
  void report_usbip(const UsbipCheck &usbip) {
#ifndef _WIN32
    switch (usbip.state) {
      case UsbipCheck::State::kOk:
        log::info("usbip and vhci-hcd are ready");
        break;
      case UsbipCheck::State::kMissing:
      case UsbipCheck::State::kTooOld:
        log::warn("usbip isn't installed: InputLine can't plug controllers in. Install it: ", desktop::install_hint(desktop::Package::kUsbip));
        desktop::show_notification("InputLine needs usbip", "It plugs the controller into this PC. Install it: " + desktop::install_hint(desktop::Package::kUsbip));
        break;
      case UsbipCheck::State::kNoDriver:
        log::warn("the vhci-hcd kernel module isn't loaded: InputLine can't plug controllers in. Try: sudo modprobe vhci-hcd");
        desktop::show_notification("InputLine needs the vhci-hcd kernel module",
                                   "It plugs the controller into this PC. Try: sudo modprobe vhci-hcd. If that fails, your kernel doesn't include it.");
        break;
    }
    return;
#endif
    switch (usbip.state) {
      case UsbipCheck::State::kOk:
        log::info("usbip-win2 ", usbip.version.empty() ? std::string("is installed") : usbip.version + " is installed");
        break;
      case UsbipCheck::State::kMissing:
        log::warn("usbip-win2 is not installed: InputLine can't plug controllers into Windows. Get it from ", kUsbipDownloadUrl);
        desktop::show_notification("InputLine needs usbip-win2",
                                   "It plugs the controller into Windows. Click to download it; InputLine starts working once it's installed.",
                                   kUsbipDownloadUrl);
        break;
      case UsbipCheck::State::kTooOld:
        log::warn("usbip-win2 ", usbip.version, " is too old for InputLine (", kMinUsbipVersion, " or newer needed). Get it from ", kUsbipDownloadUrl);
        desktop::show_notification("usbip-win2 is too old for InputLine",
                                   "You have " + usbip.version + "; InputLine needs " + kMinUsbipVersion + " or newer. Click to download it.",
                                   kUsbipDownloadUrl);
        break;
      case UsbipCheck::State::kNoDriver:
        break;  // Linux only
    }
  }

  void show_code(const std::string &client_name, const std::string &code) {
    std::printf(
      "\n  Pairing code for %s:  %.3s %s\n"
      "  Type it into InputLine on your iPad or iPhone (it works for 2 minutes).\n\n",
      client_name.c_str(), code.c_str(), code.size() > 3 ? code.c_str() + 3 : ""
    );
    std::fflush(stdout);
    // Also in the log, for when a fullscreen game hides the popup. It only works for 2 minutes.
    log::info("pairing code for '", client_name, "': ", code);
    desktop::show_pairing_code(client_name, code);
  }

  /** The arguments 'install' passes on to the background 'run'. */
  std::vector<std::string> run_arguments(const Arguments &args) {
    std::vector<std::string> out;
    if (args.port != link::kDefaultPort) {
      out.insert(out.end(), {"--port", std::to_string(args.port)});
    }
    if (args.usbip_port != usbip::kDefaultPort) {
      out.insert(out.end(), {"--usbip-port", std::to_string(args.usbip_port)});
    }
    if (!args.bind.empty() && args.bind != "::") {
      out.insert(out.end(), {"--bind", args.bind});
    }
    if (!args.usbip_exe.empty()) {
      out.insert(out.end(), {"--usbip-exe", std::filesystem::absolute(args.usbip_exe).string()});
    }
    if (!args.config.empty()) {
      out.insert(out.end(), {"--config", std::filesystem::absolute(args.config).string()});
    }
    if (!args.name.empty()) {
      out.insert(out.end(), {"--name", args.name});
    }
    if (!args.remote_pairing) {
      out.emplace_back("--no-remote-pairing");
    }
    if (!args.discovery) {
      out.emplace_back("--no-discovery");
    }
    if (args.verbose) {
      out.emplace_back("--verbose");
    }
    if (args.stats_seconds > 0) {
      out.emplace_back("--stats");
    }
    const std::set<std::uint8_t> defaults {kSettingWirelessPacketVersion};
    for (const auto setting : args.blocked_settings) {
      if (defaults.count(setting) == 0) {
        out.insert(out.end(), {"--block-setting", std::to_string(setting)});
      }
    }
    for (const auto setting : defaults) {
      if (args.blocked_settings.count(setting) == 0) {
        out.insert(out.end(), {"--allow-setting", std::to_string(setting)});
      }
    }
    return out;
  }

  /** 'status': what the background service wrote to its status file. */
  int show_status() {
    const std::string path = desktop::data_file("status.txt");
    const auto status = path.empty() ? std::nullopt : read_status_file(path);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (!status || now - status->time > 15 || status->time - now > 15) {
      std::printf("The InputLine service isn't running.\n");
#ifdef _WIN32
      std::printf("Start it from Terminal (Admin): Start-Service InputLine\n");
#else
      std::printf("Start it: sudo systemctl start inputline    (log: journalctl -u inputline)\n");
#endif
      return 1;
    }
    std::printf("InputLine %s is running\n", status->version.c_str());
    if (status->devices.empty()) {
      std::printf("  No InputLine app connected\n");
    } else {
      std::string devices;
      for (const auto &device : status->devices) {
        devices += (devices.empty() ? "" : ", ") + device;
      }
      const std::string controllers = status->controllers == 0   ? "no controller on yet"
                                      : status->controllers == 1 ? "1 controller"
                                                                 : std::to_string(status->controllers) + " controllers";
      std::printf("  Connected: %s (%s)\n", devices.c_str(), controllers.c_str());
    }
#ifdef _WIN32
    const char *driver = "usbip-win2";
#else
    const char *driver = "usbip";
#endif
    if (status->usbip == "ok") {
      std::printf("  %s: ready\n", driver);
    } else if (status->usbip == "nodriver") {
      std::printf("  The vhci-hcd kernel module isn't loaded: try sudo modprobe vhci-hcd\n");
    } else if (status->usbip == "old") {
      std::printf("  %s %s is too old: get %s or newer from %s\n", driver, status->usbip_version.c_str(), kMinUsbipVersion, kUsbipDownloadUrl);
    } else {
#ifdef _WIN32
      std::printf("  usbip-win2 isn't installed: get it from %s\n", kUsbipDownloadUrl);
#else
      std::printf("  usbip isn't installed: %s\n", desktop::install_hint(desktop::Package::kUsbip).c_str());
#endif
    }
    if (!status->update_version.empty()) {
      std::printf("  InputLine %s is available: %s\n", status->update_version.c_str(), status->update_url.c_str());
    }
    return 0;
  }

  int list_clients(ClientStore &store) {
    const auto clients = store.list();
    if (clients.empty() && !desktop::data_dir().empty() && !desktop::is_elevated()) {
#ifdef _WIN32
      std::printf("Paired devices are only readable from an administrator terminal (Terminal (Admin)).\n");
#else
      std::printf("Paired devices are only readable as root: sudo inputline-host clients\n");
#endif
      return 1;
    }
    if (clients.empty()) {
      std::printf("No paired clients. Run 'inputline-host pair' to add one.\n");
      return 0;
    }
    for (const auto &client : clients) {
      std::printf("%08x  %s\n", client.client_id, client.name.c_str());
    }
    return 0;
  }

  int forget_client(ClientStore &store, const Arguments &args) {
    if (args.positional.empty()) {
      std::fprintf(stderr, "usage: inputline-host forget <id>\n");
      return 2;
    }
    const auto id = static_cast<std::uint32_t>(std::strtoul(args.positional[0].c_str(), nullptr, 16));
    if (!store.remove(id) || !store.save()) {
      std::fprintf(stderr, "no paired client %s\n", args.positional[0].c_str());
      return 1;
    }
    std::printf("Forgot %08x\n", id);
    return 0;
  }

  int run_demo(ControllerBackend &backend, const Arguments &args) {
    const int seconds = args.positional.empty() ? 60 : std::atoi(args.positional[0].c_str());
    link::Attach attach;
    std::atomic<std::uint64_t> outputs {0};
    auto controller = backend.create(attach, 0, [&outputs](link::OutputKind kind, const std::vector<std::uint8_t> &report) {
      ++outputs;
      log::info("demo: Steam sent ", kind == link::OutputKind::kOutputReport ? "output report 0x" : "setting 0x", std::hex,
                int(report.size() > 1 && kind == link::OutputKind::kSetFeature ? report[1] : report[0]));
    });
    if (!controller) {
      log::error("demo: could not create the virtual controller");
      return 1;
    }

    log::info("demo: driving a virtual Steam Controller for ", seconds, " s. Open Steam > Settings > Controller to watch it.");
    TestPattern pattern;
    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    while (!g_quit && std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
      const auto frame = pattern.frame(static_cast<std::uint64_t>(elapsed));
      controller->submit(frame.data(), frame.size());
      next += std::chrono::microseconds(kStateReportIntervalUs);
      std::this_thread::sleep_until(next);
    }
    controller->release_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log::info("demo: done (", outputs.load(), " messages from Steam)");
    return 0;
  }

}  // namespace

namespace {

  int run_command(Arguments &args) {
    if (args.command == "install") {
      desktop::InstallOptions install;
      install.run_arguments = run_arguments(args);
      install.port = args.port;
      return desktop::install(install);
    }
    if (args.command == "uninstall") {
      return desktop::uninstall();
    }
    if (args.command == "status") {
      return show_status();
    }
    if (!net::startup()) {
      log::error("network startup failed");
      return 1;
    }

    desktop::prepare_data_dirs();
    const std::string config = args.config.empty() ? ClientStore::default_path() : args.config;
    ClientStore store(config);
    store.load();

    if (args.command == "clients") {
      return list_clients(store);
    }
    if (args.command == "forget") {
      return forget_client(store, args);
    }
    if (args.command != "run" && args.command != "pair" && args.command != "demo" && args.command != "service") {
      std::fprintf(stderr, "unknown command '%s'\n\n", args.command.c_str());
      print_usage();
      return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    usbip::Server usbip_server;
    bool usbip_started = usbip_server.start("127.0.0.1", args.usbip_port);
    // The demo can run next to the InputLine service: take the next free port.
    for (int offset = 1; !usbip_started && args.command == "demo" && offset <= 4; ++offset) {
      usbip_started = usbip_server.start("127.0.0.1", static_cast<std::uint16_t>(args.usbip_port + offset));
    }
    if (!usbip_started) {
      log::error("is another USB/IP server using port ", args.usbip_port, "? Try --usbip-port");
      return 1;
    }

    UsbipBackendOptions backend_options;
    backend_options.attach.enabled = args.attach;
    backend_options.attach.executable = args.usbip_exe;
    backend_options.attach.port = usbip_server.port();
    backend_options.blocked_settings = args.blocked_settings;
    UsbipBackend backend(usbip_server, backend_options);

    if (args.command == "demo") {
      const int code = run_demo(backend, args);
      usbip_server.stop();
      return code;
    }

    LinkServer::Options options;
    options.bind_address = args.bind;
    options.port = args.port;
    options.host_name = args.name.empty() ? computer_name() : args.name;
  options.software_version = INPUTLINE_VERSION;
    options.remote_pairing = args.remote_pairing;
    options.show_code = show_code;
    options.stats_interval = std::chrono::seconds(args.stats_seconds);
    auto server = std::make_unique<LinkServer>(options, store, backend);
    if (!server->start()) {
      if (desktop::service_installed() && args.command != "service") {
#ifdef _WIN32
        log::error("the InputLine service already runs in the background: there is nothing to start. "
                   "To run it in this window instead, first stop the service (Stop-Service InputLine).");
#else
        log::error("the InputLine service already runs in the background: there is nothing to start. "
                   "To run it here instead, first stop the service (sudo systemctl stop inputline).");
#endif
      } else {
        log::error("is inputline-host already running (for example installed with 'inputline-host install')? "
                   "Then there is nothing to start: pair by tapping Connect in InputLine.");
      }
      return 1;
    }

    if (args.command == "pair") {
      const std::string code = valid_pin(args.pin) ? args.pin : link::ClientSession::new_pin(random_bytes);
      server->open_pairing(code, options.pairing_window, [](const LinkServer::PairingOutcome &outcome) {
        if (outcome.success) {
          std::printf("Paired with '%s'. It will connect automatically from now on.\n", outcome.client_name.c_str());
        } else {
          std::printf("Pairing did not complete. Run 'inputline-host pair' to try again.\n");
        }
        std::fflush(stdout);
      });
      std::printf("Open InputLine on your iPad or iPhone, enter this PC's address and tap Connect.\n");
      show_code("your iPad or iPhone", code);
    } else if (store.list().empty()) {
      log::info("no paired devices yet: tap Connect in InputLine on your iPad or iPhone, and a pairing code will pop up here");
    }

    discovery::Advertiser advertiser;
    if (args.discovery) {
      advertiser.start(options.host_name, server->port());
    }

    // As a service: the tray icon, its status file, and the daily update check.
    const bool service = args.command == "service";
    const std::string status_path = desktop::data_file("status.txt");
    update::Checker updates(INPUTLINE_VERSION);
    if (service) {
      tray::start_agents();
      if (args.update_check) {
        updates.start();
      }
    }
    std::string update_announced = read_first_line(desktop::data_file("update-notified.txt"));

    log::info("ready: ", store.list().size(), " paired client(s). Press Ctrl+C to stop.");
    auto next_status = std::chrono::steady_clock::now();
    auto next_usbip_check = next_status;
    UsbipCheck usbip;
    std::optional<UsbipCheck::State> usbip_reported;
    while (!g_quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (!service || status_path.empty() || std::chrono::steady_clock::now() < next_status) {
        continue;
      }
      next_status = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      // usbip-win2 can be installed (or removed) while InputLine runs.
      if (std::chrono::steady_clock::now() >= next_usbip_check) {
        next_usbip_check = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        usbip = check_usbip(args.usbip_exe);
        if (usbip_reported != usbip.state) {
          usbip_reported = usbip.state;
          report_usbip(usbip);
        }
      }
      const auto link = server->status();
      ServiceStatus status;
      status.version = INPUTLINE_VERSION;
      status.time = static_cast<std::int64_t>(std::time(nullptr));
      status.controllers = link.controllers;
      status.devices = link.clients;
      status.usbip = usbip.state == UsbipCheck::State::kOk        ? "ok"
                     : usbip.state == UsbipCheck::State::kMissing  ? "missing"
                     : usbip.state == UsbipCheck::State::kNoDriver ? "nodriver"
                                                                    : "old";
      status.usbip_version = usbip.version;
      if (const auto release = updates.available()) {
        status.update_version = release->version;
        status.update_url = release->url;
        if (release->version != update_announced) {
          // Once per new version.
          update_announced = release->version;
          write_first_line(desktop::data_file("update-notified.txt"), update_announced);
          log::info("InputLine ", release->version, " is available: ", release->url);
          desktop::show_notification("InputLine " + release->version + " is available",
                                     "You have " INPUTLINE_VERSION ". Click to open the download page.", release->url);
        }
      }
      write_status_file(status_path, status);
    }

    log::info("shutting down");
    if (service) {
      updates.stop();
      tray::stop_agents();
      std::error_code ignored;
      std::filesystem::remove(status_path, ignored);
    }
    advertiser.stop();
    server->stop();
    usbip_server.stop();
    return 0;
  }

}  // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && std::strcmp(argv[1], "notify") == 0) {
    return desktop::run_notifier();  // started by show_notification()
  }
  if (argc >= 2 && std::strcmp(argv[1], "tray") == 0) {
    // The icon, started by the service (or the Start menu shortcut, with --show).
    return tray::run_tray(argc >= 3 && std::strcmp(argv[2], "--show") == 0);
  }
  Arguments args;
  if (!parse_arguments(argc, argv, args)) {
    print_usage();
    return 2;
  }

  const bool service = args.command == "service";
  if (service) {
    // Options live next to the log, in a folder only administrators can change.
    if (desktop::prepare_data_dirs() && !parse_tokens(read_options_file(desktop::options_path()), args)) {
      std::fprintf(stderr, "ignoring %s: it has an invalid option\n", desktop::options_path().c_str());
    }
#ifdef _WIN32
    if (args.log_file.empty()) {
      args.log_file = desktop::data_file("inputline-host.log");
    }
#endif  // Linux: the log goes to the journal
  }

  log::set_level(args.verbose ? log::Level::kDebug : log::Level::kInfo);
  if (!args.log_file.empty() && !log::set_file(args.log_file)) {
    log::warn("cannot write the log to ", args.log_file);
  }
  if (args.hide_console) {
    desktop::hide_console();
  }
  if (service) {
#ifdef _WIN32
    log::info("InputLine ", INPUTLINE_VERSION, " starting as a Windows service");
#else
    log::info("InputLine ", INPUTLINE_VERSION, " starting as a service");
#endif
    args.command = "service";
    return desktop::run_service([&args] { return run_command(args); }, [] { g_quit = true; });
  }
  return run_command(args);
}
