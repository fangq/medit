#
#  terminal.py
#
#  Copyright (C) 2004-2010 by Yevgen Muntyan <emuntyan@users.sourceforge.net>
#  Copyright (C) 2014 by Yannick Duchêne
#
#  This file is part of medit.  medit is free software; you can
#  redistribute it and/or modify it under the terms of the
#  GNU Lesser General Public License as published by the
#  Free Software Foundation; either version 2.1 of the License,
#  or (at your option) any later version.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with medit.  If not, see <http://www.gnu.org/licenses/>.
#

import os
import moo
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Vte", "2.91")
from gi.repository import Gtk
from gi.repository import Gdk
from gi.repository import GObject
from gi.repository import GLib
from gi.repository import Pango
from moo import _
from moo import D_

try:
    from gi.repository import Vte
except ImportError:
    moo._cancel_plugin_loading()

TERMINAL_PLUGIN_ID = "Terminal"
COLOR_SCHEME_KEY = 'Plugins/Terminal/color_scheme'
SHELL_KEY = 'Plugins/Terminal/shell'
FONT_KEY = 'Plugins/Terminal/font'
moo.prefs_new_key_string(COLOR_SCHEME_KEY, 'White on Black')
moo.prefs_new_key_string(SHELL_KEY, None)
moo.prefs_new_key_string(FONT_KEY, None)


def shell_supports_pushd(shell):
    name = os.path.split(shell)[1]
    return name in ["bash"]


class Terminal(Vte.Terminal):
    def __init__(self):
        Vte.Terminal.__init__(self)

        cs_name = moo.prefs_get_string(COLOR_SCHEME_KEY)
        cs = find_color_scheme(cs_name)
        self.set_color_scheme(cs)
        # set_allow_bold removed in VTE 0.52+; bold is always allowed
        # self.set_allow_bold(False)

        font_name = moo.prefs_get_string(FONT_KEY)
        if font_name:
            self.set_font(Pango.FontDescription(font_name))

        self._support_pushd = False

    def set_support_pushd(self, status):
        self._support_pushd = status

    def set_color_scheme(self, cs):
        self.__cs = cs
        if cs:
            cs.set_on_terminal(self)

    def color_scheme_item_activated(self, item, color_scheme):
        if not item.get_active():
            return
        self.set_color_scheme(color_scheme)
        if color_scheme.colors:
            moo.prefs_set_string(COLOR_SCHEME_KEY, color_scheme.name)
        else:
            moo.prefs_set_string(COLOR_SCHEME_KEY, None)

    def font_item_activated(self, *whatever):
        dlg = Gtk.FontChooserDialog(title=D_("Pick a Font", "gtk30"))
        old_font = self.get_font()
        if old_font:
            dlg.set_font_desc(old_font)
        if dlg.run() == Gtk.ResponseType.OK:
            font_name = dlg.get_font()
            if font_name:
                new_font = Pango.FontDescription(font_name)
                if old_font is None or not old_font.equal(new_font):
                    self.set_font(new_font)
                    moo.prefs_set_string(FONT_KEY, font_name)
        dlg.destroy()

    def fill_settings_menu(self, menu):
        item = Gtk.MenuItem(label=_("Select Font"))
        item.connect('activate', self.font_item_activated)
        menu.append(item)

        item = Gtk.MenuItem(label=_("Color Scheme"))
        submenu = Gtk.Menu()
        item.set_submenu(submenu)
        group = []
        for cs in color_schemes:
            child = Gtk.RadioMenuItem.new_with_label(group, cs.name)
            group = child.get_group()
            submenu.append(child)
            if self.__cs == cs:
                child.set_active(True)
            child.connect('activate', self.color_scheme_item_activated, cs)
        menu.append(item)

    def popup_menu(self, event=None):
        menu = Gtk.Menu()

        item = Gtk.MenuItem(label=_("Copy"))
        item.connect('activate', lambda *w: self.copy_clipboard())
        item.set_sensitive(self.get_has_selection())
        menu.append(item)
        item = Gtk.MenuItem(label=_("Paste"))
        item.connect('activate', lambda *w: self.paste_clipboard())
        menu.append(item)

        item = Gtk.MenuItem(label=_("\u201ccd\u201d to current file directory"))
        item.connect(
            'activate',
            lambda *w: self.goto_file_dir(False))
        menu.append(item)
        if self._support_pushd:
            item = Gtk.MenuItem(label=_("\u201cpushd\u201d to current file directory"))
            item.connect(
                'activate',
                lambda *w: self.goto_file_dir(True))
            menu.append(item)

        menu.append(Gtk.SeparatorMenuItem())

        item = Gtk.MenuItem(label=_("Settings"))
        submenu = Gtk.Menu()
        item.set_submenu(submenu)
        menu.append(item)

        self.fill_settings_menu(submenu)

        menu.show_all()
        if event is not None:
            menu.popup_at_pointer(event)
        else:
            menu.popup_at_widget(self, Gdk.Gravity.CENTER, Gdk.Gravity.CENTER, None)

    def goto_file_dir(self, pushd=False):
        ed = moo.Editor.instance()
        doc = ed.get_active_doc()
        if doc is None:
            return
        path = doc.get_filename()
        if path is None:
            return
        path = os.path.split(path)[0]
        if pushd:
            command = "pushd"
        else:
            command = "cd"
        command += " '" + path + "'\n"
        self.feed_child(command.encode('utf-8'))

    def do_button_press_event(self, event):
        if event.button != 3 or event.type != Gdk.EventType.BUTTON_PRESS:
            return Vte.Terminal.do_button_press_event(self, event)
        self.grab_focus()
        self.popup_menu(event)
        return True

    def do_popup_menu(self):
        self.popup_menu()
        return True


