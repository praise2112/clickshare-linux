#!/usr/bin/python3
"""Captures a screen or window through the xdg-desktop-portal ScreenCast API and keeps the
latest frame in a file that csdup.dll reads.

Usage: clickshare-capture.py <frame-file>

csdup.dll creates <frame-file>.request when the app starts sharing and deletes it when
sharing stops. Each share shows GNOME's picker: a window, a screen, or a new virtual screen
that only the room display shows, sized as the request file says ("2560x1600").

For the app's screen thumbnails, csdup.dll creates <frame-file>.thumbnail.request; the helper
then writes one frame of the monitor to <frame-file>.thumbnail. GNOME asks once which
monitor to use and remembers the answer.

Frame file layout, little-endian: u32 magic "CSFR", u32 width, u32 height,
u32 stride, u32 sequence, padding to 64 bytes, then height * stride bytes of opaque BGRA.
The sequence is odd while a frame is being written.
"""
import mmap
import os
import secrets
import signal
import struct
import sys
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gio, GLib, Gst  # noqa: E402

try:
    gi.require_version("GLibUnix", "2.0")
    from gi.repository import GLibUnix  # noqa: E402

    unix_signal_add = GLibUnix.signal_add
except ValueError:  # GLib older than 2.80
    unix_signal_add = GLib.unix_signal_add

PORTAL = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST = "org.freedesktop.portal.ScreenCast"
HEADER_SIZE = 64
MAGIC = 0x52465343
SOURCE_MONITOR = 1
SOURCE_WINDOW = 2
SOURCE_VIRTUAL = 4
SOURCE_ANY = SOURCE_MONITOR | SOURCE_WINDOW | SOURCE_VIRTUAL
PERSIST_UNTIL_REVOKED = 2
CURSOR_EMBEDDED = 2
# The app releases and recreates its duplication when a share starts; ride over that gap.
STOP_DELAY_MS = 700


class FrameFile:
    def __init__(self, path):
        self.path = path
        self.size = (0, 0)
        self.map = None
        self.sequence = 0

    def write(self, width, height, stride, pixels):
        self.sequence += 1
        if self.map:
            struct.pack_into("<I", self.map, 16, self.sequence)
        if (width, height) != self.size:
            self._resize(width, height)
        struct.pack_into("<I", self.map, 16, self.sequence)
        row_bytes = width * 4
        if stride == row_bytes:
            self.map[HEADER_SIZE:HEADER_SIZE + row_bytes * height] = pixels[:row_bytes * height]
        else:
            for y in range(height):
                start = HEADER_SIZE + y * row_bytes
                self.map[start:start + row_bytes] = pixels[y * stride:y * stride + row_bytes]
        self.sequence += 1
        struct.pack_into("<I", self.map, 16, self.sequence)

    def _resize(self, width, height):
        if self.map:
            self.map.close()
        with open(self.path, "a+b") as file:
            # Never shrink: the reader may still map the old size.
            file.truncate(max(os.fstat(file.fileno()).st_size, HEADER_SIZE + width * height * 4))
            self.map = mmap.mmap(file.fileno(), 0)
        struct.pack_into("<5I", self.map, 0, MAGIC, width, height, width * 4, self.sequence)
        self.size = (width, height)
        print(f"clickshare-capture: {width}x{height} -> {self.path}", flush=True)

    def remove(self):
        if self.map:
            self.map.close()
            self.map = None
        self.size = (0, 0)
        Path(self.path).unlink(missing_ok=True)


