# Install InputLine on iPad or iPhone

InputLine isn't on the App Store yet. Until it is, you install it yourself with a free Apple ID. You don't need a Mac: each release includes an unsigned `.ipa` of the app, and [Sideloadly](https://sideloadly.io) on Windows signs it with your Apple ID and installs it.

The same app runs on iPad and iPhone (iOS / iPadOS 15 or later).

## What you need

- A Windows PC, usually the one you stream from. (Sideloadly also runs on macOS. From Linux, use a Linux sideloading tool, or sideload from any Windows or Mac.)
- An Apple ID. A free one works, with limits (see [Free Apple ID limits](#free-apple-id-limits)).
- A USB cable, for the first install.
- The PC side set up first: [Windows host setup](host-setup-windows.md).

## 1. Download the app

Open the [Releases](../../../releases) page and download `InputLine-vX.Y.Z-iOS.ipa` from the latest release.

> Want the very latest development build instead? Open **Actions**, choose the latest green **CI** run, and download the **`InputLine-iOS.ipa`** artifact (you must be signed in to GitHub). It arrives as a `.zip`; extract it to get the `.ipa`.

## 2. Install Sideloadly and Apple's drivers

1. Install iTunes and iCloud for Windows. Sideloadly needs the versions from Apple's website, not the Microsoft Store ones; its [download page](https://sideloadly.io) links them.
2. Install [Sideloadly](https://sideloadly.io).
3. Connect the iPad or iPhone by USB, unlock it, and tap **Trust** when it asks about this computer.

## 3. Sideload

1. Open Sideloadly and drag the `.ipa` onto it. Your device should appear in the device list.
2. Enter your Apple ID.
3. Leave the Bundle ID as it is (`com.evimore.InputLine`). If Sideloadly says it isn't available for your Apple ID, open **Advanced options** and set your own, for example `com.yourname.inputline`. Either way, **use the same Bundle ID every time**: see below.
4. Click **Start** and wait for it to finish.
5. On the device (iOS 16 or later), turn on **Settings → Privacy & Security → Developer Mode**. It restarts; confirm when asked.
6. Trust your Apple ID: **Settings → General → VPN & Device Management**, tap your Apple ID, then **Trust**.

## 4. First run

1. Open InputLine and allow **Local Network** access when asked. Your PC shows up under **Found on this network**; tap it. (If it doesn't, enter the PC's address and tap **Connect**; see below.)
2. A 6-digit code pops up on the PC's screen. Type it into InputLine.
3. Put the Steam Controller in Bluetooth pairing mode, tap **Pair a new controller**, and accept the pairing request. Allow **Bluetooth** when asked.
4. InputLine shows the controller as **Connected to *your PC***, and Steam lists a wired Steam Controller.
5. Switch to your streaming app (Moonlight, for example) and play. InputLine keeps working in the background.

From then on, just switch the controller on: it reconnects by itself, even while another app is in front.

### Which address?

| Where you are | Address |
|---|---|
| At home, same network as the PC | Nothing to type: InputLine finds the PC. Otherwise, its local address, for example `192.168.1.20` (`ipconfig` on the PC shows it) |
| Away from home, through a VPN such as [Tailscale](https://tailscale.com) | The PC's address on the VPN. With Tailscale, its MagicDNS name (for example `my-pc`) stays the same; the InputLine installer allows Tailscale through the firewall. |

Use the same address your streaming app uses. If the PC's local address changes later, InputLine finds it again by itself.

InputLine remembers every address where it has reached your PC. When the current one doesn't answer, it tries the others in turn, so once both your home address and your VPN address have worked, switching between home and away needs no typing.

If InputLine doesn't find the PC at home:

- Check **Settings → Privacy & Security → Local Network → InputLine** on the device.
- On the PC, the network must be **Private** in Windows (**Settings → Network & internet → *your network* → Network profile type**). Windows doesn't answer network discovery on Public networks.
- Discovery needs Windows 10 version 1809 or later.

## Good to know

- **Don't swipe InputLine away** in the app switcher. iOS then disconnects the controller (which switches itself off) and doesn't start InputLine again for it until you open InputLine yourself. Left in the background, InputLine reconnects the controller whenever you switch it on, even while another app is in front.
- **Using the controller with the iPad itself.** While the controller goes to the PC, InputLine turns off its built-in mouse mode, so the trackpads don't also move the iPad's pointer. Tap **Disconnect from PC** (shown while a controller is connected) to use it with the iPad (the pointer works again); tap **Connect to PC**, or switch the controller off and on, to send it back. If the PC can't be reached for 10 seconds, the controller works as the iPad's mouse again by itself. To stop using the PC altogether, tap **Disconnect** under **PC**: InputLine stays disconnected, even after a restart, until you tap **Connect**.
- **After updating from a version before 0.2**, pair once more: pairing now uses a safer key exchange, and the connection is encrypted.
- **"Update InputLine on the PC" / "Update the app":** the app and the PC tell each other their versions; if they can't talk, the app says which one to update.

## Updating

Download the new `.ipa` and sideload it the same way, **with the same Bundle ID**. It replaces the app in place and keeps its pairing with the PC and its controllers.

First close InputLine on the device (swipe it away in the app switcher). It keeps running in the background, and while it does, the install can stop partway (Sideloadly stuck at around 80%).

## Free Apple ID limits

| Limit | What it means |
|---|---|
| Apps expire after **7 days** | Re-sign before then. Sideloadly can do it automatically while the device is on the same Wi-Fi as the PC (turn on its automatic refresh). Re-signing keeps the app's data. |
| **3** sideloaded apps at once | InputLine counts as one. |
| **10 new App IDs** per 7 days | Each *new* Bundle ID uses one. Re-signing or updating with the same Bundle ID doesn't. Changing the Bundle ID for every install runs this out quickly. |

A paid Apple Developer account removes these limits (apps last a year). Once InputLine is on the App Store, none of this is needed.

## Apple TV

InputLine can't run on Apple TV: tvOS doesn't let apps use Bluetooth in the background. Use an iPhone or iPad as the bridge instead:

1. Install InputLine on an iPhone (or iPad) and pair the controller with **it**, not with the Apple TV. If the controller is paired with the Apple TV, remove it there first (**Settings → Remotes and Devices → Bluetooth**).
2. Keep the iPhone near you, within Bluetooth range of the controller. It can stay locked in your pocket or on the sofa: InputLine works in the background.
3. Stream on the Apple TV with any app, as usual. The controller reaches the PC through the iPhone.

The Apple TV doesn't see the controller, so the streaming app's controller shortcuts (such as the quit combo) don't work there. Use the Siri Remote to leave the stream. The same is true on iPad and iPhone, where you can use the app switcher or a swipe instead.

## If something goes wrong

1. In InputLine, tap **Share report**.
2. On the PC, grab `C:\ProgramData\InputLine\inputline-host.log`.
3. Open an issue with both (the **Test report** form), and describe what you did.

Both include your PC's name, IP addresses and the controller's serial number. Issues are public, so look them over first and replace anything you'd rather not share.