class Plugin(moo.Plugin):
    def do_init(self):
        editor = moo.Editor.instance()
        xml = editor.get_ui_xml()

        if xml is None:
            return False

        self.set_win_plugin_type(WinPlugin)
        return True

    def do_deinit(self):
        pass

    def show_terminal(self, window):
        pane = window.get_pane(TERMINAL_PLUGIN_ID)
        window.paned.present_pane(pane)


class WinPlugin(moo.WinPlugin):
    def start(self, *whatever):
        self.terminal.reset(True, True)

        shell = moo.prefs_get_string(SHELL_KEY)
        if not shell:
            try:
                import pwd
                shell = pwd.getpwuid(os.getuid())[6]
            except Exception:
                shell = "/bin/sh"

        self.terminal.set_support_pushd(shell_supports_pushd(shell))

        # VTE 0.48+ spawn API
        try:
            # Try spawn_async first (VTE >= 0.63)
            self.terminal.spawn_async(
                Vte.PtyFlags.DEFAULT,
                None,           # working directory
                [shell],        # argv
                None,           # envv
                GLib.SpawnFlags.DEFAULT,
                None,           # child_setup
                None,           # child_setup_data
                -1,             # timeout (-1 = default)
                None,           # cancellable
                None,           # callback
            )
        except (AttributeError, TypeError):
            # Fallback to spawn_sync (VTE 0.48-0.62)
            try:
                self.terminal.spawn_sync(
                    Vte.PtyFlags.DEFAULT,
                    None,           # working directory
                    [shell],        # argv
                    None,           # envv
                    GLib.SpawnFlags.DEFAULT,
                    None,           # child_setup
                    None,           # child_setup_data
                    None,           # cancellable
                )
            except Exception as e:
                print("Failed to spawn terminal shell:", e)

    def do_create(self):
        label = moo.PaneLabel(icon_name="utilities-terminal",
                              label_text=_("Terminal"))

        self.terminal = Terminal()
        self.terminal.set_scrollback_lines(1000000)
        self.terminal.connect("child-exited", self._on_child_exited)
        self.start()

        frame = Gtk.Frame()
        hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        frame.add(hbox)
        hbox.pack_start(self.terminal, True, True, 0)
        scrollbar = Gtk.Scrollbar(orientation=Gtk.Orientation.VERTICAL,
                                  adjustment=self.terminal.get_vadjustment())
        hbox.pack_start(scrollbar, False, False, 0)
        frame.show_all()

        self.terminal.set_size(self.terminal.get_column_count(), 10)
        self.terminal.set_size_request(10, 10)

        window = self.get_window()
        if window is None:
            print("Terminal plugin: window not ready yet", file=__import__("sys").stderr)
            return False
        self.pane = window.add_pane(TERMINAL_PLUGIN_ID, frame, label, moo.PANE_POS_BOTTOM)
        self.icon_title_changed_cb_id = self.terminal.connect('icon-title-changed', self.terminal_icon_title_changed)
        self.terminal_icon_title_changed()

        return True

    def _on_child_exited(self, terminal, status):
        # child-exited in VTE 0.48+ passes exit status
        self.start()

    def do_destroy(self):
        self.terminal.disconnect(self.icon_title_changed_cb_id)
        self.get_window().remove_pane(TERMINAL_PLUGIN_ID)

    def terminal_icon_title_changed(self, *whatever):
        title = self.terminal.get_icon_title()
        if title:
            self.pane.set_frame_text(title)


