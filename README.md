# kmux - Konsole with Native tmux Integration

kmux is a fork of [Konsole](https://konsole.kde.org) (KDE's terminal emulator)
that adds first-class tmux integration. Instead of running tmux inside a plain
terminal, kmux communicates directly with a tmux server via tmux's control
mode (`tmux -CC`), mapping tmux windows and panes onto native Konsole tabs and
splits.

## Features

- **Native tmux pane/window management** — tmux windows appear as Konsole tabs;
  pane splits are reflected as native Konsole split-view containers.
- **Prefix key palette** — pressing your tmux prefix key (e.g. `Ctrl+B`) opens
  a popup listing all bound tmux commands, letting you discover and invoke them
  without leaving the keyboard.
- **Session/window/pane tree switcher** — a searchable popup lets you jump
  directly to any tmux session, window, or pane.
- **Transparent resize coordination** — the terminal and tmux are kept in sync
  so layout changes in either direction are applied consistently.
- **Full Konsole feature set** — color schemes, keyboard shortcuts, KPart
  embedding, profiles, bookmarks, and everything else Konsole provides.

## Building

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

## Directory Structure

| Directory        | Description                                                                                                              |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `/src/tmux`      | tmux integration: gateway (control-mode parser), controller, layout/pane managers, prefix palette, tree switcher.       |
| `/src`           | All other Konsole source code, including session management, terminal emulation, views, profiles, and plugins.           |
| `/doc/user`      | README files for advanced users covering fonts, keyboard handling, and other Konsole internals.                          |
| `/doc/developer` | Developer docs covering Konsole's design and the VT100 emulation layer.                                                  |
| `/desktop`       | `.desktop` and AppStream metadata files used by KDE application launchers.                                               |
| `/data`          | Color schemes, keyboard layouts, and other runtime data files.                                                           |
| `/tests`         | Automated tests, including tmux integration tests.                                                                       |

## Upstream

kmux is based on [Konsole](https://invent.kde.org/utilities/konsole). Bug
reports and feature requests that are not specific to the tmux integration
should be directed to the [Konsole project](https://bugs.kde.org/describecomponents.cgi?product=konsole).

## Quick Links
- [kmux on GitHub](https://github.com/futpib/kmux)
- [CI / Builds](https://github.com/futpib/kmux/actions)
- [Upstream Konsole](https://konsole.kde.org)

