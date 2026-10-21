# Supernet

Supernet provides on-demand SSH access to Kindle and Kobo readers over USB or
Wi-Fi. It prepares the USB network connection, manages a Dropbear SSH server,
and reports connection state. Run its commands from a root shell on the reader.

SSH uses public-key authentication, with no default password. Installing
Supernet does not enable remote access; you choose when to start and stop it.
Supernet can also manage an optional ADB daemon and keep the reader awake
while you work.

The optional [KPP Mods extension](integration/kppmods/README.md) adds an SSH
button to Kindle Quick Settings and a Wi-Fi SSH switch to Wi-Fi settings.

## Supported architectures

ARMv7 Linux (32-bit), using the target that matches the reader's userspace ABI:

| Build target | ABI | SDK root |
| --- | --- | --- |
| `kindle5` | Soft-float, older Kindle libc | `/mnt/us/.eink-sdk` |
| `kindlepw2` | Soft-float | `/mnt/us/.eink-sdk` |
| `kindlehf` | Hard-float | `/mnt/us/.eink-sdk` |
| `kobov4` | Hard-float | `/mnt/onboard/.eink-sdk` |

These are SDK platform names, not Kindle model numbers. Select the toolchain
for the installed firmware; a reader's ABI can change across firmware releases.

## Install and connect

You need a rooted reader. Install through the e-ink SDK's package manager,
which supplies the required dependencies.

1. In a root shell with the package manager available, install Supernet:

   ```sh
   opkg install supernet
   ```

2. Add your computer's OpenSSH **public** key to `authorized_keys` in
   Supernet's key directory:

   | Reader | Key directory |
   | --- | --- |
   | Kindle | `/mnt/us/supernet` |
   | Kobo | `/mnt/onboard/supernet` |

   Create the directory and file if needed, and preserve existing keys.
   `~/.ssh/id_ed25519.pub` on your computer is suitable. To create a key, run
   `ssh-keygen -t ed25519` on the computer and keep the private key there.
   Each public key must occupy a complete line, without options such as
   `from=` or `command=`.
3. From a root shell on the reader, run `supernet start-usb`,
   `supernet start-wifi` or `supernet start-both`. Connect Wi-Fi through the
   reader's normal settings first. Stop other USB networking controllers
   before starting USB access.
4. For USB, configure the computer's USB Ethernet interface with manual IPv4
   address **192.168.15.201**, subnet mask **255.255.255.0**, and no gateway or
   DNS. The reader's default address is **192.168.15.244**. Supernet does not
   provide DHCP or internet sharing.
5. Connect from your computer:

   ```sh
   ssh root@192.168.15.244
   ```

   For Wi-Fi, substitute the `wifiAddress` reported by `supernet status-json`.
   Use `ssh -i /path/to/private_key` if your key is not selected automatically.
   Confirm the reader's host-key fingerprint on first connection.

The computer needs a driver for the reader's USB network gadget. Kobo uses
CDC ECM; Kindle's gadget depends on the firmware. If no interface appears,
check the data cable and host driver before changing SSH settings. Supernet
configures IPv4 only. If the default subnet overlaps another network on the
computer, change both the reader and computer addresses to a different private
subnet.

File transfers use the installed SSH server's capabilities. An SFTP server
enables modern `scp` and SFTP clients. An installation with SCP support but no
SFTP server requires `scp -O` from an OpenSSH client.

### Stop, upgrade and remove

Run `supernet stop` on the reader to stop access. This disconnects
Supernet's SSH sessions and leaves other SSH servers alone. Stop access before
choosing a different transport, changing configuration or reconnecting after
a Wi-Fi address change.

On Kindle, USB remains in network mode to keep applications and their files
available on the reader; **reboot to restore USB file transfer**. On Kobo,
Supernet releases its USB gadget without exporting storage. USB access can
start before a cable is connected. With both transports selected, Wi-Fi is
available while USB waits for its host.

Stop SSH before upgrading or removing Supernet. The package removal hook
refuses to proceed while Supernet's SSH processes remain; otherwise it stops
ADB and exits the controller. `supernet shutdown` stops both SSH and ADB.

Keep `authorized_keys` and the generated `host_ed25519` in Supernet's key
directory across upgrades. The latter is the server's host key: preserving it
retains the reader's SSH identity. Restart SSH after editing `authorized_keys`;
Supernet uses a private runtime copy for authentication because userstore does
not provide Unix file permissions.

## Configuration

Edit `/mnt/us/.eink-sdk/etc/supernet/config` on Kindle or
`/mnt/onboard/.eink-sdk/etc/supernet/config` on Kobo.
The [default configuration](config/supernet.conf) is:

```ini
interface=usb0
address=192.168.15.244
prefix=24
port=22
timeout=20
```

The file contains plain `key=value` data. Blank lines and lines beginning with
`#` are allowed. Unknown keys, duplicate keys and invalid values are errors.
There is no shell expansion or inline comment syntax. Omitted settings use
their defaults, but the configuration file must exist to start access. Stopping
access uses the saved ownership state even if configuration is missing or invalid.

| Key | Default | Accepted values |
| --- | --- | --- |
| `interface` | `usb0` | Network interface name; USB control requires `usb0` |
| `address` | `192.168.15.244` | Reader's unicast IPv4 address; not a subnet or broadcast address |
| `prefix` | `24` | Network prefix length, 1–30 |
| `port` | `22` | SSH port, 1–65535 |
| `timeout` | `20` | Kindle USB transition timeout in seconds, 5–60 |

