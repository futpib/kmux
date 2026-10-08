# kmux - Konsole with Native tmux Integration

**Like [iTerm2's tmux integration](https://iterm2.com/documentation-tmux-integration.html), but for Linux/KDE.**

kmux is a fork of [Konsole](https://konsole.kde.org) (KDE's terminal emulator)
that adds first-class tmux integration. Instead of running tmux inside a plain
terminal, kmux communicates directly with a tmux server via tmux's control
mode (`tmux -CC`), mapping tmux windows and panes onto native Konsole tabs and
splits — detach, reattach, and remote sessions included, with the full
Konsole feature set on top.

https://github.com/user-attachments/assets/65cf72f8-b589-448d-a1ac-8d8eaed0d316

If you've used `tmux -CC` with iTerm2 on a Mac and missed it on Linux, this
is that. The only comparable Linux option is WezTerm's (still-maturing)
control-mode support; kmux instead builds the integration on Konsole's full
KDE feature set — profiles, color schemes, KPart embedding, bookmarks.

## Features

- **Native tmux control-mode bridge** — kmux starts tmux in control mode
  (`tmux -C`) and maps tmux state directly into native UI elements instead of
  running tmux inside a nested terminal.
- **Tabs and splits synchronized with tmux** — tmux windows map to Konsole tabs
  and tmux panes map to native split containers, with focus/layout updates kept
  in sync in both directions.
- **Full pane/window operations from kmux UI** — split, close, move, swap,
  break, and select panes/windows from kmux actions, with tmux updated
  immediately.
- **Zoom/maximize parity** — tmux pane zoom and Konsole maximize actions mirror
  each other (including the `Ctrl+Shift+E` zoom workflow).
- **Prefix key palette (dynamic)** — pressing the tmux prefix key opens a
  popup of live tmux key bindings; selecting an entry executes its tmux command.
- **Native choose-tree replacement** — tmux `choose-tree` bindings are
  intercepted and opened as a kmux popup, with panes/windows/sessions modes and
  fuzzy filtering.
- **Server-wide tree switcher** — quickly jump to any tmux session, window, or
  pane from a searchable hierarchical picker.
- **Multi-window tmux workflows** — detach a tmux-backed tab into a new kmux
  window and merge it back later, while staying attached to the same tmux
  session.
- **Session lifecycle actions** — detach the current tmux client or create and
  switch to a brand-new tmux session directly from kmux actions.
- **Working-directory aware creation** — new panes/windows/sessions preserve the
  expected working directory behavior during tmux operations.
- **Graceful reboot recovery** — Plasma session restore reattaches surviving
  local or remote tmux servers. If a local server was lost in the reboot, kmux
  safely rebuilds its windows, panes, layouts, and working directories with
  fresh shells; it never guesses at or reruns foreground commands.
- **Remote/socket-aware startup** — attach/create sessions by tmux session name,
  socket name/path, custom tmux binary path, and optional remote shell wrapper
  (`--rsh` / `KMUX_RSH`). Select the tmux session with `-s NAME` or
  `--tmux-session NAME`; `--session` is reserved for Qt desktop-session restore.
  For example: `kmux --rsh 'ssh user@host' --socket /path/to/tmux.sock --tmux-session work`.
- **Interactive remote login from the desktop** — when launched without a
  controlling terminal, `--rsh` opens a terminal window for passwords, host
  confirmations, and other input. It closes when tmux connects; Cancel stops
  the connection. Launches from a terminal keep using that terminal.
- **Remote host manager** — open a host from the SSH Manager as a remote tmux
  workspace, with `~/.ssh/config` aliases, GUI authentication prompts, and
  per-host tmux session/socket/path overrides.
- **Full Konsole feature set** — color schemes, keyboard shortcuts, KPart
  embedding, profiles, bookmarks, and everything else Konsole provides.

## Installation

### Arch Linux (recommended)

kmux is packaged on the AUR as
[`kmux-git`](https://aur.archlinux.org/packages/kmux-git):

```
yay -S kmux-git        # or: paru -S kmux-git
```

### Build from source (other distributions / development)

1. Install dependencies. On Arch Linux:
   ```
   pacman -S base-devel cmake ninja git extra-cmake-modules \
     qt6-base qt6-multimedia qt6-5compat qt6-tools \
     kcoreaddons kconfig kio kparts kcrash knotifications \
     kxmlgui kbookmarks kiconthemes kconfigwidgets kwindowsystem \
     kpty kdbusaddons kglobalaccel knewstuff knotifyconfig \
     ktextwidgets kwidgetsaddons kservice ki18n \
     icu tmux util-linux
   ```
   On Ubuntu/Debian (neon):
   ```
   apt install git cmake make g++ extra-cmake-modules \
     libkf6config-dev libkf6auth-dev libkf6package-dev \
     libkf6declarative-dev libkf6coreaddons-dev libkf6kcmutils-dev \
     libkf6i18n-dev libkf6crash-dev libkf6newstuff-dev \
     libkf6textwidgets-dev libkf6iconthemes-dev libkf6dbusaddons-dev \
     libkf6notifyconfig-dev libkf6pty-dev libkf6notifications-dev \
     libkf6parts-dev qt6-base-dev libqt6core6t64 libqt6widgets6 \
     libqt6gui6 libqt6qml6 qt6-multimedia-dev libicu-dev
   ```
2. Clone: `git clone https://github.com/futpib/kmux.git`
3. Configure: `cmake -B kmux/build -G Ninja kmux/`
4. Build: `cmake --build kmux/build`
5. Install: `cmake --install kmux/build`

## Troubleshooting

### Modified keys stop working after a tmux reconnect

In a long-lived tmux pane, terminal applications such as Codex can stop
distinguishing modified keys after kmux reconnects. For example, `Shift+Enter`,
`Ctrl+Enter`, `Alt+Enter`, `Shift+Tab`, or `Shift+Backspace` may be received as
their unmodified or legacy equivalents. This can affect both local sessions and
remote sessions opened with `kmux --rsh`.

These applications enable the
[Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/)
when they enter their interactive terminal mode. kmux records the protocol state
it observes in the tmux pane option `@kmux-kitty-keyboard-state`, then restores
that state when it attaches again. Check the current pane from a shell inside it:

```sh
tmux show-options -p -v -t "$TMUX_PANE" @kmux-kitty-keyboard-state
```

A value such as `5/` means kmux has recorded the state. No output means it has
not. This commonly happens when the application enabled the protocol before a
state-aware kmux client attached, such as in a pane that survived a kmux upgrade.

For Codex, press `Ctrl+Z` once. Codex leaves and re-enters its terminal mode,
causing it to send the protocol setup again while kmux is present. If this
suspends Codex and returns to a shell prompt, run `fg`. Restarting Codex has the
same effect. Check the pane option again; once it contains a value, later kmux
reconnects can restore the mode. Other terminal applications may have their own
suspend/resume behavior, so save important input before trying it.

kmux deliberately does not guess when the pane option is absent. An ordinary
shell and an application that negotiated before kmux attached look identical at
that point, while forcing the protocol on can break input in shells and editors.

tmux 3.7c provides xterm-style `extended-keys`, but it does not retain the Kitty
keyboard protocol's per-pane flag stack. Native tmux support is being tracked in
[tmux PR 5615](https://github.com/tmux/tmux/pull/5615); the related design and
status discussion is in
[tmux discussion 4644](https://github.com/orgs/tmux/discussions/4644).

## Directory Structure

| Directory        | Description                                                                                                              |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `/src/tmux`      | tmux integration: gateway (control-mode parser), controller, layout/pane managers, prefix palette, tree switcher.       |
| `/src`           | All other Konsole source code, including session management, terminal emulation, views, profiles, and plugins.           |
| `/doc/user`      | README files for advanced users covering fonts, keyboard handling, and other Konsole internals.                          |
| `/doc/developer` | Developer docs covering Konsole's design and the VT100 emulation layer.                                                  |
| `/desktop`       | `.desktop` and AppStream metadata files used by KDE application launchers.                                               |
| `/data`          | Color schemes, keyboard layouts, and other runtime data files.                                                           |
| `/src/autotests` | Qt/CTest unit and integration tests, including the tmux integration suite. Run via `ctest --test-dir build`.             |
| `/scripts/autotests` | End-to-end bash tests that drive a real kmux process under Xvfb. Run via `USE_XVFB=1 ./scripts/autotests/run-all.sh`. |
| `/tests`         | Manual test cases (text files and feature scripts) for ad-hoc verification — not run by CI.                              |

## Upstream

kmux is based on [Konsole](https://invent.kde.org/utilities/konsole). Bug
reports and feature requests that are not specific to the tmux integration
should be directed to the [Konsole project](https://bugs.kde.org/describecomponents.cgi?product=konsole).

## Quick Links
- [kmux on GitHub](https://github.com/futpib/kmux)
- [kmux-git on the AUR](https://aur.archlinux.org/packages/kmux-git)
- [CI / Builds](https://github.com/futpib/kmux/actions)
- [Upstream Konsole](https://konsole.kde.org)
