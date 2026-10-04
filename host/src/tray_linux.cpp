// The InputLine icon on a Linux desktop (KDE Plasma, and other desktops that
// show StatusNotifierItems): the service's status, a badge, and a menu.
//
// It talks the freedesktop StatusNotifierItem and DBusMenu protocols over the
// session bus, through libdbus-1 loaded at run time (see dbus_lite.h). The
// service starts it for signed-in users, and an XDG autostart entry at every
// later sign-in; it reads the status file the service writes.

#include "tray.h"

#include "dbus_lite.h"
#include "desktop.h"
#include "log.h"
#include "status_file.h"
#include "tray_badge.h"
#include "tray_icon_pixels.h"
#include "update_check.h"
#include "usbip_attach.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace inputline::tray {

  namespace {
    namespace fs = std::filesystem;
    using namespace dbus;

    constexpr const char *kItemPath = "/StatusNotifierItem";
    constexpr const char *kMenuPath = "/MenuBar";
    constexpr const char *kItemInterface = "org.kde.StatusNotifierItem";
    constexpr const char *kMenuInterface = "com.canonical.dbusmenu";
    constexpr const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";
    constexpr const char *kWatcherName = "org.kde.StatusNotifierWatcher";
    /** One icon per desktop session. */
    constexpr const char *kInstanceName = "io.github.evimore.InputLine.Tray";
    constexpr std::int64_t kStaleAfterSeconds = 15;

    enum MenuId : int {
      kMenuRoot = 0,
      kMenuTitle,
      kMenuProblem,
      kMenuProblemHint,
      kMenuSeparator1,
      kMenuSummary,
      kMenuUpdate,
      kMenuSeparator2,
      kMenuLog,
      kMenuGuide,
      kMenuSeparator3,
      kMenuHide,
    };

    struct MenuItem {
      int id;
      std::string label;
      bool enabled = true;
      bool separator = false;
    };

    struct Tray {
      const Api *api = nullptr;
      DBusConnection *bus = nullptr;
      std::string item_name;
      ServiceStatus status;
      bool running = false;
      std::string summary;
      std::string problem;
      std::string problem_hint;
      int badge = -1;  // a Badge, or -1 for none
      std::uint32_t revision = 1;
      std::vector<MenuItem> menu;
      bool quit = false;
    };

    std::string config_dir() {
      const char *xdg = std::getenv("XDG_CONFIG_HOME");
      const char *home = std::getenv("HOME");
      return (xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config") / "inputline";
    }

    std::string hidden_flag() {
      return (fs::path(config_dir()) / "tray-hidden").string();
    }

    std::string guide_url() {
      return std::string(update::kRepositoryUrl) + "/blob/main/docs/host-setup-linux.md";
    }

    /** Start a program and let it run on its own (SIGCHLD is ignored, so it is reaped). */
    bool launch(const std::vector<std::string> &argv) {
      std::vector<char *> raw;
      for (const auto &arg : argv) {
        raw.push_back(const_cast<char *>(arg.c_str()));
      }
      raw.push_back(nullptr);
      pid_t pid = -1;
      return ::posix_spawnp(&pid, raw[0], nullptr, nullptr, raw.data(), environ) == 0;
    }

    void open_url(const std::string &url) {
      // Only InputLine's own pages.
      if (url.rfind(update::kRepositoryUrl, 0) == 0) {
        launch({"xdg-open", url});
      }
    }

    /** The service's log, live, in a terminal. */
    void open_log() {
      const std::vector<std::string> command {"journalctl", "-u", "inputline", "-n", "200", "-f"};
      const std::vector<std::pair<std::string, std::vector<std::string>>> terminals {
        {"konsole", {"-e"}},     {"kgx", {"--"}},          {"gnome-terminal", {"--"}}, {"ptyxis", {"--"}},
        {"xfce4-terminal", {"-x"}}, {"alacritty", {"-e"}}, {"kitty", {}},             {"foot", {}},
        {"wezterm", {"start", "--"}}, {"xterm", {"-e"}},
      };
      for (const auto &[terminal, prefix] : terminals) {
        if (find_program(terminal).empty()) {
          continue;
        }
        std::vector<std::string> argv {terminal};
        argv.insert(argv.end(), prefix.begin(), prefix.end());
        argv.insert(argv.end(), command.begin(), command.end());
        launch(argv);
        return;
      }
      launch({"notify-send", "--app-name=InputLine", "InputLine's log", "Open a terminal and run: journalctl -u inputline -f"});
    }

    // --- Writing D-Bus values -------------------------------------------------

    void append_string(Tray &t, DBusMessageIter *it, const std::string &value) {
      const char *text = value.c_str();
      t.api->message_iter_append_basic(it, kTypeString, &text);
    }

    void append_object_path(Tray &t, DBusMessageIter *it, const char *path) {
      t.api->message_iter_append_basic(it, kTypeObjectPath, &path);
    }

    void append_int(Tray &t, DBusMessageIter *it, std::int32_t value) {
      t.api->message_iter_append_basic(it, kTypeInt32, &value);
    }

    void append_uint(Tray &t, DBusMessageIter *it, std::uint32_t value) {
      t.api->message_iter_append_basic(it, kTypeUint32, &value);
    }

    void append_bool(Tray &t, DBusMessageIter *it, bool value) {
      const dbus_bool_t flag = value ? 1 : 0;
      t.api->message_iter_append_basic(it, kTypeBoolean, &flag);
    }

    void container(Tray &t, DBusMessageIter *it, int type, const char *signature, const std::function<void(DBusMessageIter *)> &body) {
      DBusMessageIter sub {};
      t.api->message_iter_open_container(it, type, signature, &sub);
      body(&sub);
      t.api->message_iter_close_container(it, &sub);
    }

    void variant(Tray &t, DBusMessageIter *it, const char *signature, const std::function<void(DBusMessageIter *)> &body) {
      container(t, it, kTypeVariant, signature, body);
    }

    /** a{sv} entry. */
    void dict_entry(Tray &t, DBusMessageIter *dict, const std::string &key, const char *signature, const std::function<void(DBusMessageIter *)> &value) {
      container(t, dict, kTypeDictEntry, nullptr, [&](DBusMessageIter *entry) {
        append_string(t, entry, key);
        variant(t, entry, signature, value);
      });
    }

    // --- The icon ----------------------------------------------------------

    /** a(iiay): every size, ARGB in network byte order, with the badge if any. */
    void append_pixmaps(Tray &t, DBusMessageIter *it, bool with_badge) {
      container(t, it, kTypeArray, "(iiay)", [&](DBusMessageIter *array) {
        for (const auto &image : kIconImages) {
          std::vector<std::uint32_t> pixels(image.pixels, image.pixels + image.size * image.size);
          if (with_badge && t.badge >= 0) {
            draw_badge(pixels.data(), image.size, static_cast<Badge>(t.badge));
          }
          std::vector<std::uint8_t> bytes;
          bytes.reserve(pixels.size() * 4);
          for (const auto pixel : pixels) {
            bytes.push_back(static_cast<std::uint8_t>(pixel >> 24));
            bytes.push_back(static_cast<std::uint8_t>(pixel >> 16));
            bytes.push_back(static_cast<std::uint8_t>(pixel >> 8));
            bytes.push_back(static_cast<std::uint8_t>(pixel));
          }
          container(t, array, kTypeStruct, nullptr, [&](DBusMessageIter *entry) {
            append_int(t, entry, image.size);
            append_int(t, entry, image.size);
            container(t, entry, kTypeArray, "y", [&](DBusMessageIter *data) {
              const std::uint8_t *start = bytes.data();
              t.api->message_iter_append_fixed_array(data, kTypeByte, &start, static_cast<int>(bytes.size()));
            });
          });
        }
      });
    }

    std::string tooltip(const Tray &t) {
      return t.problem.empty() ? t.summary : t.problem;
    }

    /** One StatusNotifierItem property as a variant; false if there is no such property. */
    bool append_item_property(Tray &t, DBusMessageIter *it, const std::string &name) {
      auto string_variant = [&](const std::string &value) {
        variant(t, it, "s", [&](DBusMessageIter *v) {
          append_string(t, v, value);
        });
      };
      if (name == "Category") {
        string_variant("ApplicationStatus");
      } else if (name == "Id") {
        string_variant("inputline");
      } else if (name == "Title") {
        string_variant("InputLine");
      } else if (name == "Status") {
        // A problem keeps the icon in view rather than in the hidden ones.
        string_variant(t.problem.empty() ? "Active" : "NeedsAttention");
      } else if (name == "IconName" || name == "OverlayIconName" || name == "AttentionIconName" || name == "AttentionMovieName" ||
                 name == "IconThemePath") {
        string_variant("");
      } else if (name == "WindowId") {
        variant(t, it, "i", [&](DBusMessageIter *v) {
          append_int(t, v, 0);
        });
      } else if (name == "IconPixmap" || name == "AttentionIconPixmap") {
        variant(t, it, "a(iiay)", [&](DBusMessageIter *v) {
          append_pixmaps(t, v, true);
        });
      } else if (name == "OverlayIconPixmap") {
        variant(t, it, "a(iiay)", [&](DBusMessageIter *v) {
          container(t, v, kTypeArray, "(iiay)", [](DBusMessageIter *) {});
        });
      } else if (name == "ToolTip") {
        variant(t, it, "(sa(iiay)ss)", [&](DBusMessageIter *v) {
          container(t, v, kTypeStruct, nullptr, [&](DBusMessageIter *tip) {
            append_string(t, tip, "");
            container(t, tip, kTypeArray, "(iiay)", [](DBusMessageIter *) {});
            append_string(t, tip, "InputLine");
            append_string(t, tip, tooltip(t));
          });
        });
      } else if (name == "ItemIsMenu") {
        variant(t, it, "b", [&](DBusMessageIter *v) {
          append_bool(t, v, true);
        });
      } else if (name == "Menu") {
        variant(t, it, "o", [&](DBusMessageIter *v) {
          append_object_path(t, v, kMenuPath);
        });
      } else {
        return false;
      }
      return true;
    }

    const char *const kItemProperties[] = {
      "Category", "Id", "Title", "Status", "WindowId", "IconName", "IconPixmap", "OverlayIconName", "OverlayIconPixmap",
      "AttentionIconName", "AttentionIconPixmap", "AttentionMovieName", "ToolTip", "ItemIsMenu", "Menu", "IconThemePath",
    };

    // --- The menu ------------------------------------------------------------

    void build_menu(Tray &t) {
      std::vector<MenuItem> menu;
      menu.push_back({kMenuTitle, "InputLine " + (t.running ? t.status.version : std::string()), false});
      if (!t.problem.empty()) {
        menu.push_back({kMenuProblem, "⚠ " + t.problem + ": how to fix it"});
        menu.push_back({kMenuProblemHint, t.problem_hint, false});
        menu.push_back({kMenuSeparator1, "", true, true});
      }
      menu.push_back({kMenuSummary, t.summary, false});
      if (t.running && !t.status.update_version.empty()) {
        menu.push_back({kMenuUpdate, "Update available: InputLine " + t.status.update_version + "..."});
      }
      menu.push_back({kMenuSeparator2, "", true, true});
      menu.push_back({kMenuLog, "Show the log"});
      menu.push_back({kMenuGuide, "Setup guide and troubleshooting"});
      menu.push_back({kMenuSeparator3, "", true, true});
      menu.push_back({kMenuHide, "Hide this icon"});
      t.menu = std::move(menu);
    }

    const MenuItem *find_item(const Tray &t, int id) {
      for (const auto &item : t.menu) {
        if (item.id == id) {
          return &item;
        }
      }
      return nullptr;
    }

    /** a{sv} of one menu item's properties. */
    void append_item_properties(Tray &t, DBusMessageIter *it, int id) {
      container(t, it, kTypeArray, "{sv}", [&](DBusMessageIter *dict) {
        if (id == kMenuRoot) {
          dict_entry(t, dict, "children-display", "s", [&](DBusMessageIter *v) {
            append_string(t, v, "submenu");
          });
          return;
        }
        const MenuItem *item = find_item(t, id);
        if (item == nullptr) {
          return;
        }
        if (item->separator) {
          dict_entry(t, dict, "type", "s", [&](DBusMessageIter *v) {
            append_string(t, v, "separator");
          });
          return;
        }
        dict_entry(t, dict, "label", "s", [&](DBusMessageIter *v) {
          append_string(t, v, item->label);
        });
        dict_entry(t, dict, "enabled", "b", [&](DBusMessageIter *v) {
          append_bool(t, v, item->enabled);
        });
      });
    }

    /** (ia{sv}av): an item and, for the root, its children. */
    void append_layout(Tray &t, DBusMessageIter *it, int id) {
      container(t, it, kTypeStruct, nullptr, [&](DBusMessageIter *node) {
        append_int(t, node, id);
        append_item_properties(t, node, id);
        container(t, node, kTypeArray, "v", [&](DBusMessageIter *children) {
          if (id != kMenuRoot) {
            return;
          }
          for (const auto &item : t.menu) {
            variant(t, children, "(ia{sv}av)", [&](DBusMessageIter *v) {
              append_layout(t, v, item.id);
            });
          }
        });
      });
    }

    void emit(Tray &t, const char *path, const char *interface, const char *name, const std::function<void(DBusMessageIter *)> &args = {}) {
      DBusMessage *signal = t.api->message_new_signal(path, interface, name);
      if (signal == nullptr) {
        return;
      }
      if (args) {
        DBusMessageIter it {};
        t.api->message_iter_init_append(signal, &it);
        args(&it);
      }
      t.api->connection_send(t.bus, signal, nullptr);
      t.api->message_unref(signal);
    }

    void hide_icon(Tray &t) {
      std::error_code error;
      fs::create_directories(config_dir(), error);
      std::ofstream(hidden_flag()) << "hidden\n";
      launch({"notify-send", "--app-name=InputLine", "The InputLine icon is hidden",
              "InputLine keeps running in the background. To show the icon again, open InputLine from the application menu."});
      t.quit = true;
    }

    void clicked(Tray &t, int id) {
      switch (id) {
        case kMenuProblem:
          open_url(guide_url() + "#troubleshooting");
          break;
        case kMenuUpdate:
          open_url(t.status.update_url);
          break;
        case kMenuLog:
          open_log();
          break;
        case kMenuGuide:
          open_url(guide_url());
          break;
        case kMenuHide:
          hide_icon(t);
          break;
        default:
          break;
      }
    }

    // --- Reading the service's status --------------------------------------

    void refresh(Tray &t, bool force = false) {
      const auto status = read_status_file(desktop::data_file("status.txt"));
      const auto now = static_cast<std::int64_t>(std::time(nullptr));
      const bool running = status.has_value() && now - status->time <= kStaleAfterSeconds && status->time - now <= kStaleAfterSeconds;
      const ServiceStatus current = running ? *status : ServiceStatus {};

      std::string summary;
      if (!running) {
        summary = "InputLine isn't running";
      } else if (current.devices.empty()) {
        summary = "Waiting for the InputLine app";
      } else {
        for (const auto &device : current.devices) {
          summary += (summary.empty() ? "Connected: " : ", ") + device;
        }
        if (current.controllers == 1) {
          summary += " (1 controller)";
        } else if (current.controllers > 1) {
          summary += " (" + std::to_string(current.controllers) + " controllers)";
        }
      }
      std::string problem;
      std::string hint;
      if (running && (current.usbip == "missing" || current.usbip == "old")) {
        problem = "usbip isn't installed";
        hint = "Install it: " + desktop::install_hint(desktop::Package::kUsbip);
      } else if (running && current.usbip == "nodriver") {
        problem = "The vhci-hcd kernel module isn't loaded";
        hint = "Try: sudo modprobe vhci-hcd";
      }
      const int badge = !problem.empty()                           ? static_cast<int>(Badge::kProblem)
                        : running && current.controllers > 0       ? static_cast<int>(Badge::kController)
                        : running && !current.devices.empty()      ? static_cast<int>(Badge::kApp)
                                                                   : -1;

      const bool icon_changed = force || badge != t.badge;
      const bool status_changed = force || problem.empty() != t.problem.empty();
      const bool text_changed = force || summary != t.summary || problem != t.problem;
      const bool menu_changed = text_changed || running != t.running || current.version != t.status.version ||
                                current.update_version != t.status.update_version;
      t.running = running;
      t.status = current;
      t.summary = summary;
      t.problem = problem;
      t.problem_hint = hint;
      t.badge = badge;
      if (icon_changed) {
        emit(t, kItemPath, kItemInterface, "NewIcon");
        emit(t, kItemPath, kItemInterface, "NewAttentionIcon");
      }
      if (status_changed) {
        emit(t, kItemPath, kItemInterface, "NewStatus", [&](DBusMessageIter *it) {
          append_string(t, it, problem.empty() ? "Active" : "NeedsAttention");
        });
      }
      if (text_changed) {
        emit(t, kItemPath, kItemInterface, "NewToolTip");
      }
      if (menu_changed) {
        build_menu(t);
        ++t.revision;
        emit(t, kMenuPath, kMenuInterface, "LayoutUpdated", [&](DBusMessageIter *it) {
          append_uint(t, it, t.revision);
          append_int(t, it, kMenuRoot);
        });
      }
    }

    // --- Handling calls ------------------------------------------------------

    /** Reply to @p call; @p body fills in the reply's arguments. */
    void reply(Tray &t, DBusMessage *call, const std::function<void(DBusMessageIter *)> &body = {}) {
      DBusMessage *answer = t.api->message_new_method_return(call);
      if (answer == nullptr) {
        return;
      }
      if (body) {
        DBusMessageIter it {};
        t.api->message_iter_init_append(answer, &it);
        body(&it);
      }
      t.api->connection_send(t.bus, answer, nullptr);
      t.api->message_unref(answer);
    }

    void reply_error(Tray &t, DBusMessage *call, const char *name, const char *text) {
      DBusMessage *answer = t.api->message_new_error(call, name, text);
      if (answer != nullptr) {
        t.api->connection_send(t.bus, answer, nullptr);
        t.api->message_unref(answer);
      }
    }

    /** The string arguments of a call, in order (others are skipped). */
    std::vector<std::string> string_arguments(Tray &t, DBusMessage *message) {
      std::vector<std::string> out;
      DBusMessageIter it {};
      if (!t.api->message_iter_init(message, &it)) {
        return out;
      }
      do {
        if (t.api->message_iter_get_arg_type(&it) == kTypeString) {
          const char *value = nullptr;
          t.api->message_iter_get_basic(&it, &value);
          out.emplace_back(value != nullptr ? value : "");
        }
      } while (t.api->message_iter_next(&it));
      return out;
    }

    std::int32_t first_int(Tray &t, DBusMessage *message) {
      DBusMessageIter it {};
      std::int32_t value = 0;
      if (t.api->message_iter_init(message, &it) && t.api->message_iter_get_arg_type(&it) == kTypeInt32) {
        t.api->message_iter_get_basic(&it, &value);
      }
      return value;
    }

    const char *kItemIntrospection =
      "<node>"
      "<interface name=\"org.kde.StatusNotifierItem\">"
      "<property name=\"Category\" type=\"s\" access=\"read\"/><property name=\"Id\" type=\"s\" access=\"read\"/>"
      "<property name=\"Title\" type=\"s\" access=\"read\"/><property name=\"Status\" type=\"s\" access=\"read\"/>"
      "<property name=\"WindowId\" type=\"i\" access=\"read\"/><property name=\"IconName\" type=\"s\" access=\"read\"/>"
      "<property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/><property name=\"OverlayIconName\" type=\"s\" access=\"read\"/>"
      "<property name=\"OverlayIconPixmap\" type=\"a(iiay)\" access=\"read\"/><property name=\"AttentionIconName\" type=\"s\" access=\"read\"/>"
      "<property name=\"AttentionIconPixmap\" type=\"a(iiay)\" access=\"read\"/><property name=\"AttentionMovieName\" type=\"s\" access=\"read\"/>"
      "<property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/><property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>"
      "<property name=\"Menu\" type=\"o\" access=\"read\"/>"
      "<method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>"
      "<method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>"
      "<method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>"
      "<method name=\"Scroll\"><arg name=\"delta\" type=\"i\" direction=\"in\"/><arg name=\"orientation\" type=\"s\" direction=\"in\"/></method>"
      "<signal name=\"NewTitle\"/><signal name=\"NewIcon\"/><signal name=\"NewAttentionIcon\"/><signal name=\"NewOverlayIcon\"/>"
      "<signal name=\"NewToolTip\"/><signal name=\"NewStatus\"><arg name=\"status\" type=\"s\"/></signal>"
      "</interface></node>";

    const char *kMenuIntrospection =
      "<node>"
      "<interface name=\"com.canonical.dbusmenu\">"
      "<property name=\"Version\" type=\"u\" access=\"read\"/><property name=\"TextDirection\" type=\"s\" access=\"read\"/>"
      "<property name=\"Status\" type=\"s\" access=\"read\"/><property name=\"IconThemePath\" type=\"as\" access=\"read\"/>"
      "<method name=\"GetLayout\"><arg type=\"i\" name=\"parentId\" direction=\"in\"/><arg type=\"i\" name=\"recursionDepth\" direction=\"in\"/>"
      "<arg type=\"as\" name=\"propertyNames\" direction=\"in\"/><arg type=\"u\" name=\"revision\" direction=\"out\"/>"
      "<arg type=\"(ia{sv}av)\" name=\"layout\" direction=\"out\"/></method>"
      "<method name=\"GetGroupProperties\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/><arg type=\"as\" name=\"propertyNames\" direction=\"in\"/>"
      "<arg type=\"a(ia{sv})\" name=\"properties\" direction=\"out\"/></method>"
      "<method name=\"GetProperty\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"name\" direction=\"in\"/>"
      "<arg type=\"v\" name=\"value\" direction=\"out\"/></method>"
      "<method name=\"Event\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"eventId\" direction=\"in\"/>"
      "<arg type=\"v\" name=\"data\" direction=\"in\"/><arg type=\"u\" name=\"timestamp\" direction=\"in\"/></method>"
      "<method name=\"EventGroup\"><arg type=\"a(isvu)\" name=\"events\" direction=\"in\"/><arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>"
      "<method name=\"AboutToShow\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"b\" name=\"needUpdate\" direction=\"out\"/></method>"
      "<method name=\"AboutToShowGroup\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/><arg type=\"ai\" name=\"updatesNeeded\" direction=\"out\"/>"
      "<arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>"
      "<signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\" name=\"updatedProps\"/><arg type=\"a(ias)\" name=\"removedProps\"/></signal>"
      "<signal name=\"LayoutUpdated\"><arg type=\"u\" name=\"revision\"/><arg type=\"i\" name=\"parent\"/></signal>"
      "</interface></node>";

    HandlerResult on_item(DBusConnection *, DBusMessage *message, void *data) {
      Tray &t = *static_cast<Tray *>(data);
      const Api &a = *t.api;
      if (a.message_is_method_call(message, kPropertiesInterface, "Get")) {
        const auto args = string_arguments(t, message);
        const std::string name = args.size() >= 2 ? args[1] : std::string();
        DBusMessage *answer = a.message_new_method_return(message);
        DBusMessageIter it {};
        a.message_iter_init_append(answer, &it);
        if (append_item_property(t, &it, name)) {
          a.connection_send(t.bus, answer, nullptr);
        } else {
          reply_error(t, message, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
        }
        a.message_unref(answer);
        return kHandled;
      }
      if (a.message_is_method_call(message, kPropertiesInterface, "GetAll")) {
        reply(t, message, [&](DBusMessageIter *it) {
          container(t, it, kTypeArray, "{sv}", [&](DBusMessageIter *dict) {
            for (const char *name : kItemProperties) {
              container(t, dict, kTypeDictEntry, nullptr, [&](DBusMessageIter *entry) {
                append_string(t, entry, name);
                append_item_property(t, entry, name);
              });
            }
          });
        });
        return kHandled;
      }
      if (a.message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        reply(t, message, [&](DBusMessageIter *it) {
          append_string(t, it, kItemIntrospection);
        });
        return kHandled;
      }
      // Clicks: the menu does everything (ItemIsMenu), so these just answer.
      for (const char *method : {"Activate", "SecondaryActivate", "ContextMenu", "Scroll"}) {
        if (a.message_is_method_call(message, kItemInterface, method)) {
          reply(t, message);
          return kHandled;
        }
      }
      return kNotYetHandled;
    }

    HandlerResult on_menu(DBusConnection *, DBusMessage *message, void *data) {
      Tray &t = *static_cast<Tray *>(data);
      const Api &a = *t.api;
      if (a.message_is_method_call(message, kMenuInterface, "GetLayout")) {
        const int parent = first_int(t, message);
        reply(t, message, [&](DBusMessageIter *it) {
          append_uint(t, it, t.revision);
          append_layout(t, it, find_item(t, parent) != nullptr ? parent : kMenuRoot);
        });
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "GetGroupProperties")) {
        std::vector<int> ids;
        DBusMessageIter it {};
        if (a.message_iter_init(message, &it) && a.message_iter_get_arg_type(&it) == kTypeArray) {
          DBusMessageIter array {};
          a.message_iter_recurse(&it, &array);
          while (a.message_iter_get_arg_type(&array) == kTypeInt32) {
            std::int32_t id = 0;
            a.message_iter_get_basic(&array, &id);
            ids.push_back(id);
            a.message_iter_next(&array);
          }
        }
        if (ids.empty()) {
          for (const auto &item : t.menu) {
            ids.push_back(item.id);
          }
        }
        reply(t, message, [&](DBusMessageIter *out) {
          container(t, out, kTypeArray, "(ia{sv})", [&](DBusMessageIter *array) {
            for (const int id : ids) {
              container(t, array, kTypeStruct, nullptr, [&](DBusMessageIter *entry) {
                append_int(t, entry, id);
                append_item_properties(t, entry, id);
              });
            }
          });
        });
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "GetProperty")) {
        reply_error(t, message, "org.freedesktop.DBus.Error.NotSupported", "Use GetGroupProperties");
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "Event")) {
        const int id = first_int(t, message);
        const auto args = string_arguments(t, message);
        reply(t, message);
        if (!args.empty() && args[0] == "clicked") {
          clicked(t, id);
        }
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "EventGroup")) {
        std::vector<int> clicks;
        DBusMessageIter it {};
        if (a.message_iter_init(message, &it) && a.message_iter_get_arg_type(&it) == kTypeArray) {
          DBusMessageIter array {};
          a.message_iter_recurse(&it, &array);
          while (a.message_iter_get_arg_type(&array) == kTypeStruct) {
            DBusMessageIter event {};
            a.message_iter_recurse(&array, &event);
            std::int32_t id = 0;
            const char *kind = nullptr;
            if (a.message_iter_get_arg_type(&event) == kTypeInt32) {
              a.message_iter_get_basic(&event, &id);
              a.message_iter_next(&event);
              if (a.message_iter_get_arg_type(&event) == kTypeString) {
                a.message_iter_get_basic(&event, &kind);
              }
            }
            if (kind != nullptr && std::string(kind) == "clicked") {
              clicks.push_back(id);
            }
            a.message_iter_next(&array);
          }
        }
        reply(t, message, [&](DBusMessageIter *out) {
          container(t, out, kTypeArray, "i", [](DBusMessageIter *) {});
        });
        for (const int id : clicks) {
          clicked(t, id);
        }
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "AboutToShow")) {
        refresh(t);
        reply(t, message, [&](DBusMessageIter *it) {
          append_bool(t, it, false);
        });
        return kHandled;
      }
      if (a.message_is_method_call(message, kMenuInterface, "AboutToShowGroup")) {
        refresh(t);
        reply(t, message, [&](DBusMessageIter *it) {
          container(t, it, kTypeArray, "i", [](DBusMessageIter *) {});
          container(t, it, kTypeArray, "i", [](DBusMessageIter *) {});
        });
        return kHandled;
      }
      if (a.message_is_method_call(message, kPropertiesInterface, "Get") || a.message_is_method_call(message, kPropertiesInterface, "GetAll")) {
        const auto args = string_arguments(t, message);
        const bool all = a.message_is_method_call(message, kPropertiesInterface, "GetAll");
        auto append_menu_property = [&](DBusMessageIter *it, const std::string &name) {
          if (name == "Version") {
            variant(t, it, "u", [&](DBusMessageIter *v) {
              append_uint(t, v, 3);
            });
          } else if (name == "TextDirection") {
            variant(t, it, "s", [&](DBusMessageIter *v) {
              append_string(t, v, "ltr");
            });
          } else if (name == "Status") {
            variant(t, it, "s", [&](DBusMessageIter *v) {
              append_string(t, v, "normal");
            });
          } else if (name == "IconThemePath") {
            variant(t, it, "as", [&](DBusMessageIter *v) {
              container(t, v, kTypeArray, "s", [](DBusMessageIter *) {});
            });
          } else {
            return false;
          }
          return true;
        };
        if (all) {
          reply(t, message, [&](DBusMessageIter *it) {
            container(t, it, kTypeArray, "{sv}", [&](DBusMessageIter *dict) {
              for (const char *name : {"Version", "TextDirection", "Status", "IconThemePath"}) {
                container(t, dict, kTypeDictEntry, nullptr, [&](DBusMessageIter *entry) {
                  append_string(t, entry, name);
                  append_menu_property(entry, name);
                });
              }
            });
          });
        } else {
          const std::string name = args.size() >= 2 ? args[1] : std::string();
          if (name == "Version" || name == "TextDirection" || name == "Status" || name == "IconThemePath") {
            reply(t, message, [&](DBusMessageIter *it) {
              append_menu_property(it, name);
            });
          } else {
            reply_error(t, message, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
          }
        }
        return kHandled;
      }
      if (a.message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        reply(t, message, [&](DBusMessageIter *it) {
          append_string(t, it, kMenuIntrospection);
        });
        return kHandled;
      }
      return kNotYetHandled;
    }

    /** Tell the desktop's tray (the StatusNotifierWatcher) about the icon. */
    void register_with_watcher(Tray &t) {
      DBusMessage *call = t.api->message_new_method_call(kWatcherName, "/StatusNotifierWatcher", kWatcherName, "RegisterStatusNotifierItem");
      if (call == nullptr) {
        return;
      }
      DBusMessageIter it {};
      t.api->message_iter_init_append(call, &it);
      append_string(t, &it, t.item_name);
      t.api->message_set_no_reply(call, 1);
      t.api->connection_send(t.bus, call, nullptr);
      t.api->message_unref(call);
    }

    /** The tray (re)started, for example when Plasma restarts: register again. */
    HandlerResult on_signal(DBusConnection *, DBusMessage *message, void *data) {
      Tray &t = *static_cast<Tray *>(data);
      if (t.api->message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const auto args = string_arguments(t, message);
        if (args.size() == 3 && args[0] == kWatcherName && !args[2].empty()) {
          register_with_watcher(t);
        }
      }
      return kNotYetHandled;
    }

    /** As root: run 'inputline-host tray' in each signed-in user's desktop session. */
    void launch_for_signed_in_users() {
      std::error_code error;
      const fs::path self = fs::read_symlink("/proc/self/exe", error);
      if (error) {
        return;
      }
      const std::string systemd_run = find_program("systemd-run");
      const std::string runuser = find_program("runuser");
      for (const auto &entry : fs::directory_iterator("/run/user", error)) {
        const std::string uid = entry.path().filename().string();
        if (uid.empty() || uid.find_first_not_of("0123456789") != std::string::npos || std::stoul(uid) < 1000 ||
            !fs::exists(entry.path() / "bus", error)) {
          continue;
        }
        std::string name;
        {
          // The user name, for systemd-run and runuser.
          std::ifstream passwd("/etc/passwd");
          std::string line;
          while (std::getline(passwd, line)) {
            const auto first = line.find(':');
            const auto second = line.find(':', first + 1);
            const auto third = line.find(':', second + 1);
            if (first != std::string::npos && third != std::string::npos && line.substr(second + 1, third - second - 1) == uid) {
              name = line.substr(0, first);
              break;
            }
          }
        }
        if (name.empty()) {
          continue;
        }
        // In the user's own systemd manager, it gets the desktop's environment
        // (needed to open links). The icon refuses to run twice.
        std::string output;
        if (!systemd_run.empty() &&
            run_process({systemd_run, "--user", "--machine=" + name + "@.host", "--collect", "--quiet", "--unit=inputline-tray", self.string(), "tray"},
                        &output) == 0) {
          continue;
        }
        if (!runuser.empty()) {
          run_process({runuser, "-u", name, "--", "env", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + uid + "/bus", "setsid", "-f",
                       self.string(), "tray"});
        }
      }
    }
  }  // namespace

  void start_agents() {
    if (::geteuid() == 0) {
      std::thread(launch_for_signed_in_users).detach();
    }
  }

  void session_changed(unsigned long, unsigned long) {}

  void stop_agents() {}

  bool launch_in_user_session(const std::string &) {
    return false;
  }

  void run_message_loop() {}

  bool forward_notification(const std::wstring &, const std::wstring &, const std::wstring &) {
    return false;
  }

  int run_tray(bool show) {
    std::error_code error;
    if (show) {
      fs::remove(hidden_flag(), error);
    } else if (fs::exists(hidden_flag(), error)) {
      return 0;  // the user hid it
    }
    std::signal(SIGCHLD, SIG_IGN);  // programs it opens are reaped automatically

    Tray t;
    t.api = dbus::api();
    if (t.api == nullptr) {
      log::error("tray: libdbus-1 isn't available, so there's no tray icon");
      return 1;
    }
    DBusError failure {};
    t.api->error_init(&failure);
    t.bus = t.api->bus_get(kBusSession, &failure);
    if (t.bus == nullptr) {
      log::error("tray: no desktop session bus: ", failure.message != nullptr ? failure.message : "unknown error");
      t.api->error_free(&failure);
      return 1;
    }
    t.api->connection_set_exit_on_disconnect(t.bus, 0);
    if (t.api->bus_request_name(t.bus, kInstanceName, kNameFlagDoNotQueue, &failure) != kRequestNamePrimaryOwner) {
      t.api->error_free(&failure);
      return 0;  // already showing in this session
    }
    t.item_name = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
    t.api->bus_request_name(t.bus, t.item_name.c_str(), kNameFlagDoNotQueue, &failure);
    t.api->error_free(&failure);

    static const ObjectPathVTable item_table {nullptr, on_item, nullptr, nullptr, nullptr, nullptr};
    static const ObjectPathVTable menu_table {nullptr, on_menu, nullptr, nullptr, nullptr, nullptr};
    t.api->connection_register_object_path(t.bus, kItemPath, &item_table, &t);
    t.api->connection_register_object_path(t.bus, kMenuPath, &menu_table, &t);
    t.api->connection_add_filter(t.bus, on_signal, &t, nullptr);
    t.api->bus_add_match(t.bus,
                         "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                         "arg0='org.kde.StatusNotifierWatcher'",
                         nullptr);

    refresh(t, true);
    register_with_watcher(t);
    auto next_refresh = std::chrono::steady_clock::now();
    while (!t.quit) {
      if (!t.api->connection_read_write_dispatch(t.bus, 500)) {
        break;  // the session ended
      }
      if (std::chrono::steady_clock::now() >= next_refresh) {
        next_refresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        refresh(t);
      }
    }
    t.api->connection_flush(t.bus);
    return 0;
  }

}  // namespace inputline::tray
