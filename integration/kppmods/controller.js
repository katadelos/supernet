export const controller = "/var/run/eink-sdk/usr/bin/supernet";

export async function command(extension, name) {
  const result = await extension.exec([controller, name], {
    user: "root",
    timeout: 90,
  });
  if (result.exitCode !== 0 || result.timedOut || result.cancelled) {
    throw new Error(
      result.stderr.trim() ||
        result.stdout.trim() ||
        "Supernet did not complete the request.",
    );
  }
  return result.stdout;
}

export function connectionSummary(status) {
  if (!status.ssh) {
    return "USB: disabled\nWi-Fi: disabled";
  }
  const usb = status.usbEnabled
    ? `${status.address}:${status.port}`
    : "disabled";
  const wifi = status.wifiEnabled
    ? `${status.wifiAddress}:${status.port}`
    : "disabled";
  return `USB: ${usb}\nWi-Fi: ${wifi}`;
}

export function checkStart(status, wifi, changing = false) {
  if (status.error) {
    throw new Error(status.error);
  }
  if (!status.keysReady) {
    throw new Error(`Add your SSH public key to ${status.keyPath}.`);
  }
  if (!status.canStartUsb && !(changing && status.usbEnabled)) {
    throw new Error(
      status.usbError || status.detail || "USB SSH is unavailable.",
    );
  }
  if (wifi && !status.wifiAvailable) {
    throw new Error(
      status.wifiError || "Connect to Wi-Fi before allowing Wi-Fi SSH.",
    );
  }
}
