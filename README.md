<p align="center"><img src="docs/images/icon.png" width="128" height="128" alt="InputLine icon"></p>

<h1 align="center">InputLine</h1>

<p align="center"><b>Stream with Moonlight, keep your Steam Controller.</b><br>
Full Steam Input on your PC while you play on an iPad or iPhone, with any streaming app.</p>

---

Steam Link is the only streaming app that brings the Steam Controller across with everything Steam Input can do. Moonlight and other streaming apps only see standard gamepads, so the trackpads, gyro and back buttons are lost.

**InputLine closes that gap.** A small app on your iPad or iPhone reads the controller over Bluetooth and sends it straight to your PC, where Steam sees a real, wired Steam Controller. Your streaming app doesn't need to know: it streams video and audio as usual, while InputLine runs in the background.

- **Everything Steam Input offers:** both trackpads with pressure and haptics, gyro, the four back buttons, capacitive sticks and grips, and your per-game layouts.
- **Works with any streaming app and host:** Moonlight with Sunshine, Apollo or Vibepollo, unmodified.
- **Easy to live with:** the PC side runs in the background (a Windows installer, or one command on Linux); the app finds your PC on the network and reconnects the controller by itself.
- **Free and open source.**

> **Status: public beta.** It works end to end with a 2026 Steam Controller on Windows 11 and Linux (CachyOS, Fedora), with an iPad or an iPhone. Steam shows full Steam Input, every input works, and it feels the same as Steam Link. Apple TV (through an iPhone) and Windows 10 haven't been tested yet, nor long sessions with an iPhone's screen locked. If you try InputLine, please [say how it went](../../issues/new/choose), whether it worked or not. The app isn't on the App Store yet, so you install it yourself (free, from Windows). See the [roadmap](docs/roadmap.md).

```
 iPad / iPhone                                        PC
┌──────────────────────────────┐            ┌──────────────────────────────────────┐
│ Steam Controller ──BLE──►    │            │ InputLine service                    │
│ InputLine (in the background)│ ─────────► │   └► virtual wired Steam Controller  │
│                              │ ◄───────── │         └► Steam Input ─► your game  │
│ (haptics played back)        │  haptics   │                                      │
└──────────────────────────────┘            └──────────────────────────────────────┘
        Moonlight (or any streaming app) streams video and audio as usual.
```

## What you need

