# Kindle Quick Settings

This KPP Mods extension adds an **SSH** button to Quick Settings. Press it to
start or stop all Supernet SSH access. USB is the default connection. Enable
**Allow SSH connections over WiFi** on the Wi-Fi settings page to include Wi-Fi.
A native dialog shows USB and Wi-Fi connection state, or explains a failure.
SSH keys and connection settings follow the
[Supernet instructions](../../README.md#install-and-connect).

Use **Settings → Extensions → Supernet SSH** to show or hide the controls.
Without Supernet installed, the extension is unavailable and contributes no
button or Wi-Fi setting. Hiding the controls does not stop existing SSH access.

Stage the UI extension with KPP Mods:

```sh
python3 /path/to/kppmods/tools/packaging/package.py supernet /tmp/supernet-ui \
  --source /path/to/supernet/integration/kppmods
```

Install it as `kppmods-supernet` with a KPP Mods runtime supporting
`requiresExecutables`, Wi-Fi contributions and native dialog modules. The
manifest declares its UI host dependency; Supernet
is checked at runtime so the extension can remain installed without it.
After installing Supernet, enable **Supernet SSH** in Extensions.