Select USB, Wi-Fi or both when starting access; there is no `wifi` configuration
key. Restart access after changing configuration. With a custom port, use
`ssh -p 2222 root@192.168.15.244` and `scp -P 2222 ...`.

## Commands

Run controls as root on the reader. `status-json` reads state without starting
services or changing USB mode. `--version` prints the executable's version and
build platform without requiring root.

| Command | Effect |
| --- | --- |
| `--version` | Print the executable's version and build platform |
| `status-json` | Read current state and available controls |
| `start-usb`, `start-wifi`, `start-both` | Start SSH on the requested transports |
| `stop` | Close owned SSH sessions and clean up owned networking resources |
| `adb-start`, `adb-stop` | Start/stop Supernet's ADB daemon |
| `awake-on`, `awake-off` | Request/release the manual keep-awake setting |
| `prepare-remove` | Refuse removal with owned SSH processes; otherwise clean up and exit |
| `shutdown` | Stop SSH and ADB, release the wake lease and exit |

Exit status 0 indicates success. A running controller sends an `OK` reply;
a command with nothing to clean up may succeed without output. Failures return
nonzero; inspect stderr and query status before retrying. `status-json` can
succeed while reporting that access is unavailable. Its `detail`, `error`,
`usbError` and `wifiError` fields explain the state. See the
[integration guide](integration/README.md#status-and-controls) for the JSON
interface used by graphical controls.

### ADB and keeping the reader awake

ADB requires `adbd` to be installed, a USB ADB endpoint configured on the
reader, and your computer's ADB public key in `/var/run/eink-sdk/etc/adb/adb_keys`.
Supernet does not switch the reader into ADB mode or
open an ADB TCP listener. USB SSH and ADB are mutually exclusive; Wi-Fi SSH
can run alongside ADB.

`stop` controls SSH only; use `adb-stop` to stop ADB alone. `shutdown`,
`prepare-remove` and normal controller exit also stop Supernet's ADB daemon.
Other controllers' ADB processes are left alone. Package operations block
`adb-start`, but stopping ADB remains possible during an upgrade or removal.

Manual keep-awake and active SSH sessions request an expiring wake lease on
supported kernels. Kindle Touch 5.3.7.3, Paperwhite 2 5.12.2.2 and Kobo Forma
4.38 do not expose the required wake controls; `keepAwakeAvailable` is false
on those firmwares. An idle SSH listener allows sleep. The manual request resets
when the controller exits or the reader reboots; an unexpected exit releases
the lease within 45 seconds. Stock firmware can still turn Wi-Fi off when the
foreground application exits.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Permission denied (publickey) | The correct public key is in Supernet's `authorized_keys` file, one complete line per key; the computer is using the matching private key. |
| No USB interface | Use a data cable, eject storage before starting, and check the host's USB Ethernet driver. Stop other USB networking tools. |
| Connection refused or timed out | Check status locally, the host's manual address, the configured SSH port and whether the reader is asleep. Wi-Fi must already be connected. |
| Another service uses the port | Stop that service explicitly or configure another port. Preflight reserves a port across all local addresses. |
| Package operation in progress | Allow the package operation to finish. Stop Supernet SSH before upgrade/removal. |
| USB preparation did not finish on Kindle | Start access to retry. Stop access releases a reservation if the reader is still in storage mode. |
| USB configuration failed on Kobo | Supernet retries failed connection setup three times, then stops SSH. Stop access and start again; inspect the log if the failure repeats. |
| Kobo cleanup incomplete | Retry Stop access, or reboot before starting again. |
| Kindle USB file transfer unavailable after stopping | Reboot to leave USB network mode and restore file transfers. |

Diagnostics are in `/dev/supernet-sdk/supernet.log` and, for ADB,
`/dev/supernet-sdk/adbd.log`. Each is limited to 256 KiB, discarding older contents
when full, and disappears at reboot. The controller also uses syslog and stderr.
When reporting a problem, include the reader model,
firmware version, `supernet --version`, `supernet status-json` and the relevant
log excerpt. Omit private keys and redact personal network details.

## Build

Requires CMake 3.25+, a C11 compiler for the reader's Linux userspace, and its
libc development files. Supply a toolchain file; the build fetches no
dependencies. For Kindle:

```sh
cmake -S . -B build/kindlehf \
  -DSUPERNET_PLATFORM=kindlehf \
  -DSUPERNET_SDK_ROOT=/mnt/us/.eink-sdk \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/device-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build/kindlehf
DESTDIR="$PWD/stage" cmake --install build/kindlehf
```

This stages `stage/usr/bin/supernet`. For `kindle5` or `kindlepw2`, use a
separate build directory, the corresponding soft-float toolchain and
`SUPERNET_PLATFORM`, with the same Kindle SDK root. For Kobo, use a separate
build directory, a Kobo toolchain, `SUPERNET_PLATFORM=kobov4` and
`SUPERNET_SDK_ROOT=/mnt/onboard/.eink-sdk`. `SUPERNET_SDK_ROOT` selects the
installation path on the reader, not the host staging directory. These are
the supported platform/path pairs. `CMAKE_INSTALL_BINDIR` controls where the
executable is installed within the prefix.

The build produces the controller executable. See
[package integration](integration/README.md) for configuration files,
dependencies and removal hooks.

## License

Supernet is licensed under the [MIT license](LICENSE). The vendored openlipc
declarations have their own [MIT license notice](vendor/openlipc/LICENSE.txt)
and come from revision `91645e77b291d92926b4292e31b2fa470322d1c7` of openlipc.