- A **2026 Steam Controller**
- An **iPad or iPhone** with iOS 15 or later (for Apple TV, see [below](#apple-tv))
- A **Windows 10 (1809+) or 11 PC**, or a **Linux PC** (CachyOS, Arch, Ubuntu, Fedora...), with Steam, and a streaming host such as [Sunshine](https://github.com/LizardByte/Sunshine), [Apollo](https://github.com/ClassicOldSong/Apollo) or [Vibepollo](https://github.com/Nonary/Vibepollo)
- A streaming app on the iPad or iPhone, such as [Moonlight](https://moonlight-stream.org)

## Get started

### 1. Set up the PC

1. Install [**usbip-win2**](https://github.com/vadimgrn/usbip-win2/releases) (0.9.7 or newer). It lets InputLine plug a virtual controller into Windows; its drivers are signed by Microsoft.
2. Download the **InputLine-Setup** installer (`.msi`) from [Releases](../../releases) and run it. If Windows says it "protected your PC", click **More info → Run anyway** (the installer isn't code-signed yet).

The order doesn't matter: the installer's last screen says whether usbip-win2 is there, and until it is, InputLine's icon next to the clock shows a red mark with a download link.

> If Windows' **Smart App Control** is on, it blocks part of usbip-win2 and Steam never sees the controller. See [Troubleshooting](docs/host-setup-windows.md#troubleshooting).

That's all on the PC. InputLine now runs in the background and starts with Windows. More in the [Windows setup guide](docs/host-setup-windows.md).

**On Linux:** install `usbip` and Avahi, then run `sudo ./inputline-host install` from the Linux download. It sets InputLine up as a background service the same way. Step by step: [Linux setup](docs/host-setup-linux.md).

### 2. Install the app

Download the **InputLine** app (`InputLine-…-iOS.ipa`) from [Releases](../../releases) and install it from your PC with Sideloadly and a free Apple ID. Step by step: [Install InputLine](docs/install-app.md).

With a free Apple ID, apps you install yourself stop opening after **7 days** until they're re-signed. Sideloadly can re-sign InputLine by itself while the iPad or iPhone is on the same Wi-Fi as the PC; your settings and pairing are kept.

### 3. Pair and play

1. Open InputLine and tap your PC under **Found on this network**.
2. A 6-digit code pops up on the PC's screen. Type it into InputLine.
3. Switch on the controller. If it isn't paired with the iPad or iPhone yet, put it in pairing mode and tap **Pair a new controller**.
4. Open Moonlight (or your streaming app) and start playing. Steam sees a wired Steam Controller.

From then on, just switch the controller on and stream. Leave InputLine in the background; don't swipe it away in the app switcher.

### Apple TV

Not tested yet. tvOS doesn't let apps use Bluetooth in the background, so InputLine runs on an iPhone (or iPad) near you instead: pair the controller with the iPhone, keep the iPhone locked in your pocket, and stream on the Apple TV as usual. Use the Siri Remote to leave the stream. Details: [Apple TV](docs/install-app.md#apple-tv).

## Questions

**Does it add lag?**
Very little. iOS reads the controller over Bluetooth every 15 ms, for every app, Steam Link included, and the hop to the PC takes a millisecond or two on a home network. A side-by-side measurement against Steam Link is on the [roadmap](docs/roadmap.md). See [Checking smoothness](docs/timing.md).

**Do I need to change Moonlight or my streaming host?**
No. InputLine runs next to them and talks to the PC on its own.

**Can I play away from home?**
Yes, over a VPN such as [Tailscale](https://tailscale.com): enter the PC's VPN address in InputLine once. It remembers every address where it reached your PC and switches between them by itself.

**Every button press counts twice.**
Check *Steam → Settings → Controller*. If an Xbox or PlayStation controller is listed next to the Steam Controller, your streaming app may be forwarding a second copy of it. Turn off gamepad input in your streaming host (in Sunshine, Apollo and Vibepollo: *Configuration → Input → Enable Gamepad Input*).

**Can the streaming app's controller shortcuts (like the quit combo) be used?**
No, because the streaming app doesn't see the controller. Leave the stream with a touch gesture on the iPad or iPhone, or the Siri Remote on Apple TV.

**Is the connection secure?**
Yes. Pairing uses a key exchange protected by the code shown on your PC, and everything after that is encrypted, like Moonlight and Steam Link do. See [SECURITY.md](SECURITY.md).

**Can I use the controller on the iPad itself?**
Tap **Disconnect from PC** in InputLine and it works as the iPad's mouse again. Tap **Connect to PC**, or switch the controller off and on, to send it back to the PC.

**Is it safe for my controller?**
InputLine passes settings, haptics, calibration and turning it off to your controller, but never firmware updates, factory resets, or pairing and radio changes: those could leave it unreachable over Bluetooth. To update the controller's firmware, plug it into the PC. See [SECURITY.md](SECURITY.md) for how the connection is protected.

**Why isn't it on the App Store?**
It will be. Until then, a free Apple ID lets you install it yourself; see [Install InputLine](docs/install-app.md).

## Documentation

- [Windows setup](docs/host-setup-windows.md): the installer, settings and troubleshooting
- [Linux setup](docs/host-setup-linux.md): CachyOS, Arch and other distributions
- [Install InputLine](docs/install-app.md): sideloading, updates, Apple TV
- [Checking smoothness](docs/timing.md): timing statistics on the device and the PC
- [Architecture](docs/architecture.md) and [link protocol](docs/protocol.md)
- [Roadmap](docs/roadmap.md)

## For developers

| Component | What it does |
|---|---|
| [`core/`](core) | Portable C++17, no dependencies. Controller report layouts, Bluetooth → wired report conversion, Steam's identity handshake, the authenticated link protocol, timing statistics. |
| [`host/`](host) | `inputline-host`: the link server plus a small USB/IP device server that exports a virtual `28DE:1302` Steam Controller, which usbip-win2 (Windows) or vhci-hcd (Linux) attaches. Runs as a Windows or systemd service. `inputline-sim` stands in for the app when testing. |
| [`clients/inputline-ios/`](clients/inputline-ios) | The InputLine app for iPad and iPhone. |
| [`clients/apple-shared/`](clients/apple-shared) | CoreBluetooth driver for Valve's controller protocol, with background state restoration. |
| [`installer/windows/`](installer/windows) | The WiX installer. |

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This builds on Windows (MSVC), Linux and macOS. CI tests everything on every push, including the installer on Windows, installing and pairing through the Linux service, and the whole path through Linux's USB/IP and SDL's Steam Controller driver. See [CONTRIBUTING.md](CONTRIBUTING.md), which also explains how releases are made.

## Credits

InputLine builds on work by the SDL team and Valve (Steam Controller protocol), [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) (the virtual Steam Controller's USB identity), [OpenPuck](https://github.com/safijari/openpuck), and [usbip-win2](https://github.com/vadimgrn/usbip-win2). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

| Part | Licence |
|---|---|
| `core/`, `clients/apple-shared/`, `clients/inputline-ios/` (the app) | [MIT](core/LICENSE) |
| `host/` (`inputline-host`), `installer/` | [GPL-3.0-or-later](LICENSE) |

Not affiliated with or endorsed by Valve Corporation, the Moonlight project or any streaming host project. Steam and Steam Controller are trademarks of Valve Corporation.
