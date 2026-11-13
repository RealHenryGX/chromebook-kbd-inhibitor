# chromebook-kbd-inhibitor

A lightweight Linux daemon that disables a convertible laptop’s built-in keyboard when it's in tablet-mode, while leaving external keyboards active.

On the Lenovo C13 ThinkPad and Ideapad Flex 5i, the built-in keyboard remained active after switching into tablet mode. So I've figured out this tool listens for the kernel’s `SW_TABLET_MODE` event and toggles the built-in keyboard’s `inhibited` sysfs attribute.

The daemon discovers the tablet-mode switch at runtime, retries device and write failures, resynchronizes after dropped input events, and enables the built-in keyboard when it receives `SIGINT` or `SIGTERM`.

## Hardware support

The current installer is configured for the development ThinkPad:

- Built-in keyboard: `AT Translated Set 2 keyboard` on `serio0`
- Tablet-mode switch: `Tablet Mode Switch`, reporting `SW_TABLET_MODE`

My ThinkPad is a Chromebook variant, which is why the repository has a Chromebook-oriented name. Input device names and sysfs paths vary between models. To support another laptop, adjust the built-in keyboard name in `inhibitor/tablet-inhibitd.c` and `inhibitor/install-service.sh`, then update the `serio0` path marker in the installer to match that laptop’s internal keyboard.

The installer checks for the configured keyboard and its `inhibited` attribute before making system changes. The daemon finds a tablet-mode switch by its `SW_TABLET_MODE` capability.

## Requirements

- Linux with systemd
- bash, `make`, a C compiler, `pkg-config`, and libevdev development headers
- `sudo` privileges for system installation
- A built-in keyboard that exposes an `inhibited` sysfs attribute
- A tablet-mode switch that reports `SW_TABLET_MODE`

## Build

Run:

```bash
make -C inhibitor
```

## Install and start

Run the installer as your regular user from the project directory:

```bash
./inhibitor/install-service.sh
```

The installer builds the daemon, checks the configured built-in keyboard, installs the binary to `/usr/local/libexec/tablet-inhibitd`, writes `/etc/systemd/system/tablet-inhibitd.service`, then enables and starts that service.

## Service management

Run the install script with `--uninstall` arg as your regular user to remove:

```bash
./inhibitor/install-service.sh --uninstall
```

which stops & removes the service and binary.

## Configuration

The daemon selects a keyboard by its exact input-device name. Its default is `AT Translated Set 2 keyboard`; `TABLET_INHIBIT_KEYBOARD_NAME` can override that when launching the daemon directly. The installer writes the detected keyboard name into the systemd unit and also requires the keyboard’s sysfs path to match the configured internal-device marker.

The system service runs as root because changing the kernel input device’s `inhibited` attribute requires elevated privileges.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
