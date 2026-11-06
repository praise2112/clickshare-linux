# ClickShare on Linux

Share your Linux screen to a Barco ClickShare meeting room with a **ClickShare
Button**, the USB dongle on the meeting room table. Barco makes the ClickShare app
for Windows and macOS only. This project runs the Windows app under Wine and
adds the screen capture that Wine is missing, on both Wayland and X11.

Deutsche Fassung: [README.de.md](README.de.md)

| Feature | Status |
|---|---|
| Button pairing with the room | Works |
| Share your screen | Works on Wayland (GNOME) and X11 |
| Share a single window | Works on Wayland, through GNOME's picker |
| Share a separate virtual screen that only the room display shows | Works on Wayland (GNOME) |
| Live screen thumbnail in the app | Works |
| App starts when you plug in the Button, closes when you unplug it | Works (systemd) |
| Audio in the room | Works on X11 setups; not yet tested on Wayland |

Tested setups:

| Distribution | Session | Wine | ClickShare app | Button | Base unit |
|---|---|---|---|---|---|
| Ubuntu 26.04 | GNOME 50, Wayland | 10.0 (Ubuntu package) | 4.51.1 | Gen4 `0600:00ce` | CX-30 |
| Pop!_OS 22.04 | GNOME, X11 | 11.0 (WineHQ) | 4.36.0 | `0600:00ce` | not recorded |

> **Disclaimer:** This is an unofficial community project. Barco does not
> endorse or support it. "ClickShare" and "Barco" are trademarks of Barco N.V.
> This repository contains **no Barco software**; you need your own copy of the
> ClickShare app installer.

## Contents

- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Installation](#installation)
- [Usage](#usage)
- [Settings](#settings)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Uninstall](#uninstall)
- [Development](#development)
- [Technical details](docs/TECHNICAL.md)
- [License](#license)

## Quick start

With Wine 9 or newer installed and `ClickShare_Setup.exe` in `~/Downloads`
([where to get it](#2-get-clickshare_setupexe)):

```sh
git clone https://github.com/praise2112/clickshare-linux.git
cd clickshare-linux
sudo sh scripts/install-udev-rule.sh
sh scripts/setup.sh ~/Downloads/ClickShare_Setup.exe
```

Then plug in the Button. The app opens by itself after about 15 seconds.

## How it works

```
 Wayland: GNOME screen sharing (xdg-desktop-portal + PipeWire)
          -> helper/clickshare-capture.py -> frame file in /run/user/<uid>
 X11:     GDI BitBlt inside Wine
                         |
                         v
 +--------------------------+  DuplicateOutput()   +-----------------------------+
 | csdup.dll (shim)         |<---------------------| clickshare_native.exe       |
 | DXGI Desktop Duplication |--- D3D11 texture --->| (Barco's app, under Wine)   |
 +--------------------------+                      +--------------+--------------+
                                          HID over USB (hidraw)   | encoded video
                                                                  v
                     ClickShare Button --- Wi-Fi ---> base unit ---> room display
```

1. **The app.** `clickshare_native.exe` is a native Qt program.
   `scripts/setup.sh` extracts it straight from Barco's `ClickShare_Setup.exe`,
   skipping Barco's installer, which needs .NET and hangs under Wine.
2. **The Button.** Wine talks to the Button's HID interfaces through `hidraw`.
   A udev rule gives the logged-in user access, and also starts the app when a
   Button is plugged in.
3. **Screen capture.** The app captures the screen only through *DXGI Desktop
   Duplication*, which Wine does not implement. `csdup.dll` implements it. On
   Wayland, Wine cannot see the screen at all, so a small Python helper asks
   GNOME for the screen through the screen-sharing portal (the same one
   browsers use) and passes the frames to the shim. On X11, the shim reads the
   screen with GDI. `inject.exe` starts the app and loads the shim into it.
4. **Audio.** The Button is also a USB sound card, and it forwards whatever
   plays to it while a share runs. `clickshare.sh` makes it the default output.

More detail: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Requirements

- A **ClickShare Button** that works with your meeting room's base unit.
- `ClickShare_Setup.exe`, the Windows installer of the ClickShare app
  ([step 2](#2-get-clickshare_setupexe)).
- **Wine 9 or newer.** Ubuntu 22.04's Wine 6.0 is too old: the app crashes on
  start.
- For Wayland: GNOME with `xdg-desktop-portal-gnome`, PipeWire, and the
  GStreamer PipeWire plugin with Python bindings. On Ubuntu:
  `sudo apt install gstreamer1.0-pipewire python3-gi gir1.2-gstreamer-1.0`.
  Most GNOME installs already have them.
- `python3`, `curl`, and `pactl` for audio routing (package `pulseaudio-utils`,
  which also works with PipeWire). Optional: `7z` (`p7zip-full`).
- A password for `sudo`, once, for the udev rule (and for Wine, if you need to
  install it).

## Installation

### 1. Wine

```sh
wine --version
```

If Wine is older than 9, install the current WineHQ build. On Ubuntu and
derivatives such as Pop!_OS or Linux Mint:

```sh
sudo sh scripts/install-winehq.sh
```

For other distributions, see the
[WineHQ download page](https://gitlab.winehq.org/wine/wine/-/wikis/Download).
Ubuntu 26.04's own `wine` package (10.0) works as is.

### 2. Get `ClickShare_Setup.exe`

You need the Windows installer of the ClickShare app. Either:

- Download *ClickShare App for Windows* from Barco's website (barco.com →
  ClickShare → Apps / Downloads), or
- Plug in the Button and run the launcher stored on it:
  `wine /media/$USER/ClickShare/ClickShare_for_Windows.exe`. Accept the
  license and click *Install*. The launcher downloads `ClickShare_Setup.exe` to
  `~/Downloads`. Then **cancel** the .NET installation that follows: it hangs
  under Wine, and the app does not need it.

### 3. USB permissions and autostart (udev)

```sh
sudo sh scripts/install-udev-rule.sh
```

The rule [`udev/70-clickshare.rules`](udev/70-clickshare.rules) gives the user
logged in at the computer access to Barco USB devices (vendor ID `0600`). For
ClickShare Buttons, it also asks systemd to start the app when one is plugged
in. **Unplug and re-plug the Button afterwards.**

### 4. Setup

```sh
sh scripts/setup.sh ~/Downloads/ClickShare_Setup.exe
```

The script:

1. Creates a Wine prefix (a separate Windows environment) in
   `~/.wine-clickshare`. To put it elsewhere, run the script with
   `CLICKSHARE_PREFIX=/some/path`; the path is saved in the
   [settings file](#settings).
2. Extracts the app into the prefix with
   [`scripts/extract-clickshare-app.py`](scripts/extract-clickshare-app.py).
3. Builds `build/csdup.dll` and `build/inject.exe`. If MinGW-w64 is not
   installed, it downloads a pinned Zig compiler into `.toolchain/`, without
   root.
4. Adds **ClickShare** to your app grid, with the app's own icon.
5. Installs a systemd user service, `clickshare.service`, which runs the app
   while a Button is plugged in.

The setup is tied to where you cloned the repository. If you move the folder,
run `sh scripts/setup.sh` again from its new place.

## Usage

1. **Plug in the Button.** The app starts by itself and shows *Share in the
   room* once it has paired with the room, after about 15 seconds. You can
   also start it from the app grid, or with `sh clickshare.sh`. Opening it from
   the app grid while it already runs leaves it, and any share, as it is.
2. **Click Desktop 1** (or press the Button). On Wayland, GNOME's sharing
   picker opens:
   - **Window** tab: share one window.
   - **Display** tab: share your screen, or pick **Virtual Monitor** for a
     separate screen.
3. **Stop or pause** with the buttons on the thumbnail. **Unplugging the
   Button** closes the app.

**A separate screen for the room.** Pick *Virtual Monitor*: GNOME adds an
extra screen to the right of your own, and only the room display shows it.
Drag the windows you want to present past the right edge of your screen. Your
own screen stays private.

**The screen thumbnail.** The first time the app starts, GNOME asks which
screen it may use for the *Desktop 1* thumbnail. Keep *Remember this
selection* ticked, and it won't ask again. The thumbnail updates when the app
redraws it, for example when you switch to the app.

**Audio.** While the app runs, the Button is your default sound output, and
the room hears what you play during a share. Afterwards, switch the output back
in *Settings → Sound*.

## Settings

Settings go in `~/.config/clickshare-linux/env`, one `NAME=value` per line.
The launcher, the autostart service and `clickshare.sh` all read it. A
variable set in the environment overrides the file.

| Setting | Effect |
|---|---|
| `CLICKSHARE_PREFIX=/path` | Wine prefix to use (default `~/.wine-clickshare`) |
| `CLICKSHARE_NO_AUDIO=1` | Leave the sound output alone |
| `CLICKSHARE_MAX_FPS=20` | Wayland only: hand the app at most this many frames per second. On one setup, 15-20 fps reduced the delay while scrolling; the default `0` keeps the app's own rate. |
| `WINEDLLOVERRIDES=d3d11,dxgi=n` | Use DXVK's DLLs, see below |

**Optional: DXVK.** [DXVK](https://github.com/doitsujin/dxvk) runs the app's
Direct3D through Vulkan. In our tests it lowered the app's CPU use from about
2 to 1.2 cores and smoothed scrolling slightly. It needs a working Vulkan
driver. To use it, download a DXVK release and copy its 32-bit DLLs into the
prefix (the app is 32-bit). Run this in the repository folder, so the prefix
comes from your settings:

```sh
PREFIX=$(sh -c '. scripts/env.sh; echo "${CLICKSHARE_PREFIX:-$HOME/.wine-clickshare}"')
tar xf dxvk-3.1.1.tar.gz
cp dxvk-3.1.1/x32/d3d11.dll dxvk-3.1.1/x32/dxgi.dll "$PREFIX/drive_c/windows/syswow64/"
mkdir -p ~/.config/clickshare-linux
echo 'WINEDLLOVERRIDES=d3d11,dxgi=n' >> ~/.config/clickshare-linux/env
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Nothing happens when you plug in the Button | Check `systemctl --user status clickshare`. If the service is unknown, run `sh scripts/setup.sh` again. If it never starts, re-run `sudo sh scripts/install-udev-rule.sh` and re-plug the Button. |
| App says *"plug in a ClickShare Button"* although it is plugged in | No HID access. Re-run the udev step, re-plug the Button, and restart the app. The app does not notice a Button plugged in after it started; with autostart, re-plugging restarts it. |
| GNOME's sharing picker does not appear | It can open behind other windows. Look in the Activities overview. |
| *Desktop 1* thumbnail stays black | On Wayland you cancelled GNOME's thumbnail question. Restart the app and allow it. |
| GNOME asks for the thumbnail screen again | GNOME remembers the answer per program that started the app. The launcher and autostart count as one; starting `sh clickshare.sh` from a terminal counts as another. |
| Room shows the ClickShare frame but the **picture is black** | The shim is not loaded. `$WINEPREFIX/drive_c/csdup.log` must contain `patched DuplicateOutput` and `duplication created`. Start the app only through the launcher or `clickshare.sh`. |
| **No sound** in the room | `pactl` is missing, or the default output is not the Button: `pactl get-default-sink` should name `…Barco_ClickShare…`. Audio only reaches the room while sharing. |
| App crashes on start (`RoGetActivationFactory`, `c0000005`) | Wine is too old. Install Wine 9 or newer. |
| `.NET Framework Initialization Error` dialog | Harmless. Click OK. |
| .NET installer hangs at *"netfx_Full_LDR.mzz"* | You ran `ClickShare_Setup.exe` directly. Use `scripts/setup.sh` instead, and stop Wine with `wineserver -k`. |

**Logs:**

- App: `$WINEPREFIX/drive_c/users/$USER/AppData/Local/Barco/ClickShare App/clickshare.log`
- Shim: `$WINEPREFIX/drive_c/csdup.log`
- Autostart and screen-capture helper: `journalctl --user -u clickshare`
- Wine: `WINEDEBUG=err,+hid sh clickshare.sh`

## Limitations

- **Wayland was tested on GNOME only.** Other desktops with a ScreenCast portal
  (KDE Plasma, for example) may work, but the separate virtual screen and the
  thumbnail rely on GNOME behaviour.
- **The app's own window list is hidden on Wayland.** Wine cannot see other
  programs' windows there, so pick windows in GNOME's picker instead. On X11,
  single-window sharing is untested.
- **Fast motion drops the frame rate.** Slides and documents run at about
  30 fps. While scrolling quickly, the USB link to the Button fills up under Wine,
  and the app sends 16-19 fps. Delay from laptop to room screen was about
  0.35-0.45 s in our tests; Barco publishes no figure to compare with.
- **Starting takes about 15 seconds**, mostly the app's own startup and pairing
  with the room.
- Features that need .NET (calendar and Outlook integration) and automatic
  updates are not available. Update by running `setup.sh` with a newer
  installer.
- Sharing over the network without a Button is untested.
- Multi-monitor setups are untested.

## Uninstall

Run this in the repository folder. `scripts/uninstall.sh` stops the app, then
removes the autostart service, the launcher, the icon and the settings, and
asks before deleting the Wine prefix with the ClickShare app.

```sh
sh scripts/uninstall.sh
sudo rm /etc/udev/rules.d/70-clickshare.rules && sudo udevadm control --reload
rm -rf build .toolchain                    # build output in this repository
```

## Development

```sh
sh scripts/build.sh     # builds the shim into build/
sh tests/run.sh         # runs the tests
```

Repository layout:

```
clickshare.sh                     starts the app: settings, audio, capture helper, shim
helper/clickshare-capture.py      Wayland screen capture through the ScreenCast portal
shim/csdup.c                      DXGI Desktop Duplication for Wine, thumbnail and window-list fixes
shim/inject.c                     starts a program and loads a DLL into it
shim/pacing.h                     frame pacing for CLICKSHARE_MAX_FPS
shim/dupltest.c                   checks screen capture without the app (build/dupltest.exe)
scripts/setup.sh                  one-time setup: prefix, app, build, launcher, autostart
scripts/build.sh                  builds the shim (MinGW-w64 or a downloaded Zig)
scripts/extract-clickshare-app.py unpacks the app from ClickShare_Setup.exe
scripts/extract-icon.py           saves the app's icon for the launcher
scripts/install-launcher.sh       adds ClickShare to the app grid
scripts/launch.sh                 what the launcher runs; never stops a running app
scripts/uninstall.sh              removes what setup.sh installed
scripts/install-autostart.sh      installs clickshare.service
scripts/install-udev-rule.sh      installs udev/70-clickshare.rules
scripts/install-winehq.sh         installs current Wine from WineHQ (Ubuntu family)
scripts/env.sh                    reads ~/.config/clickshare-linux/env, finds wineserver
udev/70-clickshare.rules          device access and autostart
tests/                            tests, run with tests/run.sh
docs/TECHNICAL.md                 analysis and design notes
```

Reports from other distributions, desktops, Button models
(`lsusb | grep 0600`) and app versions are welcome. Please include your Wine
version, the app log and `csdup.log`.

## Credits

Based on [ProjektW2/clickshare-linux](https://github.com/ProjektW2/clickshare-linux)
by Christoph Bellmann, which got the app running under Wine on X11 and wrote
the DXGI shim. This fork adds Wayland capture, the thumbnail and window-list
fixes, the virtual screen, frame pacing, autostart and the launcher.

## License

The code in this repository is released under the [MIT License](LICENSE).
The ClickShare software is proprietary software by Barco N.V. and is **not**
covered by this license. Using it is subject to Barco's license terms.
