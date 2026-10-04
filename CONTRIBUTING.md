# Contributing

Issues and pull requests are welcome. The most useful contributions right now are reports from real hardware: see *Next* in the [roadmap](docs/roadmap.md).

## Reporting a hardware result

Open an issue with the **Test report** form: it asks for the details below.

- PC OS and version, Steam client version, usbip-win2 version
- iPad / iPhone model and iOS version
- Controller firmware, if you know it
- InputLine's **Share report** and `inputline-host.log`

Both include your PC's name, IP addresses and the controller's serial number. Issues are public, so look them over first and replace anything you'd rather not share.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

- C++17, no third-party dependencies in `core/` or `host/`.
- New behaviour comes with a test. `host/tests/` has an in-process USB/IP client and a UDP test client for end-to-end tests.
- CI runs the tests under ASan/UBSan and TSan. Please keep both clean.
- CI also fuzzes what the network and other programs can reach: the link protocol, the link server (including messages from a paired client) and the USB/IP server. To fuzz locally (clang with libFuzzer):
  ```sh
  cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DINPUTLINE_FUZZ=ON -DINPUTLINE_BUILD_TESTS=OFF
  cmake --build build-fuzz
  build-fuzz/fuzz/fuzz_link_server -max_total_time=300
  ```
- The InputLine app builds with XcodeGen (`clients/inputline-ios/project.yml`); CI builds it on every push.

## Releasing

On GitHub: **Actions → Release → Run workflow**, enter the version (for example `0.2.0`) and run it. It builds the Windows installer (`InputLine-Setup-vX.msi`), a portable Windows zip, the Linux build (`inputline-vX-linux-x86_64.tar.gz`) and the unsigned `InputLine-vX-iOS.ipa` from the latest `main`, then publishes them on the **Releases** page under the tag `v0.2.0`, with notes generated from the changes since the last release. A version such as `0.2.0-beta.1` becomes a pre-release. Pushing a `v…` tag does the same.

By contributing you agree that your contribution is licensed under the licence of the part you change: MIT for `core/` and `clients/`, GPL-3.0-or-later for `host/`.
