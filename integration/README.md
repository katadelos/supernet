# Integrating Supernet

Supernet builds a controller executable. A device package supplies its
configuration, compatible services and optional graphical controls. The current
integration uses the e-ink SDK's installation layout and package manager.

## Package contents

Paths below are relative to the installation root: `/mnt/us/.eink-sdk` on
Kindle or `/mnt/onboard/.eink-sdk` on Kobo.

| Source | Package destination | Handling |
| --- | --- | --- |
| Built `supernet` | `usr/bin/supernet` | Executable |
| [config/supernet.conf](../config/supernet.conf) | `etc/supernet/config` | opkg conffile; preserve user edits |
| [opkg/supernet.prerm](opkg/supernet.prerm) | Package's `prerm` control script | Execute before removal/upgrade; abort on failure |

After a staged CMake install, add the configuration:

```sh
install -D -m 0644 config/supernet.conf stage/etc/supernet/config
```

See the [KPP Mods extension](kppmods/README.md) for Kindle Quick Settings and
Wi-Fi controls, or the [k2 integration](../docs/k2.md) for its feature descriptor.

SSH keys live in `/mnt/us/supernet` on Kindle and `/mnt/onboard/supernet` on
Kobo. Users create the directory and add `authorized_keys`; Supernet generates
`host_ed25519` there on first use. Preserve both files across upgrades and
keep them outside the package payload. Do not start remote access automatically
at installation.

The removal hook calls `prepare-remove` through
`/var/run/eink-sdk/usr/bin/supernet`. It refuses removal while owned SSH processes
remain, preserving active remote sessions. Otherwise the controller stops ADB,
cleans up its resources and exits before the hook succeeds.

## Runtime requirements

- `usr/sbin/dropbear` must be a regular multicall executable supporting the
  server and `dropbearkey` applets, Ed25519 host keys, `-D` for the authorized-key
  directory and `-l` for interface binding, plus the root session environment
  patch. Standard
  Dropbear builds may lack these features.
- File transfers need the appropriate SCP or SFTP programs. SFTP support is
  supplied by a separate server package.
- Optional ADB uses `usr/bin/adbd`, `etc/adb/adb_keys` and an existing USB ADB
  endpoint. The daemon must support the packaged key path and run without
  enabling a TCP listener by default.
- The environment supplies the runtime mount/link layout, including
  `/var/run/eink-sdk`, and uses `/tmp/eink-opkg.lock` for package operations.
- Kindle uses the firmware's `/usr/lib/liblipc.so` and iptables. Kobo uses
  configfs USB gadgets; `control/device` must contain `forma` or `elipsa2e`.
  Elipsa 2E loads `usb_f_ecm` and its dependencies through `control/modprobe`.

Supernet stages executables in private RAM storage at `/dev/supernet-sdk` and
keeps keys on userstore. Starting access refreshes the private Dropbear copy and
copies `authorized_keys` into the runtime directory with Unix permissions.
ADB runs from `/dev/supernet-sdk/bin/adbd`, so replacing or removing the package
does not hide the running process from cleanup. ADB receives SIGTERM if its
parent controller dies.

## Status and controls

Use the commands documented in the [README](../README.md#commands). Control
requests are serialized through a root-only Unix socket. Allow for USB
transitions and cleanup: the client can wait up to 75 seconds for a response.

`status-json` emits one JSON object with `schema: 1`. Exit status 0 means the
query succeeded, even if access is unavailable. Consumers should check the
schema and ignore unknown fields.

| Fields | Meaning |
| --- | --- |
| `version`, `platform` | Identity of the responding controller, or the invoked executable if no controller is running |
| `service`, `ssh`, `usbEnabled`, `wifiEnabled` | Controller and SSH transport state |
| `address`, `prefix`, `port`, `wifiAddress` | Connection settings |
| `activeSessions`, `keepsAwake` | Active SSH connections and current wake lease |
| `keysReady`, `keyPath` | SSH public-key readiness and file location |
| `canStartUsb`, `canStartWifi`, `canStartBoth`, `canStop` | Whether to offer SSH controls |
| `detail`, `error`, `usbError`, `wifiError` | State explanations and reasons a control is unavailable |
| `usbMode`, `adbAvailable`, `adb`, `adbError` | USB mode and ADB availability/state |
| `keepAwakeAvailable`, `keepAwake` | Manual keep-awake capability and requested setting |

`adbAvailable` stays true while Supernet's ADB daemon is running even if its
package has been removed, so clients can still offer the stop control. Package
operations block ADB startup but permit shutdown and removal cleanup.

The reported version comes from CMake's project version. `supernet --version`
identifies the invoked executable; a status response identifies the running
controller, which may be an older executable until it is restarted.

## Source layout

- `src/main.c` handles command-line entry and dispatch to the controller.
- `src/control/` contains the control socket, service lifecycle and JSON status.
- `src/access/` manages SSH keys, access checks, ADB and wake locks.
- `src/network/` handles IP configuration, Wi-Fi discovery and firewall rules.
- `src/platform/` contains Kindle and Kobo USB backends and Kindle LIPC calls.
- `src/runtime/` provides configuration, files, logging, processes and executable staging.
- `src/supernet.h` defines shared configuration and device paths; each module has
  its own interface header.

C sources use four-space indentation, braces on control flow and expanded
function bodies. `.clang-format` defines the formatting rules.
