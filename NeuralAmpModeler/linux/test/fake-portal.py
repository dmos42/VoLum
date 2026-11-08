#!/usr/bin/env python3
"""A stand-in for org.freedesktop.portal.Desktop, for tests.

The real portal needs a person to click a file chooser, so it cannot be part of an
automated check. This script owns the same bus name on a throwaway session bus,
answers the requests the VoLum plug-in makes, and - the part that matters - refuses
any request whose payload it cannot read. Marshalling a filter list (a(sa(us))) or
a bytestring folder (ay) by hand is where the C++ side could go wrong silently:
here it is an error.

Usage: fake-portal.py [--cancel] [--legacy-rgb] [path1 path2 ...]
  --cancel      answer every request as "the user pressed Cancel" (exit code 1)
  --legacy-rgb  answer PickColor with the old "rgb" (ii) vardict entry
  path1 ...     URIs to answer an open request with (default: two sample files)
"""

import sys
from urllib.parse import quote

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "org.freedesktop.portal.Desktop"
ROOT_PATH = "/org/freedesktop/portal/desktop"
REQUEST_IFACE = "org.freedesktop.portal.Request"

INTROSPECTION = """
<node>
  <interface name="org.freedesktop.portal.FileChooser">
    <method name="OpenFile">
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <method name="SaveFiles">
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.portal.Screenshot">
    <method name="PickColor">
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
"""

# URIs, as the portal spells them - a path is not a URI and the client says so.
# The second one carries a space, which the client has to turn back into a path.
DEFAULT_OPEN = ["file:///tmp/volum-fake/capture.nam", "file:///tmp/volum-fake/room%20ir.wav"]
FAKE_FOLDER = "/tmp/volum-fake"


def uri(path):
    """A path spelled the way the portal spells it: file:// plus percent escapes."""
    return "file://" + quote(path)


def bytestring(value):
    """An 'ay' from the wire, which PyGObject hands over as a list of ints.

    A portal path is nul-terminated in that array, so the bytes are cut at the
    first zero. Anything else (a str, a list of strings) is the wrong type.
    """
    if isinstance(value, (bytes, bytearray)):
        raw = bytes(value)
    elif isinstance(value, list) and all(isinstance(v, int) and 0 <= v < 256 for v in value):
        raw = bytes(value)
    else:
        raise ValueError("expected an 'ay' of bytes, got %r" % (value,))
    return raw.split(b"\x00", 1)[0]


