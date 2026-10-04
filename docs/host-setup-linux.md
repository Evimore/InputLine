# Linux setup

InputLine runs on Linux as a background service, like on Windows. These steps are for CachyOS and other Arch-based distributions; other distributions are below.

You need Steam, and a kernel with the `vhci-hcd` module (the USB/IP virtual host controller). Arch, Ubuntu, Debian and Fedora kernels have it, and CachyOS's kernels follow Arch's; `install` tells you if yours doesn't.

## 1. Install what InputLine uses

```sh
sudo pacman -S --needed usbip avahi libnotify
sudo systemctl enable --now avahi-daemon
```

- **usbip** plugs the virtual Steam Controller into the system.
- **Avahi** lets the InputLine app find this PC on your network. Without it, type the PC's address in the app instead.
- **libnotify** shows the pairing code as a desktop notification.

On other distributions:

| Distribution | Command |
|---|---|
| Ubuntu | `sudo apt install linux-tools-generic avahi-daemon avahi-utils libnotify-bin` |
| Debian | `sudo apt install usbip avahi-daemon avahi-utils libnotify-bin` |
| Fedora | `sudo dnf install usbip avahi avahi-tools libnotify && sudo systemctl enable --now avahi-daemon` |

## 2. Install InputLine

Download **`inputline-vX.Y.Z-linux-x86_64.tar.gz`** from [Releases](../../../releases), then in a terminal:

```sh
tar xf inputline-*-linux-x86_64.tar.gz
cd inputline-*-linux-x86_64
sudo ./inputline-host install
```

`install`:

- copies `inputline-host` to `/usr/local/bin`,
- sets it up as the **inputline** systemd service: it starts with the system, runs in the background, and restarts by itself if something goes wrong,
- loads `vhci-hcd` now and at every boot,
- adds a udev rule so Steam, running as you, can open the virtual controller,
- allows UDP port 48150 in **ufw** or **firewalld**, from your local network and Tailscale only (CachyOS uses ufw),
- shows the **InputLine icon** in your desktop's tray, now and at every sign-in (see below),
- ends with a short checklist of anything still missing.

You can delete the downloaded folder afterwards.

## 3. Pair your iPad or iPhone

Open InputLine on the iPad or iPhone and tap your PC under **Found on this network** (or enter its address and tap **Connect**). The first time, a notification on the PC shows a 6-digit code; type it into InputLine. From then on, the device connects by itself.

Missed the notification, or playing in Steam's Gaming Mode, where desktop notifications don't show? The code is also in the log:

```sh
journalctl -u inputline | grep "pairing code"
```

## The tray icon

On KDE Plasma (and other desktops that show tray icons, such as GNOME with the AppIndicator extension), the InputLine icon sits in the system tray:

| Badge | Meaning |
|---|---|
| None | The service runs, and no InputLine app is connected |
| White, with a green ▶ | An InputLine app is connected |
| Green, with a white ▶ | A controller is plugged in |
| Red ✕ | Something needs you (usbip or vhci-hcd missing); the menu says how to fix it |

Click it for the menu: the status, an update when one is out, **Show the log**, the setup guide, and **Hide this icon**. To show a hidden icon again, open **InputLine** from the application menu.

## Checking that it works

```sh
inputline-host status          # what the service is doing
journalctl -u inputline -f     # its log, live
```

To check the PC side on its own, without the iPad:

```sh
sudo inputline-host demo 30
```

A virtual controller is plugged in for 30 seconds, driven by a test pattern. Open *Steam → Settings → Controller*: you should see a **Steam Controller**, and its test screen shows the left stick circling and A pulsing.

## Settings

Put extra options in `/etc/inputline/options.txt`, then run `sudo systemctl restart inputline`. For example:

```
# Log report timing every 10 s (see docs/timing.md)
--stats
```

The options are the same as on Windows: see [Windows setup → Settings](host-setup-windows.md#settings), or `inputline-host --help`.

Paired devices:

```sh
sudo inputline-host clients              # list them
sudo inputline-host forget 1a2b3c4d      # remove one
sudo systemctl restart inputline         # so the service notices
```

They're kept in `/var/lib/inputline/pairing`, which only root can read.

**Updating:** download the new release and run `sudo ./inputline-host install` from it again. Paired devices and options are kept.
**Uninstalling:** `sudo inputline-host uninstall`. Paired devices and options stay in `/var/lib/inputline` and `/etc/inputline` in case you install again; delete those folders to remove them too.

## Troubleshooting

| Symptom | Try |
|---|---|
| `inputline-host status` says usbip isn't installed | Install it (step 1), then wait a moment: InputLine checks again every 30 seconds. |
| It says the vhci-hcd module isn't loaded | `sudo modprobe vhci-hcd`. If that fails, your kernel doesn't include it; try your distribution's standard kernel. |
| InputLine doesn't list the PC | Check that Avahi runs (`systemctl status avahi-daemon`). You can always enter the PC's address instead (`ip -br addr` shows it). |
| InputLine says the PC does not answer | Check `inputline-host status`. With a firewall other than ufw or firewalld, allow UDP 48150 from your local network yourself. |
| The controller appears but Steam ignores it | Add `--verbose` to `options.txt`, restart the service, and open an issue with `journalctl -u inputline`. |
