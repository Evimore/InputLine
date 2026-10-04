#include "dbus_lite.h"

#include <dlfcn.h>

#include <mutex>

namespace inputline::dbus {

  namespace {
    template <typename T>
    bool load(void *library, const char *name, T &out) {
      out = reinterpret_cast<T>(::dlsym(library, name));
      return out != nullptr;
    }

    bool load_all(void *library, Api &a) {
      return load(library, "dbus_error_init", a.error_init) && load(library, "dbus_error_free", a.error_free) &&
             load(library, "dbus_error_is_set", a.error_is_set) && load(library, "dbus_bus_get", a.bus_get) &&
             load(library, "dbus_bus_request_name", a.bus_request_name) && load(library, "dbus_bus_add_match", a.bus_add_match) &&
             load(library, "dbus_connection_register_object_path", a.connection_register_object_path) &&
             load(library, "dbus_connection_add_filter", a.connection_add_filter) &&
             load(library, "dbus_connection_read_write_dispatch", a.connection_read_write_dispatch) &&
             load(library, "dbus_connection_send", a.connection_send) && load(library, "dbus_connection_flush", a.connection_flush) &&
             load(library, "dbus_connection_set_exit_on_disconnect", a.connection_set_exit_on_disconnect) &&
             load(library, "dbus_message_new_method_call", a.message_new_method_call) &&
             load(library, "dbus_message_new_method_return", a.message_new_method_return) &&
             load(library, "dbus_message_new_error", a.message_new_error) && load(library, "dbus_message_new_signal", a.message_new_signal) &&
             load(library, "dbus_message_unref", a.message_unref) && load(library, "dbus_message_is_method_call", a.message_is_method_call) &&
             load(library, "dbus_message_is_signal", a.message_is_signal) && load(library, "dbus_message_set_no_reply", a.message_set_no_reply) &&
             load(library, "dbus_message_iter_init", a.message_iter_init) &&
             load(library, "dbus_message_iter_init_append", a.message_iter_init_append) &&
             load(library, "dbus_message_iter_append_basic", a.message_iter_append_basic) &&
             load(library, "dbus_message_iter_append_fixed_array", a.message_iter_append_fixed_array) &&
             load(library, "dbus_message_iter_open_container", a.message_iter_open_container) &&
             load(library, "dbus_message_iter_close_container", a.message_iter_close_container) &&
             load(library, "dbus_message_iter_get_arg_type", a.message_iter_get_arg_type) &&
             load(library, "dbus_message_iter_get_basic", a.message_iter_get_basic) &&
             load(library, "dbus_message_iter_next", a.message_iter_next) && load(library, "dbus_message_iter_recurse", a.message_iter_recurse);
    }
  }  // namespace

  const Api *api() {
    static std::once_flag once;
    static Api table {};
    static bool loaded = false;
    std::call_once(once, [] {
      void *library = ::dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
      if (library != nullptr && load_all(library, table)) {
        loaded = true;
      }
    });
    return loaded ? &table : nullptr;
  }

}  // namespace inputline::dbus