class ScreenCast:
    def __init__(self, frame_file, bus, size_path=None):
        self.frame_file = frame_file
        self.bus = bus
        self.size_path = size_path
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.session = None
        self.pipeline = None
        self.active = False
        self.cancelled = False

    def start(self):
        self.active = True
        session_token = "clickshare" + secrets.token_hex(4)
        self._request("CreateSession", lambda token: GLib.Variant(
            "(a{sv})", ({"handle_token": GLib.Variant("s", token),
                         "session_handle_token": GLib.Variant("s", session_token)},)),
            self._on_session)

    def _request(self, method, build_args, on_response):
        token = "clickshare" + secrets.token_hex(4)
        path = f"{PORTAL_PATH}/request/{self.sender}/{token}"
        subscription = None

        def on_signal(_bus, _sender, _path, _iface, _signal, params):
            self.bus.signal_unsubscribe(subscription)
            if not self.active:
                return
            code, results = params.unpack()
            if code != 0:
                print(f"clickshare-capture: {method} cancelled (code {code})", file=sys.stderr)
                self.cancelled = True
                self.stop()
                return
            try:
                on_response(results)
            except GLib.Error as error:
                print(f"clickshare-capture: {error.message}", file=sys.stderr)
                self.stop()

        subscription = self.bus.signal_subscribe(
            PORTAL, "org.freedesktop.portal.Request", "Response", path, None,
            Gio.DBusSignalFlags.NO_MATCH_RULE, on_signal)
        try:
            self.bus.call_sync(PORTAL, PORTAL_PATH, SCREENCAST, method, build_args(token),
                               None, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error as error:
            print(f"clickshare-capture: {method} failed: {error.message}", file=sys.stderr)
            self.bus.signal_unsubscribe(subscription)
            self.stop()

    def _on_session(self, results):
        self.session = results["session_handle"]
        options = self._source_options()
        self._request("SelectSources", lambda token: GLib.Variant(
            "(oa{sv})", (self.session, {**options, "handle_token": GLib.Variant("s", token)})),
            lambda _results: self._request("Start", lambda token: GLib.Variant(
                "(osa{sv})", (self.session, "",
                              {"handle_token": GLib.Variant("s", token)})),
                self._on_started))

    def _source_options(self):
        return {
            "types": GLib.Variant("u", SOURCE_ANY),
            "multiple": GLib.Variant("b", False),
            "cursor_mode": GLib.Variant("u", CURSOR_EMBEDDED),
        }

    def _on_started(self, results):
        node_id, stream = results["streams"][0]
        caps = self._virtual_caps() if stream.get("source_type") == SOURCE_VIRTUAL else ""
        reply, fds = self.bus.call_with_unix_fd_list_sync(
            PORTAL, PORTAL_PATH, SCREENCAST, "OpenPipeWireRemote",
            GLib.Variant("(oa{sv})", (self.session, {})), GLib.VariantType("(h)"),
            Gio.DBusCallFlags.NONE, -1, None, None)
        fd = fds.get(reply.unpack()[0])
        self.pipeline = Gst.parse_launch(
            f"pipewiresrc fd={fd} path={node_id} always-copy=true do-timestamp=true ! "
            f"{caps}videoconvert ! video/x-raw,format=BGRA ! "
            "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false")
        self.pipeline.get_by_name("sink").connect("new-sample", self._on_sample)
        self.pipeline.get_bus().add_signal_watch()
        self.pipeline.get_bus().connect("message::error", self._on_error)
        self.pipeline.set_state(Gst.State.PLAYING)

    def _virtual_caps(self):
        """GNOME sizes a virtual screen from the format the consumer negotiates (1x1 if unset)."""
        try:
            with open(self.size_path) as file:
                width, height = (int(n) for n in file.read().split("x"))
        except (OSError, ValueError):
            return ""
        return f"video/x-raw,width={width},height={height} ! "

    def _on_sample(self, sink):
        sample = sink.emit("pull-sample")
        structure = sample.get_caps().get_structure(0)
        width, height = structure.get_value("width"), structure.get_value("height")
        buffer = sample.get_buffer()
        ok, info = buffer.map(Gst.MapFlags.READ)
        if ok:
            stride = info.size // height
            self.frame_file.write(width, height, stride, info.data)
            buffer.unmap(info)
        return Gst.FlowReturn.OK

    def _on_error(self, _bus, message):
        error, _debug = message.parse_error()
        print(f"clickshare-capture: {error.message}", file=sys.stderr)
        self.stop()

    def _close(self):
        self.active = False
        if self.pipeline:
            self.pipeline.get_bus().remove_signal_watch()
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        if self.session:
            self.bus.call_sync(PORTAL, self.session, "org.freedesktop.portal.Session", "Close",
                               None, None, Gio.DBusCallFlags.NONE, -1, None)
            self.session = None

    def stop(self):
        self._close()
        self.frame_file.remove()


class Snapshot(ScreenCast):
    """Writes one frame of a monitor each time the request file appears, reusing the restore
    token GNOME issued last time. After the user cancels GNOME's picker it stops asking."""

    def __init__(self, frame_file, bus, token_path, request_path):
        super().__init__(frame_file, bus)
        self.token_path = token_path
        self.request_path = request_path
        self.taken = False
        Path(request_path).unlink(missing_ok=True)
        self.monitor = Gio.File.new_for_path(request_path).monitor_file(Gio.FileMonitorFlags.NONE, None)
        self.monitor.connect("changed", self._on_changed)

    def _on_changed(self, _monitor, _file, _other, event):
        if event != Gio.FileMonitorEvent.CREATED or self.active:
            return
        if self.cancelled:
            Path(self.request_path).unlink(missing_ok=True)
            return
        self.start()

    def start(self):
        self.taken = False
        super().start()

    def _source_options(self):
        options = {
            **super()._source_options(),
            "types": GLib.Variant("u", SOURCE_MONITOR),
            "persist_mode": GLib.Variant("u", PERSIST_UNTIL_REVOKED),
        }
        if os.path.exists(self.token_path):
            with open(self.token_path) as file:
                options["restore_token"] = GLib.Variant("s", file.read().strip())
        return options

    def _on_started(self, results):
        # GNOME's restore tokens are single-use; each start issues the next one.
        if "restore_token" in results:
            os.makedirs(os.path.dirname(self.token_path), exist_ok=True)
            with open(self.token_path, "w") as file:
                file.write(results["restore_token"])
        super()._on_started(results)

    def _on_sample(self, sink):
        result = super()._on_sample(sink)
        if not self.taken:
            self.taken = True
            GLib.idle_add(self.stop)
        return result

    def stop(self):
        self._close()
        Path(self.request_path).unlink(missing_ok=True)
        return GLib.SOURCE_REMOVE


class RequestWatch:
    """Starts a capture when the request file appears and stops it once the file is gone."""

    def __init__(self, path, screencast):
        self.path = path
        self.screencast = screencast
        self.pending_stop = 0
        Path(path).unlink(missing_ok=True)
        self.monitor = Gio.File.new_for_path(path).monitor_file(Gio.FileMonitorFlags.NONE, None)
        self.monitor.connect("changed", self._on_changed)

    def _on_changed(self, _monitor, _file, _other, event):
        if event == Gio.FileMonitorEvent.CREATED:
            if self.pending_stop:
                GLib.source_remove(self.pending_stop)
                self.pending_stop = 0
            if not self.screencast.active:
                self.screencast.start()
        elif event == Gio.FileMonitorEvent.DELETED and not self.pending_stop:
            self.pending_stop = GLib.timeout_add(STOP_DELAY_MS, self._stop_if_gone)

    def _stop_if_gone(self):
        self.pending_stop = 0
        if not os.path.exists(self.path):
            self.screencast.stop()
        return GLib.SOURCE_REMOVE


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    Gst.init(None)
    loop = GLib.MainLoop()
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    frame_file = FrameFile(sys.argv[1])
    frame_file.remove()
    request_path = sys.argv[1] + ".request"
    screencast = ScreenCast(frame_file, bus, request_path)
    watch = RequestWatch(request_path, screencast)
    config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    snapshot_file = FrameFile(sys.argv[1] + ".thumbnail")
    snapshot_file.remove()
    snapshot = Snapshot(snapshot_file, bus, os.path.join(config, "clickshare-linux", "thumbnail_token"),
                        sys.argv[1] + ".thumbnail.request")
    if os.path.exists(snapshot.token_path):
        snapshot.start()
    for number in (signal.SIGINT, signal.SIGTERM):
        unix_signal_add(GLib.PRIORITY_DEFAULT, number, loop.quit)
    loop.run()
    if screencast.active:
        screencast.stop()
    if snapshot.active:
        snapshot.stop()
    snapshot_file.remove()
    Path(watch.path).unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
