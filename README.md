# obtools

A suckless-style (dwm/st philosophy) taskbar + status bar for Openbox.
Plain Xlib + Xft + Imlib2, `config.h`-driven, no window manager library,
no runtime deps beyond those.

## Components

- **obbar** - the persistent docked bar: live window-list taskbar on one
  side, signal-driven status blocks on the other.
- **oblist** - a standalone popup that renders a scriptable, clickable
  icon+label list (an "XP-style" list/menu, minimal and graphical), fed by
  any script via stdin.

## Build

```sh
make        # builds obbar and oblist
make install
```

Configuration is compile-time only, suckless style: edit `obbar/config.h`
/ `oblist/config.h` (generated from `config.def.h` on first build) and
rebuild. Do not edit `config.def.h` in place if you want `git pull` to
carry your local changes safely - it exists so `config.h` can be
regenerated from a known default.

## Status

`obbar` maps a dockable, EWMH-strut'd bar window with the default
white-on-black Sans scheme, runs the status block engine (each entry in
`blocks[]` re-run on its own timer and/or instantly via a dedicated
realtime signal, `pkill -RTMIN+N obbar`, or on click via `$BUTTON`), and
shows a live taskbar on the left (one button per window from
`_NET_CLIENT_LIST`, with an icon, left-click to activate, middle-click to
close). No multi-monitor awareness yet.
`oblist` is not implemented yet.

## Autostart with Openbox

Add to `~/.config/openbox/autostart`:

```sh
obbar &
```
