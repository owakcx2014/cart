# Cart

A small GTK3 app that installs `.deb` packages the way macOS installs apps from a DMG: drop the package on the window, then drag its icon onto the Applications folder.

## Features

- Drop a `.deb` anywhere on the window
- Reads the package name, version and icon straight from the package
- Drag the app icon onto **Applications** to install
- Asks for your sudo password in a dialog (never shown on the command line)
- Missing dependencies are resolved automatically through apt
- Installation result is shown in the status bar

## Requirements

- GTK 3
- `dpkg`, `apt`, `sudo`, `tar`
- A C compiler

On Debian/Ubuntu/KDE neon:

```sh
sudo apt install build-essential libgtk-3-dev pkg-config
```

## Build

```sh
gcc cart.c -o cart $(pkg-config --cflags --libs gtk+-3.0)
```

## Usage

```sh
./cart                 # then drop a .deb onto the window
./cart package.deb     # or open a package directly
```
or Just make the binary is the default app for opening `.deb` files 

## License

MIT, see [LICENSE](LICENSE).