class FakePortal:
    def __init__(self, argv):
        self.cancel = "--cancel" in argv
        self.legacy_rgb = "--legacy-rgb" in argv
        self.paths = [a for a in argv if not a.startswith("--")] or DEFAULT_OPEN
        self.loop = GLib.MainLoop()
        self.failed = False

    # --- request plumbing ---------------------------------------------------

    def on_method_call(self, conn, sender, path, iface, method, params, invocation):
        parent_window, title, options = self.read_args(iface, method, params)

        try:
            token = self.check_request(iface, method, parent_window, title, options)
        except Exception as exc:  # noqa: BLE001 - a refusal is the test failing
            print("fake-portal: refused %s: %s" % (method, exc), file=sys.stderr)
            self.failed = True
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", str(exc))
            # Keep serving: a session bus activates a real portal the moment the name
            # is free, and the rest of the run would then be answering somebody else.
            return

        # What the request carried, spelled out for the shell to check.
        print(self.describe(method, title, options), file=sys.stderr)

        # Same spelling the real portal uses: a bus name inside an object path loses
        # its colon and its dots. ":1.7/volum1" would not be a path at all.
        handle = "%s/request/%s/%s" % (ROOT_PATH, sender.lstrip(":").replace(".", "_"), token)
        invocation.return_value(GLib.Variant("(o)", (handle,)))

        # A real desktop answers when a person clicks; a timer stands in for that
        # so the client has to be listening already, exactly as it must be.
        GLib.timeout_add(150, self.respond, conn, sender, handle, method, options)

    def respond(self, conn, sender, handle, method, options):
        # A throw here would be swallowed by the main loop, and the client would sit
        # in front of a dialog that was never answered: say what went wrong instead.
        try:
            self.emit_response(conn, sender, handle, method, options)
        except Exception as exc:  # noqa: BLE001
            print("fake-portal: answering %s failed: %s" % (method, exc), file=sys.stderr)
            self.failed = True
            conn.emit_signal(sender, handle, REQUEST_IFACE, "Response", GLib.Variant("(ua{sv})", (2, {})))
        return False

    def emit_response(self, conn, sender, handle, method, options):
        code = 1 if self.cancel else 0
        results = {}
        if not self.cancel:
            if method == "PickColor":
                if self.legacy_rgb:
                    results["rgb"] = GLib.Variant("(iii)", (128, 64, 32))
                else:
                    results["color"] = GLib.Variant("(ddd)", (0.5, 0.25, 0.125))
            else:
                results["uris"] = GLib.Variant("as", self.answer(method, options))
        conn.emit_signal(sender, handle, REQUEST_IFACE, "Response", GLib.Variant("(ua{sv})", (code, results)))
        return False

    def answer(self, method, options):
        """The URIs this desktop would hand back, after seeing what was asked."""
        if method != "SaveFile":
            return [uri(FAKE_FOLDER)] if options.get("directory") is not None else self.paths

        # A save answer is built from the hint the caller gave, so the test can see
        # that current_folder travelled correctly rather than being ignored.
        folder = options.get("current_folder")
        path = bytestring(folder).decode() if folder is not None else FAKE_FOLDER
        name = options.get("current_name") or "choisi pack.volumpack"
        return [uri(path + "/" + name)]

    # --- what the client sent us -------------------------------------------

    def read_args(self, iface, method, params):
        values = params.unpack()
        if iface.endswith("Screenshot"):
            if len(values) != 2:
                raise ValueError("PickColor takes (s, a{sv}), got %r" % (values,))
            return values[0], "", values[1]
        if len(values) != 3:
            raise ValueError("%s takes (s, s, a{sv}), got %r" % (method, values))
        return values[0], values[1], values[2]

    def check_request(self, iface, method, parent_window, title, options):
        # params.unpack() already turned the wire types into Python ones, which is
        # the check we want: a folder hint that came over as a string instead of a
        # bytestring shows up here as the wrong Python type.
        token = options.get("handle_token")
        if not isinstance(token, str):
            raise ValueError("handle_token is %r, not a string" % (token,))
        if not token.replace("_", "").isalnum():
            raise ValueError("handle_token %r is not a path element" % token)

        for key in options:
            if key not in ("handle_token", "multiple", "directory", "filters", "current_folder", "current_name"):
                raise ValueError("unknown option %r" % key)

        if iface.endswith("Screenshot"):
            return token

        if title in ("", "VoLum"):
            raise ValueError("the dialog was sent an empty title")

        folder = options.get("current_folder")
        if folder is not None and not isinstance(folder, (bytes, bytearray, list)):
            raise ValueError("current_folder arrived as %r; the portal wants an 'ay' path" % (folder,))

        name = options.get("current_name")
        if name is not None and not isinstance(name, str):
            raise ValueError("current_name arrived as %r; the portal wants an 's'" % (name,))

        filters = options.get("filters")
        directory = options.get("directory")

        if directory is not None:
            if directory is not True:
                raise ValueError("directory was sent as %r" % (directory,))
            if method != "OpenFile":
                raise ValueError("%s asked to pick a folder" % method)
            if filters is not None:
                raise ValueError("a folder chooser should not carry file filters")
            return token

        self.check_filters(filters)
        return token

    def check_filters(self, filters):
        """Shape only: a(sa(us)), a list of (name, list of (priority, pattern)).

        Whether the *content* is what the caller meant is the test's business, so the
        request is written out and the shell checks what it says. What is checked
        here is the marshalling, which nothing but a portal would notice.
        """
        if filters is None:
            return

        if not isinstance(filters, list):
            raise ValueError("filters arrived as %r, expected a list" % (filters,))

        for entry in filters:
            if not isinstance(entry, (tuple, list)) or len(entry) != 2:
                raise ValueError("filter %r is not a (name, patterns) pair" % (entry,))
            name, patterns = entry
            if not isinstance(name, str):
                raise ValueError("filter name is %r, not a string" % (name,))
            if not isinstance(patterns, list):
                raise ValueError("filter %r carries %r, not a list" % (name, patterns))
            for pattern in patterns:
                if not isinstance(pattern, (tuple, list)) or len(pattern) != 2 or not isinstance(pattern[1], str):
                    raise ValueError("filter %r carries %r, not a (priority, pattern) pair" % (name, pattern))

    def describe(self, method, title, options):
        """One line per request, for the shell to assert on."""
        folder = options.get("current_folder")
        filters = options.get("filters")
        return "request %s title=%r multiple=%s directory=%s folder=%s name=%s filters=%s" % (
            method,
            title,
            options.get("multiple") is True,
            options.get("directory") is True,
            bytestring(folder).decode() if folder is not None else "-",
            options.get("current_name") or "-",
            "none" if filters is None else ";".join("%s:%s" % (name, ",".join(p[1] for p in patterns)) for name, patterns in filters),
        )

    # --- bus plumbing -------------------------------------------------------

    def on_get_property(self, conn, sender, path, iface, prop):
        return GLib.Variant("u", 4 if iface.endswith("FileChooser") else 2)

    def run(self):
        # Sync on purpose: the main loop has to be running before the bus answers,
        # and starting it from a callback would be a loop that never begins. The
        # objects are exported before the name is taken, so a request that arrives
        # the moment the name exists is already handled.
        conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        node = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION)
        for iface in node.interfaces:
            conn.register_object(ROOT_PATH, iface, self.on_method_call, self.on_get_property, None)
        Gio.bus_own_name_on_connection(conn, BUS_NAME, Gio.BusNameOwnerFlags.NONE, None, None)
        self.loop.run()
        return 1 if self.failed else 0


def main():
    portal = FakePortal(sys.argv[1:])
    sys.exit(portal.run())


if __name__ == "__main__":
    main()
