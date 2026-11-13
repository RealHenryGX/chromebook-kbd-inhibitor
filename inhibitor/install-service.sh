#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8

readonly SERVICE_NAME="tablet-inhibitd.service"
readonly UNIT_PATH="/etc/systemd/system/${SERVICE_NAME}"
readonly INSTALL_PATH="/usr/local/libexec/tablet-inhibitd"
readonly INTERNAL_KEYBOARD_NAME="AT Translated Set 2 keyboard"
readonly INTERNAL_SYSFS_MARKER="/serio0/input/"

usage() {
    cat <<'EOF'
Build, install, or uninstall tablet-inhibitd as a system service.

Run this script as your regular user. It builds in the project directory and
uses sudo only for system installation and systemd operations.

Usage:
  ./install-service.sh            Build, install, enable, and start the service
  ./install-service.sh --uninstall Stop and remove the service and daemon
  ./install-service.sh --help      Show this help
EOF
}

stop_disable_unit() {
    local unit=$1
    if sudo systemctl is-active --quiet "$unit"; then
        sudo systemctl stop "$unit"
    fi
    if sudo systemctl is-enabled --quiet "$unit"; then
        sudo systemctl disable "$unit"
    fi
}

mode="install"
case "${1:-}" in
    "" ) ;;
    -h|--help)
        usage
        exit 0
        ;;
    --uninstall)
        mode="uninstall"
        shift
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if (($# != 0)); then
    usage >&2
    exit 2
fi

if ((EUID == 0)); then
    printf 'Run this script as your regular user; it invokes sudo for installation.\n' >&2
    exit 1
fi

for command in sudo systemctl; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "$command" >&2
        exit 1
    fi
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

if [[ $mode == "uninstall" ]]; then
    stop_disable_unit "$SERVICE_NAME"
    sudo rm -f -- "$UNIT_PATH"
    sudo rm -f -- "$INSTALL_PATH"
    sudo systemctl daemon-reload
    sudo systemctl reset-failed "$SERVICE_NAME" || true
    printf 'Uninstalled %s; removed %s\n' "$SERVICE_NAME" "$INSTALL_PATH"
    exit 0
fi

for command in make realpath; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "$command" >&2
        exit 1
    fi
done

printf 'Building daemon...\n'
make -C "$script_dir"

keyboard_path=""
keyboard_event=""
for name_file in /sys/class/input/event*/device/name; do
    [[ -r $name_file ]] || continue
    name=$(<"$name_file")
    [[ $name == "$INTERNAL_KEYBOARD_NAME" ]] || continue

    device_link=${name_file%/name}
    device_path=$(realpath -e "$device_link" 2>/dev/null) || continue
    [[ $device_path == *"$INTERNAL_SYSFS_MARKER"* ]] || continue
    [[ -e $device_path/inhibited ]] || continue

    if [[ -n $keyboard_path && $keyboard_path != "$device_path/inhibited" ]]; then
        printf 'Multiple built-in keyboard devices matched; refusing ambiguous configuration.\n' >&2
        exit 1
    fi
    keyboard_path="$device_path/inhibited"
    keyboard_event=${name_file%/device/name}
    keyboard_event=${keyboard_event##*/}
done

if [[ -z $keyboard_path ]]; then
    printf 'Could not find %s on the internal serio0 bus with an inhibited attribute.\n' \
        "$INTERNAL_KEYBOARD_NAME" >&2
    exit 1
fi

printf 'Built-in keyboard: %s (%s)\n' "$INTERNAL_KEYBOARD_NAME" "$keyboard_event"
printf 'Inhibit attribute: %s\n' "$keyboard_path"

sudo install -D -m 0755 "$script_dir/tablet-inhibitd" "$INSTALL_PATH"

unit_tmp=$(mktemp "$script_dir/.tablet-inhibitd.service.XXXXXX")
trap 'rm -f -- "$unit_tmp"' EXIT
cat >"$unit_tmp" <<EOF
[Unit]
Description=Disable the built-in keyboard in tablet mode
After=systemd-udevd.service

[Service]
Type=simple
User=root
Environment="TABLET_INHIBIT_KEYBOARD_NAME=${INTERNAL_KEYBOARD_NAME}"
ExecStart=${INSTALL_PATH}
Restart=on-failure
RestartSec=2s
TimeoutStopSec=10s

[Install]
WantedBy=multi-user.target
EOF

sudo install -D -m 0644 "$unit_tmp" "$UNIT_PATH"
sudo systemctl daemon-reload

if sudo systemctl is-active --quiet "$SERVICE_NAME"; then
    sudo systemctl stop "$SERVICE_NAME"
fi

sudo systemctl enable --now "$SERVICE_NAME"

printf 'Installed and started %s\n' "$SERVICE_NAME"
sudo systemctl --no-pager --full status "$SERVICE_NAME" || true
