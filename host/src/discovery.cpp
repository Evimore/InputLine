#include "discovery.h"

#include "log.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#ifdef _WIN32
  #include <windows.h>
#else
  #include "desktop.h"
  #include "usbip_attach.h"  // find_program

  #include <csignal>
  #include <fcntl.h>
  #include <spawn.h>
  #include <string>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>
extern char **environ;
#endif

namespace inputline::discovery {

  std::string instance_label(const std::string &name) {
    std::string label;
    for (char c : name) {
      label.push_back(c == '.' ? '-' : c);
    }
    if (label.size() > 63) {
      std::size_t cut = 63;
      while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) {
        --cut;  // don't split a UTF-8 sequence
      }
      label.resize(cut);
    }
    return label.empty() ? "inputline-host" : label;
  }

#ifdef _WIN32

  namespace {
    // The DNS-SD API is in dnsapi.dll since Windows 10 1809. It is loaded at
    // run time, so older Windows still runs inputline-host (without discovery)
    // and the build does not depend on the SDK version's windns.h.
    constexpr ULONG kRequestVersion1 = 1;  // DNS_QUERY_REQUEST_VERSION1
    constexpr DWORD kRequestPending = 9506;  // DNS_REQUEST_PENDING

    using RegisterComplete = VOID(WINAPI *)(DWORD status, PVOID context, PVOID instance);

    struct RegisterRequest {  // DNS_SERVICE_REGISTER_REQUEST
      ULONG version;
      ULONG interface_index;
      PVOID instance;
      RegisterComplete callback;
      PVOID context;
      HANDLE credentials;
      BOOL unicast_enabled;
    };

    using ConstructInstance = PVOID(WINAPI *)(
      PCWSTR service_name, PCWSTR host_name, PVOID ip4, PVOID ip6, WORD port, WORD priority, WORD weight,
      DWORD property_count, PCWSTR *keys, PCWSTR *values
    );
    using FreeInstance = VOID(WINAPI *)(PVOID instance);
    using Register = DWORD(WINAPI *)(RegisterRequest *request, PVOID cancel);

    std::wstring widen(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
      return out;
    }

    template<typename T>
    T load(HMODULE module, const char *name) {
      // Casting through void (*)() keeps GCC's -Wcast-function-type quiet.
      return reinterpret_cast<T>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
    }
  }  // namespace

  struct Advertiser::Impl {
    HMODULE dnsapi = nullptr;
    ConstructInstance construct = nullptr;
    FreeInstance free_instance = nullptr;
    Register register_service = nullptr;
    Register deregister_service = nullptr;

    PVOID instance = nullptr;
    RegisterRequest request {};

    std::mutex mutex;
    std::condition_variable changed;
    bool deregistered = false;

    static VOID WINAPI on_registered(DWORD status, PVOID context, PVOID instance) {
      auto *self = static_cast<Impl *>(context);
      if (instance != nullptr) {
        self->free_instance(instance);
      }
      if (status == ERROR_SUCCESS) {
        log::info("discovery: InputLine apps on this network can now find this PC");
      } else {
        log::warn("discovery: announcing this PC failed (error ", status, "); enter its address in InputLine instead");
      }
    }

    static VOID WINAPI on_deregistered(DWORD, PVOID context, PVOID instance) {
      auto *self = static_cast<Impl *>(context);
      if (instance != nullptr) {
        self->free_instance(instance);
      }
      std::lock_guard lock(self->mutex);
      self->deregistered = true;
      self->changed.notify_all();
    }

    ~Impl() {
      if (instance != nullptr) {
        free_instance(instance);
      }
      if (dnsapi != nullptr) {
        FreeLibrary(dnsapi);
      }
    }
  };

  Advertiser::Advertiser() = default;

  Advertiser::~Advertiser() {
    stop();
  }

  bool Advertiser::start(const std::string &name, std::uint16_t port) {
    stop();
    auto impl = std::make_unique<Impl>();
    impl->dnsapi = LoadLibraryW(L"dnsapi.dll");
    if (impl->dnsapi != nullptr) {
      impl->construct = load<ConstructInstance>(impl->dnsapi, "DnsServiceConstructInstance");
      impl->free_instance = load<FreeInstance>(impl->dnsapi, "DnsServiceFreeInstance");
      impl->register_service = load<Register>(impl->dnsapi, "DnsServiceRegister");
      impl->deregister_service = load<Register>(impl->dnsapi, "DnsServiceDeRegister");
    }
    if (!impl->construct || !impl->free_instance || !impl->register_service || !impl->deregister_service) {
      log::info("discovery: not available on this Windows version; enter this PC's address in InputLine");
      return false;
    }

    wchar_t computer[256] = {};
    DWORD computer_length = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer, &computer_length) || computer_length == 0) {
      log::warn("discovery: could not read this PC's host name");
      return false;
    }
    const std::wstring service = widen(instance_label(name)) + L"." + widen(kServiceType) + L".local";
    const std::wstring host = std::wstring(computer) + L".local";
    PCWSTR keys[] = {L"v"};
    PCWSTR values[] = {L"1"};
    impl->instance = impl->construct(service.c_str(), host.c_str(), nullptr, nullptr, port, 0, 0, 1, keys, values);
    if (impl->instance == nullptr) {
      log::warn("discovery: could not describe the service");
      return false;
    }

    impl->request.version = kRequestVersion1;
    impl->request.instance = impl->instance;
    impl->request.callback = &Impl::on_registered;
    impl->request.context = impl.get();
    const DWORD status = impl->register_service(&impl->request, nullptr);
    if (status != kRequestPending) {
      log::warn("discovery: announcing this PC failed (error ", status, "); enter its address in InputLine instead");
      return false;
    }
    log::debug("discovery: announcing '", instance_label(name), "' as ", kServiceType, " on port ", port);
    impl_ = impl.release();
    return true;
  }

  void Advertiser::stop() {
    if (impl_ == nullptr) {
      return;
    }
    Impl *impl = impl_;
    impl_ = nullptr;
    impl->request.callback = &Impl::on_deregistered;
    if (impl->deregister_service(&impl->request, nullptr) != kRequestPending) {
      delete impl;
      return;
    }
    std::unique_lock lock(impl->mutex);
    if (impl->changed.wait_for(lock, std::chrono::seconds(2), [impl] { return impl->deregistered; })) {
      lock.unlock();
      delete impl;
    }
    // Otherwise Windows may still call back into impl: leave it allocated.
  }

