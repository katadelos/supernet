import {
  controller,
  command,
  connectionSummary,
  checkStart,
} from "./controller";
import { createDialog } from "./dialog";

export function activate(extension) {
  const showDialog = createDialog(extension);
  let state = null;
  let disposed = false;
  let busy = false;
  let timer = null;
  let reading = null;
  let polling = false;
  const listeners = new Set();
  const allowWifi = () => extension.settings.get("allowWifi", false);

  function notify() {
    listeners.forEach((listener) => listener());
  }

  async function readStatus() {
    const installed = await extension.exec(
      [
        "/bin/sh",
        "-c",
        'test -f "$1" && test -x "$1"',
        "supernet-installed",
        controller,
      ],
      { user: "root", timeout: 5 },
    );
    if (disposed) {
      return null;
    }
    if (installed.exitCode === 1) {
      extension.quickSettings.remove("ssh");
      extension.wifiSettings.remove("ssh");
      throw new Error("Supernet is not installed.");
    }
    if (installed.exitCode !== 0 || installed.timedOut || installed.cancelled) {
      throw new Error("Could not check whether Supernet is installed.");
    }
    const next = JSON.parse(await command(extension, "status-json"));
    if (next.schema !== 1 || typeof next.ssh !== "boolean") {
      throw new Error("Supernet returned an invalid status.");
    }
    if (!disposed) {
      state = next;
      notify();
    }
    return next;
  }

  function refresh() {
    if (!reading) {
      reading = readStatus()
        .catch((error) => {
          state = null;
          notify();
          throw error;
        })
        .finally(() => {
          reading = null;
        });
    }
    return reading;
  }

  function poll() {
    if (disposed || polling || !listeners.size) {
      return;
    }
    polling = true;
    const update = busy ? Promise.resolve() : refresh();
    update
      .catch((error) => extension.log(error.message))
      .finally(() => {
        polling = false;
        if (!disposed && listeners.size) {
          timer = setTimeout(poll, 5000);
        }
      });
  }

  function subscribe(listener) {
    listeners.add(listener);
    if (listeners.size === 1) {
      poll();
    }
    return () => {
      listeners.delete(listener);
      if (!listeners.size) {
        clearTimeout(timer);
      }
    };
  }

  // Keep command failures separate from dialog failures: never retry the action.
  async function runAction(work) {
    if (busy || disposed) {
      return;
    }
    busy = true;
    notify();
    let title;
    let message;
    try {
      [title, message] = await work();
    } catch (error) {
      extension.log(error.message);
      const current = await refresh().catch(() => null);
      title = "SSH could not be changed";
      message = error.message;
      if (current) {
        message += `\n\n${connectionSummary(current)}`;
      }
    }
    try {
      if (!disposed) {
        await showDialog(title, message);
      }
    } finally {
      busy = false;
      notify();
    }
  }

  async function toggleSsh() {
    const current = await refresh();
    if (disposed) {
      return ["", ""];
    }
    const enabling = !current.ssh;
    if (enabling) {
      checkStart(current, allowWifi());
    }
    await command(
      extension,
      enabling ? (allowWifi() ? "start-both" : "start-usb") : "stop",
    );
    const updated = await refresh();
    if (
      updated.ssh !== enabling ||
      (enabling && (!updated.usbEnabled || updated.wifiEnabled !== allowWifi()))
    ) {
      throw new Error("Supernet could not confirm the requested connections.");
    }
    return [
      enabling ? "SSH enabled" : "SSH disabled",
      connectionSummary(updated),
    ];
  }

  async function toggleWifi() {
    const current = await refresh();
    if (disposed) {
      return ["", ""];
    }
    const previous = allowWifi();
    const enabled = !previous;
    let updated = current;
    if (current.ssh) {
      checkStart(current, enabled, true);
      await command(extension, "stop");
      try {
        await command(extension, enabled ? "start-both" : "start-usb");
        updated = await refresh();
        if (
          !updated.ssh ||
          !updated.usbEnabled ||
          updated.wifiEnabled !== enabled
        ) {
          throw new Error(
            "Supernet could not confirm the requested connections.",
          );
        }
        await extension.settings.set("allowWifi", enabled);
      } catch (error) {
        // Restore the previously active connections if the new listener fails.
        const restore = current.usbEnabled
          ? current.wifiEnabled
            ? "start-both"
            : "start-usb"
          : "start-wifi";
        try {
          await command(extension, "stop");
          await command(extension, restore);
        } catch (failure) {
          extension.log(failure.message);
        }
        throw error;
      }
    } else {
      await extension.settings.set("allowWifi", enabled);
    }
    const message = current.ssh
      ? connectionSummary(updated)
      : `${enabled ? "Wi-Fi will be included" : "USB only"} when SSH is enabled.\n\n${connectionSummary(updated)}`;
    return [enabled ? "Wi-Fi SSH allowed" : "Wi-Fi SSH not allowed", message];
  }

  function WifiRow({ ToggleRow }) {
    const [, render] = extension.React.useState(0);
    extension.React.useEffect(
      () => subscribe(() => render((revision) => revision + 1)),
      [],
    );
    return extension.React.createElement(ToggleRow, {
      widgetId: "supernet-wifi-ssh",
      title: "Allow SSH connections over WiFi",
      currentState: allowWifi(),
      disabled: busy || !state,
      onPress: () => extension.execute("wifi"),
    });
  }

  const unsubscribeSettings = extension.settings.subscribe(notify);
  extension.onDispose(() => {
    disposed = true;
    clearTimeout(timer);
    unsubscribeSettings();
    listeners.clear();
  });
  extension.icons.add({ id: "terminal", path: "assets/terminal.svg" });
  extension.icons.add({ id: "terminal-on", path: "assets/terminal-on.svg" });
  extension.actions.add({ id: "ssh", run: () => runAction(toggleSsh) });
  extension.actions.add({ id: "wifi", run: () => runAction(toggleWifi) });
  extension.quickSettings.add({
    id: "ssh",
    title: "SSH",
    icon: "supernet:terminal",
    iconOn: "supernet:terminal-on",
    action: "ssh",
    value: () => !!(state && state.ssh),
    disabled: () => busy || !state,
    subscribe,
  });
  if (extension.capabilities().includes("wifiSettings")) {
    extension.wifiSettings.add({ id: "ssh", component: WifiRow });
  }
  refresh().catch((error) => extension.log(error.message));
}
