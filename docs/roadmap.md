# Roadmap

## Done

- [x] Shared core: report conversion, Steam's identity handshake with a safety filter, link protocol with pairing and replay protection, timing statistics
- [x] `inputline-host`: USB/IP virtual wired Steam Controller, link server, pairing, `demo` mode
- [x] `inputline-sim` to test the PC side without an iPad
- [x] End-to-end CI: attach through Linux vhci-hcd, then verify over libusb (descriptors, Steam's handshake, input stream, haptics back to the client, factory reset blocked) and with SDL's Steam Controller driver (two trackpads, gyro, live input)
- [x] Pairing from the couch: the PC shows the code on its screen, the device asks for it
- [x] `inputline-host install`: background start at logon and firewall setup in one command
- [x] Report the physical controller's firmware version, so Steam doesn't offer an update the virtual controller can't take
- [x] Reconnects the way Steam expects: a controller that drops off Bluetooth is unplugged like a real one and set up by Steam again when it returns; through a Wi-Fi drop of up to 10 s it stays plugged in
- [x] Windows + usbip-win2 + iPad with a real 2026 Steam Controller: Steam shows full Steam Input, all inputs work
- [x] InputLine app for iPad and iPhone: background Bluetooth with state restoration, Keychain pairing, haptics, timing statistics, event log and shareable report
- [x] Background delivery measured: as smooth as foreground ([Checking smoothness](timing.md))
- [x] Recover single lost datagrams: each one also carries the previous report
- [x] Unsigned `InputLine-iOS.ipa` on every CI run and release, for sideloading without a Mac
- [x] Automatic discovery: `inputline-host` announces the PC over DNS-SD (`_inputline._udp`); InputLine lists it and follows a paired PC to a new address
- [x] InputLine remembers every address a PC answered on (home network, VPN) and tries them all at once
- [x] Releases from GitHub's website: Actions → Release → Run workflow
- [x] Protocol version 2: X25519 pairing, ChaCha20-Poly1305 encryption, version negotiation with frozen wire-format tests
- [x] Disconnect / Connect in the app: hand the controller back to the iPad; it goes back to the PC when switched off and on
- [x] App: while connected, the PC section shows "Connected to <PC>" and a Disconnect button
- [x] Tray icon with status, the pairing code and update notices; the service checks GitHub for new versions; the app says which side runs an older version
- [x] Protocol version 3: pairing with CPace, a password-authenticated key exchange (checked against the CFRG draft's test vectors)
- [x] Fuzzing in CI: link protocol, link server and USB/IP server
- [x] usbip-win2 installed in either order: the installer's last screen shows its status (green, amber, red) with a download link; the tray icon shows a red mark and the link until it's there
- [x] Windows installer (MSI): `inputline-host` runs as a service that starts with Windows, with the firewall rule; CI installs and uninstalls it
- [x] Linux: `sudo inputline-host install` sets it up as a systemd service (vhci-hcd, udev rule for Steam, ufw/firewalld rule), with Avahi discovery, desktop notifications for the pairing code, a tray icon (KDE Plasma and other StatusNotifierItem trays) and `inputline-host status`; CI installs it, pairs through it, checks the icon and uninstalls it

## Phases

| Phase | What | Status |
|---|---|---|
| 0 | InputLine app with background Bluetooth; timing measured on the device and at the PC | **Done** |
| 1 | PC program: automatic discovery, installer, usbip-win2 status, update check (all done); code signing | In progress |
| 2 | InputLine release: TestFlight, then a free App Store app | Later |
| 3 | More bridges: desktop (Windows, Linux, Mac, Steam Deck; USB, Puck or Bluetooth), Android and Android TV | |
| 4 | Optional: offer the host side to streaming hosts (Vibepollo, Apollo, Sunshine) as a built-in feature | |

## Next

- [ ] Find which setting makes the controller drop Bluetooth when Steam restarts or changes a config, and stop passing it on (setting 49 is already blocked)
- [ ] iPhone with a real 2026 Steam Controller: works end to end with Fedora, also in the background (reported by a tester); still to check: long sessions with the screen locked
- [ ] Apple TV through an iPhone bridge
- [ ] Settle whether Steam uses the IMU quaternion in report `0x42`. If it does, compute orientation host-side from gyro and accel instead of sending identity.
- [ ] Haptics end to end: trackpad clicks and rumble felt on the controller
- [ ] Measure input latency against Steam Link (240 fps camera, same TV)
- [ ] Battery: the app reads it over Bluetooth and passes it to Steam; confirm on real hardware which source the controller offers (Valve's report 0x43 or the standard battery level)

## Later

- [ ] 2015 Steam Controller (BLE `0x1106`, wired `28DE:1102` persona)