#else

  // Linux: Avahi's avahi-publish announces the service for as long as it runs.
  // It is restarted if it stops (for example when avahi-daemon restarts).
  struct Advertiser::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    pid_t child = -1;
    std::thread worker;

    /** Wait up to @p duration; false if stop() was called. */
    bool pause(std::chrono::milliseconds duration) {
      std::unique_lock lock(mutex);
      return !wake.wait_for(lock, duration, [this] {
        return stopping;
      });
    }

    pid_t spawn(const std::vector<std::string> &argv) {
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
      std::vector<char *> raw;
      for (const auto &arg : argv) {
        raw.push_back(const_cast<char *>(arg.c_str()));
      }
      raw.push_back(nullptr);
      pid_t pid = -1;
      const int failed = posix_spawn(&pid, raw[0], &actions, nullptr, raw.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      return failed == 0 ? pid : -1;
    }

    void run(const std::string &label, std::uint16_t port) {
      bool warned = false;
      bool announced = false;
      while (true) {
        const std::string publish = find_program("avahi-publish");
        if (publish.empty()) {
          if (!warned) {
            warned = true;
            log::warn("discovery: Avahi isn't installed, so InputLine apps won't list this PC (type its address instead). Install it: ",
                      desktop::install_hint(desktop::Package::kAvahi));
          }
          if (!pause(std::chrono::seconds(60))) {
            return;
          }
          continue;
        }
        {
          std::lock_guard lock(mutex);
          if (stopping) {
            return;
          }
          child = spawn({publish, "-s", label, kServiceType, std::to_string(port)});
        }
        const auto started = std::chrono::steady_clock::now();
        int status = 0;
        pid_t done = 0;
        // Still running after 2 s: Avahi took it.
        while ((done = ::waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (done == 0) {
          if (!announced) {
            announced = true;
            log::info("discovery: InputLine apps on this network can now find this PC");
          }
          ::waitpid(child, &status, 0);
        }
        {
          std::lock_guard lock(mutex);
          child = -1;
          if (stopping) {
            return;
          }
        }
        if (std::chrono::steady_clock::now() - started < std::chrono::seconds(5) && !warned) {
          warned = true;
          log::warn("discovery: Avahi didn't take the announcement; is avahi-daemon running? ", desktop::install_hint(desktop::Package::kAvahi),
                    ". Meanwhile, type this PC's address in InputLine.");
        }
        if (!pause(std::chrono::seconds(10))) {
          return;
        }
      }
    }
  };

  Advertiser::Advertiser() = default;

  Advertiser::~Advertiser() {
    stop();
  }

  bool Advertiser::start(const std::string &name, std::uint16_t port) {
    if (impl_ != nullptr) {
      return true;
    }
    impl_ = new Impl;
    const std::string label = instance_label(name);
    impl_->worker = std::thread([impl = impl_, label, port] {
      impl->run(label, port);
    });
    return true;
  }

  void Advertiser::stop() {
    if (impl_ == nullptr) {
      return;
    }
    {
      std::lock_guard lock(impl_->mutex);
      impl_->stopping = true;
      if (impl_->child > 0) {
        ::kill(impl_->child, SIGTERM);
      }
    }
    impl_->wake.notify_all();
    impl_->worker.join();
    delete impl_;
    impl_ = nullptr;
  }

#endif

}  // namespace inputline::discovery