def _parse_color(color_string):
    """Parse a color string into a Gdk.RGBA."""
    rgba = Gdk.RGBA()
    rgba.parse(color_string)
    return rgba


class ColorScheme(object):
    def __init__(self, name, colors):
        object.__init__(self)
        self.name = name
        if colors is None:
            self.colors = None
        else:
            self.colors = [_parse_color(c) for c in colors]

    def set_on_terminal(self, term):
        if self.colors is not None:
            fg = self.colors[0]
            bg = self.colors[1]
            palette = self.colors[2:10] + self.colors[12:20]
            term.set_colors(fg, bg, palette)
        else:
            # Use default colors — reset by setting None
            ctx = term.get_style_context()
            fg = ctx.get_color(Gtk.StateFlags.NORMAL)
            bg_found, bg = ctx.lookup_color("theme_base_color")
            if not bg_found:
                bg = Gdk.RGBA()
                bg.parse("#ffffff")
            term.set_colors(fg, bg, [])


# Color schemes shamelessly stolen from Konsole
color_schemes = [ColorScheme(cs[0], cs[1]) for cs in [
    [_("Default"),
        ['#b2b2b2', '#000000',
         '#000000', '#b21818', '#18b218', '#b26818', '#5C5CFF', '#b218b2', '#18b2b2', '#b2b2b2',
         '#ffffff', '#000000',
         '#686868', '#ff5454', '#54ff54', '#ffff54', '#7D7DFF', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Black on White"),
        ['#000000', '#ffffff', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#000000', '#ffffff', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Black on Light Yellow"),
        ['#000000', '#ffffdd', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#000000', '#ffffdd', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Marble"),
        ['#ffffff', '#000000', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#ffffff', '#000000', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Green on Black"),
        ['#18f018', '#000000', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#18f018', '#000000', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Paper, Light"),
        ['#000000', '#ffffff', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#000000', '#ffffff', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Paper"),
        ['#000000', '#ffffff', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#000000', '#ffffff', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("Linux Colors"),
        ['#b2b2b2', '#000000', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#ffffff', '#686868', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']],
    [_("VIM Colors"),
        ['#000000', '#ffffff', '#000000', '#c00000', '#008000', '#808000', '#0000c0', '#c000c0', '#008080', '#c0c0c0',
         '#4d4d4d', '#ffffff', '#808080', '#ff6060', '#00ff00', '#ffff00', '#8080ff', '#ff40ff', '#00ffff', '#ffffff']],
    [_("White on Black"),
        ['#ffffff', '#000000', '#000000', '#b21818', '#18b218', '#b26818', '#1818b2', '#b218b2', '#18b2b2', '#b2b2b2',
         '#ffffff', '#000000', '#686868', '#ff5454', '#54ff54', '#ffff54', '#5454ff', '#ff54ff', '#54ffff', '#ffffff']]
]]


def find_color_scheme(name):
    for cs in color_schemes:
        if cs.name == name:
            return cs


GObject.type_register(Terminal)
GObject.type_register(Plugin)
GObject.type_register(WinPlugin)
__plugin__ = Plugin