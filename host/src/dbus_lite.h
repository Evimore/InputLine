/**
 * @file dbus_lite.h
 * @brief The few libdbus-1 calls the Linux tray icon needs, loaded at run
 *        time with dlopen: no build dependency, and the rest of InputLine
 *        works without it. The declarations follow libdbus's public headers.
 */
#pragma once

#include <cstdint>

namespace inputline::dbus {

  struct DBusConnection;
  struct DBusMessage;

  using dbus_bool_t = std::uint32_t;

  /** DBusError, as in dbus-errors.h. */
  struct DBusError {
    const char *name;
    const char *message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void *padding1;
  };

  /** DBusMessageIter, as in dbus-message.h (64-bit and 32-bit layouts). */
  struct DBusMessageIter {
    void *dummy1;
    void *dummy2;
    std::uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void *pad2;
    void *pad3;
  };

  enum HandlerResult : int {
    kHandled = 0,
    kNotYetHandled = 1,
  };

  using MessageFunction = HandlerResult (*)(DBusConnection *, DBusMessage *, void *);

  /** DBusObjectPathVTable. */
  struct ObjectPathVTable {
    void (*unregister_function)(DBusConnection *, void *);
    MessageFunction message_function;
    void (*pad1)(void *);
    void (*pad2)(void *);
    void (*pad3)(void *);
    void (*pad4)(void *);
  };

  // Type codes and constants (dbus-protocol.h, dbus-shared.h).
  inline constexpr int kTypeByte = 'y';
  inline constexpr int kTypeBoolean = 'b';
  inline constexpr int kTypeInt32 = 'i';
  inline constexpr int kTypeUint32 = 'u';
  inline constexpr int kTypeString = 's';
  inline constexpr int kTypeObjectPath = 'o';
  inline constexpr int kTypeArray = 'a';
  inline constexpr int kTypeVariant = 'v';
  inline constexpr int kTypeStruct = 'r';
  inline constexpr int kTypeDictEntry = 'e';
  inline constexpr int kTypeInvalid = 0;
  inline constexpr int kBusSession = 0;
  inline constexpr unsigned int kNameFlagDoNotQueue = 0x4;
  inline constexpr int kRequestNamePrimaryOwner = 1;

  struct Api {
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    dbus_bool_t (*error_is_set)(const DBusError *);
    DBusConnection *(*bus_get)(int, DBusError *);
    int (*bus_request_name)(DBusConnection *, const char *, unsigned int, DBusError *);
    void (*bus_add_match)(DBusConnection *, const char *, DBusError *);
    dbus_bool_t (*connection_register_object_path)(DBusConnection *, const char *, const ObjectPathVTable *, void *);
    dbus_bool_t (*connection_add_filter)(DBusConnection *, MessageFunction, void *, void (*)(void *));
    dbus_bool_t (*connection_read_write_dispatch)(DBusConnection *, int);
    dbus_bool_t (*connection_send)(DBusConnection *, DBusMessage *, std::uint32_t *);
    void (*connection_flush)(DBusConnection *);
    void (*connection_set_exit_on_disconnect)(DBusConnection *, dbus_bool_t);
    DBusMessage *(*message_new_method_call)(const char *, const char *, const char *, const char *);
    DBusMessage *(*message_new_method_return)(DBusMessage *);
    DBusMessage *(*message_new_error)(DBusMessage *, const char *, const char *);
    DBusMessage *(*message_new_signal)(const char *, const char *, const char *);
    void (*message_unref)(DBusMessage *);
    dbus_bool_t (*message_is_method_call)(DBusMessage *, const char *, const char *);
    dbus_bool_t (*message_is_signal)(DBusMessage *, const char *, const char *);
    void (*message_set_no_reply)(DBusMessage *, dbus_bool_t);
    dbus_bool_t (*message_iter_init)(DBusMessage *, DBusMessageIter *);
    void (*message_iter_init_append)(DBusMessage *, DBusMessageIter *);
    dbus_bool_t (*message_iter_append_basic)(DBusMessageIter *, int, const void *);
    dbus_bool_t (*message_iter_append_fixed_array)(DBusMessageIter *, int, const void *, int);
    dbus_bool_t (*message_iter_open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    dbus_bool_t (*message_iter_close_container)(DBusMessageIter *, DBusMessageIter *);
    int (*message_iter_get_arg_type)(DBusMessageIter *);
    void (*message_iter_get_basic)(DBusMessageIter *, void *);
    dbus_bool_t (*message_iter_next)(DBusMessageIter *);
    void (*message_iter_recurse)(DBusMessageIter *, DBusMessageIter *);
  };

  /** libdbus-1, loaded on first use; nullptr if it isn't installed. */
  const Api *api();

}  // namespace inputline::dbus
