# Technical details

This document records how the ClickShare Button was made to work on Linux:
what the hardware exposes, which approaches failed and why, how the
DXGI Desktop Duplication shim works, and how Wayland capture, thumbnails and
autostart were added. It is meant for people who want to debug, extend or port
this to other setups.

- [1. Hardware: what the Button exposes](#1-hardware-what-the-button-exposes)
- [2. Approaches that did not work](#2-approaches-that-did-not-work)
- [3. Getting the app to run under Wine](#3-getting-the-app-to-run-under-wine)
- [4. Device access: HID and USB in Wine](#4-device-access-hid-and-usb-in-wine)
- [5. The black screen: DXGI Desktop Duplication](#5-the-black-screen-dxgi-desktop-duplication)
- [6. The shim (`csdup.dll`)](#6-the-shim-csdupdll)
- [7. Injection (`inject.exe`)](#7-injection-injectexe)
- [8. Audio](#8-audio)
- [9. Debugging reference](#9-debugging-reference)
- [10. Ideas for future work](#10-ideas-for-future-work)
- [11. Wayland](#11-wayland)
- [12. Thumbnails and the window list](#12-thumbnails-and-the-window-list)
- [13. Autostart](#13-autostart)

---

## 1. Hardware: what the Button exposes

`lsusb` with the Button plugged in:

```
ID 0600:00ce Barco Display Systems ClickShare
ID 0600:0114 Barco Display Systems Room Camera
```

`0600:0114` appears in addition when the room supports *bring your own meeting*
(conference mode). It is a standard UVC camera (`/dev/video*`) and works
natively on Linux.

Interfaces of `0600:00ce` (`lsusb -v -d 0600:00ce`):

| Interface | Class | Purpose (observed) |
|---|---|---|
| 0 | 255/255 vendor-specific | bulk interface; purpose unknown (USB traffic was not traced) |
| 1 | HID | vendor usage page, 1025-byte reports; control and crypto channel (certificate exchange observed) |
| 2 | HID | vendor usage page, 1025-byte reports; data channel |
| 3 | Mass storage (SCSI) | read-only CD-ROM image, label `ClickShare`: Windows/macOS launcher, EULA links |
| 4 | HID | vendor usage page, 1025-byte reports |
| 5–7 | USB Audio Class 1 | stereo playback device ("ClickShare"), appears as a PipeWire/ALSA sink |

On Linux the kernel binds `usbhid` (→ `/dev/hidrawN`, root-only by default),
`usb-storage` (→ `/dev/sr0`, automounted) and `snd-usb-audio` (→ sink
`alsa_output.usb-Barco_ClickShare_<serial>-<port>.analog-stereo`).

The CD-ROM only contains `ClickShare_for_Windows.exe` (a UPX-packed launcher)
and `ClickShare_for_MacOSX.app`. There is no Linux client.

## 2. Approaches that did not work

| Approach | Result |
|---|---|
| Audio only (set the Button as default sink) | The sink accepts audio, but nothing is heard in the room. The Button forwards audio **only while an app share session is active**. |
| Miracast via GNOME Network Displays | The Wi-Fi chip in the test laptop (Realtek 8822CE, `rtw88`) supports no P2P/Wi-Fi Direct modes (`iw list`: no `P2P-client`/`P2P-GO`). Miracast-over-Infrastructure and Google Cast found nothing: `avahi-browse -a` showed no services on the (client-isolated) eduroam network. |
| Launcher from the Button under Wine 6.0 | Runs and shows its EULA, then insists on installing the full app (`ClickShare_Setup.exe`). |
| `ClickShare_Setup.exe` under Wine | Squirrel installer → `Update.exe` needs .NET Framework 4.x → Microsoft's .NET web installer (BITS download) hangs at `netfx_Full_LDR.mzz`. It cannot be cancelled from the UI and must be killed with `wineserver -k`. |
| `clickshare_native.exe` under Wine 6.0 | Crashes immediately: `RoGetActivationFactory Failed to find library` → access violation `c0000005`. Wine 6.0 lacks the needed WinRT pieces. |

## 3. Getting the app to run under Wine

### Extracting the app from the Squirrel installer

`ClickShare_Setup.exe` (≈ 92 MB, PE32) carries a ZIP archive in a PE resource:

```
ClickShare-4.36.0-b11-full.nupkg   88 MB   (NuGet package = ZIP)
RELEASES
setupIcon.ico
Update.exe                          Squirrel updater (.NET)
```

Neither `7z` nor Python's `zipfile` open the EXE directly, because the
Authenticode signature follows the archive. `extract-clickshare-app.py`
therefore searches for the first local file header (`PK\x03\x04`) whose name
is the `.nupkg` or `RELEASES`, then for the following end-of-central-directory
record, and opens exactly that slice. The app lives in the `.nupkg` under
`lib/net45/`:

| File | Kind | Notes |
|---|---|---|
| `clickshare_native.exe` | PE32, native (Qt 5/QML, C++) | **the actual app**, needs no .NET |
| `ClickShare.exe` | PE32, .NET | thin wrapper/launcher; not needed |
| `PresentSense.exe`, `calendarreader*.exe` | native | optional features |
| `WebComponent.7z` | Electron bundle | embedded web UI parts; unpacked when `7z` is available |
| `*_ExecutionStub.exe` | Squirrel stubs | unused |

Squirrel would install to `%LOCALAPPDATA%\ClickShare\app-<version>\`. The
script uses the same layout.

### Wine version and prefix

- **Wine 11.0** (WineHQ stable) runs `clickshare_native.exe`. The Qt Quick UI
  renders through D3D11 (wined3d). The `libEGL.so.1` missing message is
  harmless.
- Use a **fresh prefix** created by the new Wine. A prefix that went through the
  aborted .NET installation under Wine 6 lost the `wineusb` service registration
  (see [§4](#4-device-access-hid-and-usb-in-wine)), and `wineboot -u` then
  blocked on a modal `.NET Framework Initialization Error` dialog.
- `WINEDLLOVERRIDES="mscoree,mshtml="` suppresses the Mono/Gecko install
  prompts. The app needs neither.

## 4. Device access: HID and USB in Wine

### Permissions

By default `/dev/hidraw*` is `crw------- root root` and the USB device nodes
are `crw-rw-r-- root root`. The udev rule tags the Barco devices with `uaccess`,
which makes systemd-logind add an ACL for the active seat user:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="0600", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="0600", TAG+="uaccess"
```

A third line starts the app on plug-in, see [§13](#13-autostart).

### How the app finds the Button

Import and string analysis of `clickshare_native.exe` show:

- `SetupDiGetClassDevs` / `SetupDiEnumDeviceInterfaces` on the HID class GUID
  `{4d1e55b2-f16f-11cf-88cb-001111000030}`, then `HidD_GetAttributes`
  (VID `0x0600`), `HidD_GetPreparsedData`/`HidP_GetCaps`,
  `HidD_Get/SetFeature` and plain `ReadFile`/`WriteFile` on 1024-byte reports.
- `GetLogicalDrives` + `GetVolumeInformationW` (the `ClickShare` CD-ROM volume;
  Wine maps it to `D:` automatically via udisks).

Wine's `winebus.sys` (udev backend) exposes the hidraw nodes as
`HID\VID_0600&PID_00CE&MI_01|02|04`. With access granted, `+hid` traces show the
app writing 1024-byte output reports and receiving input reports that contain
an X.509 chain (`Barco … Root Certificate`, `Kortrijk`). This is the Button
pairing/authentication, and it works unmodified under Wine.

`wineusb.sys` (libusb backend) additionally exposes all interfaces including
the vendor bulk interface 0 (`USB\VID_0600&PID_00CE&MI_00`). In the broken
prefix the `wineusb` service was missing. In a fresh Wine 11 prefix it is
present. Whether the app uses interface 0 was not traced; HID access alone
was enough for pairing, and video reached the room in the working setup.

## 5. The black screen: DXGI Desktop Duplication

Once paired, the room display showed the ClickShare frame with a black picture,
while the app's own thumbnail of *Desktop 1* was correct. The app log
(`AppData/Local/Barco/ClickShare App/clickshare.log`) gives the reason:

```
[DXGIDesktopCapture::initDXGI] Duplicate output error: -2147467263   (0x80004001 E_NOTIMPL)
[upserver::VideoSourceNode::setup] open failed
[upserver::VideoMixedStream::startPipeline] source start failed
```

Wine's `dxgi.dll` implements `IDXGIOutput1::DuplicateOutput` and
`IDXGIOutput5::DuplicateOutput1` as stubs only (`… stub!`, `E_NOTIMPL`),
including Wine 11.0. The app has no alternative desktop capture backend. Its
only desktop grabber is `DXGIDesktopCapture` (a `PrintWindowCapture` class also
exists, presumably for single windows). The thumbnail is produced via GDI, which is why it looked
fine.

## 6. The shim (`csdup.dll`)

Source: [`shim/csdup.c`](../shim/csdup.c). It is about 770 lines of C, built
as a 32-bit DLL because the app is 32-bit. This section describes the X11
path; [§11](#11-wayland) and [§12](#12-thumbnails-and-the-window-list) cover
what changes under Wayland.

### Hooking

All `IDXGIOutput` objects created by Wine's `dxgi.dll` share one static vtable
(`dxgi_output_vtbl`, implementing up to `IDXGIOutput6`). When the DLL loads, a
worker thread (to stay out of the loader lock) does the following:

1. `CreateDXGIFactory1` → `EnumAdapters1(0)` → `EnumOutputs(0)`.
2. `QueryInterface(IDXGIOutput1)`: remember `GetDesc`, then overwrite the vtable
   slot `DuplicateOutput` (index 22) after a `VirtualProtect`.
3. `QueryInterface(IDXGIOutput5)`: overwrite `DuplicateOutput1` (index 26).

Because the vtable is shared, this patches every output, including ones the app
already created. The ClickShare app calls `DuplicateOutput1` and re-creates the
duplication each time a share starts, so late injection is fine.
The module pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) because the patched
vtable points into it.

### `IDXGIOutputDuplication` implementation

| Method | Behaviour |
|---|---|
| `GetDesc` | output size from `IDXGIOutput::GetDesc().DesktopCoordinates`, `DXGI_FORMAT_B8G8R8A8_UNORM`, 60 Hz, `DesktopImageInSystemMemory = FALSE` |
| `AcquireNextFrame` | X11: throttles to ~30 fps (returns `DXGI_ERROR_WAIT_TIMEOUT` if the caller's timeout is shorter than the remaining interval), grabs a frame, returns the texture as `IDXGIResource`. Wayland: returns as soon as the helper has a new frame, and times out while the screen is still, like real duplication |
| `GetFrameDirtyRects` | one rectangle covering the whole output (`DXGI_ERROR_MORE_DATA` if the buffer is too small) |
| `GetFrameMoveRects` | none |
| `GetFramePointerShape` | none: the cursor is drawn into the image instead, and `PointerPosition.Visible = FALSE` |
| `MapDesktopSurface` | `DXGI_ERROR_UNSUPPORTED` (the image is in video memory) |
| `ReleaseFrame` | no-op |
| `GetParent` | forwards to the output |

Frame grab:

1. `BitBlt(SRCCOPY | CAPTUREBLT)` from `GetDC(NULL)` into a top-down 32-bpp DIB
   section. In Wine/X11 the screen DC is the real X root window, so this
   captures the Linux desktop including non-Wine windows.
2. Draw the current cursor with `GetCursorInfo` + `DrawIconEx`.
3. Set alpha to `0xFF`, because GDI leaves it at 0.
4. `ID3D11DeviceContext::UpdateSubresource` into a `D3D11_USAGE_DEFAULT`
   `B8G8R8A8` texture created on the **caller's** D3D11 device (the `IUnknown`
   passed to `DuplicateOutput*`). The app then copies it into its encoder
   pipeline as it would on Windows.

Log output goes to `OutputDebugString` and `C:\csdup.log`.

## 7. Injection (`inject.exe`)

Wine does not implement `AppInit_DLLs`, and proxying one of the app's imported
DLLs would require forwarding every export. `inject.exe` uses the classic
method instead:

1. `CreateProcessW(app)` with the app's directory as the working directory. The
   app writes `DLCap.log`/`DXCap.log` into the current directory.
2. `WaitForInputIdle` + a delay (default 3000 ms).
3. `VirtualAllocEx` + `WriteProcessMemory` (DLL path) +
   `CreateRemoteThread(LoadLibraryW)`.

This works in Wine without changes.

## 8. Audio

The Button's audio function is a plain UAC1 stereo playback device. Observed
behaviour:

- Playing audio to it without an active share produces **no sound** in the
  room.
- With a share running (started by the app under Wine), audio played to the
  sink is heard in the room. Whether the app additionally captures audio
  itself (WASAPI loopback) was not investigated; routing to the sink was enough.
- The PipeWire sink name contains a port suffix that changes on re-plug, for
  example `…-05.analog-stereo` → `…-05.3.analog-stereo`. Default-sink selection
  may fall back to internal speakers. The launcher therefore looks the sink up
  by the substring `Barco_ClickShare` every time.

## 9. Debugging reference

```sh
export WINEPREFIX=~/.wine-clickshare

# HID enumeration and traffic (very verbose)
WINEDEBUG=+hid sh clickshare.sh 2>hid.log

# raw USB enumeration (wineusb / libusb)
WINEDEBUG=+wineusb wine cmd /c exit 2>&1 | grep 0600

# which DLLs the app loads
WINEDEBUG=+loaddll sh clickshare.sh 2>&1 | grep Loaded

# device tree as Wine sees it
wine reg query 'HKLM\System\CurrentControlSet\Enum\USB' | grep 0600
wine reg query 'HKLM\System\CurrentControlSet\Enum\HID' | grep 0600

# app log (most useful)
tail -f "$WINEPREFIX/drive_c/users/$USER/AppData/Local/Barco/ClickShare App/clickshare.log"
# shim log
cat "$WINEPREFIX/drive_c/csdup.log"
```

Typical healthy shim log:

```
csdup: patched DuplicateOutput
csdup: patched DuplicateOutput1
csdup: DuplicateOutput1 called
csdup: duplication created 1920x1080 at 0,0
```

## 10. Ideas for future work

- **Upstream:** implement `IDXGIOutput1::DuplicateOutput` in Wine itself
  (wined3d could read back the X11 root window, or use XDamage for dirty rects).
  That would make the shim unnecessary for this and other screen-sharing apps.
- **Performance:** dirty rectangles via XDamage, MIT-SHM capture, or GPU upload
  without the intermediate DIB. Under fast motion the limit is elsewhere: the
  app writes 1025-byte HID reports one at a time, about 0.5 ms each under Wine,
  which caps a busy screen at 16-19 fps. Most of each write is the kernel
  waiting for the USB transfer, plus Wine's round trips through wineserver.
- **Window sharing on X11:** hook the `PrintWindow`-based path.
- **Packaging:** a Lutris/Bottles installer script or a Flatpak.

## 11. Wayland

Under Wayland, Wine runs on XWayland and GDI sees only Wine's own windows: a
`BitBlt` of the screen is black. Screen content is only available through the
`org.freedesktop.portal.ScreenCast` portal, which needs the user's consent.

**The helper.** `clickshare.sh` starts
[`helper/clickshare-capture.py`](../helper/clickshare-capture.py) when
`XDG_SESSION_TYPE=wayland`, and passes the frame file's path to the shim in
`CLICKSHARE_FRAMES`. The handshake goes through files in `/run/user/<uid>`:

1. The app calls `DuplicateOutput1` when a share starts. The shim creates
   `clickshare-frames.request`, containing the output size (`2560x1600`).
2. The helper opens a portal session with source types monitor, window and
   virtual (1 | 2 | 4). GNOME shows one picker with a *Window* and a *Display*
   tab; *Display* lists the real screens and *Virtual Monitor*.
3. GStreamer (`pipewiresrc ! videoconvert ! appsink`) delivers BGRA frames,
   which the helper writes into `clickshare-frames`: a 64-byte header (magic
   `CSFR`, width, height, stride, sequence) and the pixels. The sequence is odd
   while a frame is being written, so the shim retries torn reads.
4. When the share ends, the shim deletes the request file. The helper waits
   0.7 s before closing the portal session, because the app releases and
   re-creates its duplication when a share starts.

The shim copies each new frame into its own memory before uploading it to the
D3D11 texture, and centres smaller frames (a window) on black. Frames larger
than the output (a bigger monitor, or a HiDPI screen whose XWayland output is
scaled down) are shrunk to fit, keeping the aspect ratio. Reading straight from
the shared file during the upload tore almost every frame.

**The virtual screen.** When the user picks *Virtual Monitor*, the portal
reports `source_type` 4 for the stream. GNOME sizes a virtual monitor from the
video format the consumer negotiates, and makes it 1x1 if nothing is asked
for. The helper therefore puts caps with the size from the request file after
`pipewiresrc`. The app encodes at the room display's resolution anyway, but
matching the DXGI output size avoids black bars.

**Frame pacing.** `CLICKSHARE_MAX_FPS` limits how often `AcquireNextFrame`
hands over a frame. The next due time advances by one interval from the last
due time ([`shim/pacing.h`](../shim/pacing.h)), so late handovers do not lower
the average rate, and after a still screen the schedule restarts instead of
releasing a burst.

**XWayland quirks.** The app creates a fully transparent layered popup ("Room
Display - <room>"). Windows ignores such windows, but under XWayland it covered
other windows and took their clicks. The shim hides it every 500 ms while it
is fully transparent.

**Touch.** When the room's display supports touch-back, the app delay-loads
`InitializeTouchInjection` and `InjectTouchInput`, which Wine 10 lacks; the
missing import crashed the app (`0xc06d007f`). The shim hooks the app's
`GetProcAddress` import and returns no-op versions, so room touch input is
ignored.

## 12. Thumbnails and the window list

Found with Wine's relay trace (set `HKCU\Software\Wine\Debug\RelayInclude` to
the functions of interest and run with `WINEDEBUG=+relay`):

- The *Desktop 1* thumbnail is one `BitBlt` from `GetDC(NULL)` with
  `SRCCOPY | CAPTUREBLT`, made whenever the app redraws its source list.
- The window list comes from `EnumChildWindows(GetDesktopWindow(), …)`, and
  window thumbnails from `PrintWindow`.

Both are delay imports, which the shim patches in the app's delay-import
address table, whether or not they were already bound.

**Thumbnail.** For a `BitBlt` whose source is the screen, the shim draws the
latest monitor snapshot from `clickshare-frames.thumbnail` with
`StretchDIBits`, then creates `clickshare-frames.thumbnail.request`, so the
next redraw shows a fresh picture. Only when there is no snapshot yet does it
wait, up to 1.5 s, and it stops as soon as the helper deletes the request. The
helper takes each snapshot as a one-frame screencast of a monitor with
`persist_mode` 2, and stores GNOME's restore token in
`~/.config/clickshare-linux/thumbnail_token`. When a token exists, it also
takes one snapshot at startup, so the app's first thumbnail is ready. GNOME asks
once; later snapshots need no interaction. Restore tokens are single-use, so the
helper saves the new one after every snapshot. If the user cancels, the helper
stops asking until it restarts.

GNOME stores the permission per app, and for programs outside Flatpak it
derives the app from the systemd unit that started the process: the autostart
service gives an empty app id, a desktop launcher gives `clickshare-linux`, a
terminal gives the terminal's id. The launcher therefore runs
[`scripts/launch.sh`](../scripts/launch.sh): with a Button plugged in it starts
`clickshare.service` (a no-op if it runs), otherwise it starts the app with
`systemd-run --user`, so the launcher and autostart share one permission. It
never restarts an app that is already running, which would end a share.

GNOME's screenshot portal was rejected for this: it works without a prompt for
unsandboxed apps, but each call flashes the screen and saves a file in
`~/Pictures`.

**Window list.** Under Wine, the only top-level windows from other processes
are Wine's own (explorer), so the list showed one useless "Untitled" entry. The
shim filters other processes' windows out of the desktop's
`EnumChildWindows`, and the app then hides its *Share a window* section.
GNOME gives ordinary apps no list of other apps' windows; the portal picker's
*Window* tab offers them instead.

## 13. Autostart

The udev rule tags ClickShare Buttons (product IDs `0070`, `009f`, `00ce`,
`0185`, the IDs listed in `clickshare_native.exe`; the room camera `0114` is
excluded):

```
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0600", ATTR{idProduct}=="0070|009f|00ce|0185", TAG+="systemd", ENV{SYSTEMD_ALIAS}+="/dev/clickshare-button", ENV{SYSTEMD_USER_WANTS}+="clickshare.service"
```

`scripts/install-autostart.sh` writes `~/.config/systemd/user/clickshare.service`
with `BindsTo=` and `After=` on `dev-clickshare\x2dbutton.device`. Plugging in
a Button starts the app; unplugging stops it, which also covers the app not
noticing a re-plugged Button. Once the Button is gone, `winedevice.exe`
ignores `SIGTERM`. With systemd's default 90 s stop timeout, a re-plug during
that time was dropped, so the unit sets `TimeoutStopSec=3`.

With the Button plugged in at login, systemd can start the service before GNOME
has exported `WAYLAND_DISPLAY` and `DISPLAY` to the user manager. `clickshare.sh`
then exits with status 75, and `RestartForceExitStatus=75` with `RestartSec=3`
retries until the desktop is ready.

From plug-in to *ready to share* takes about 14 s: Wine starts in about 3.5 s,
the app initialises for about 7 s, and detecting and pairing the Button takes
about 3.5 s. A manual start takes as long.
