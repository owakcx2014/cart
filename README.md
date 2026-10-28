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



## Build & install

```sh
chmod +x install.sh
./install.sh
```

## Usage

```sh
cart                 # then drop a .deb onto the window
cart package.deb     # or open a package directly
```
or Just make the binary is the default app for opening `.deb` files 

## License

MIT, see [LICENSE](LICENSE).
