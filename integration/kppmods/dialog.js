export function createDialog(extension) {
  let launcher = null;
  let sequence = 0;
  let activeId = null;

  extension.onDispose(() => {
    if (activeId) {
      launcher
        .hide(activeId, "cancel")
        .catch((error) => extension.log(error.message));
    }
  });

  return (title, message) =>
    new Promise((resolve, reject) => {
      // Resolve dialog components only after the stock application has started.
      launcher = extension.module("dialogLauncher").default;
      const SimpleModal = extension.module("simpleModal").default;
      const dialogId = `${extension.id}-ssh-${++sequence}`;
      activeId = dialogId;
      launcher
        .show({
          dialogId,
          testID: dialogId,
          lightbox: "on",
          accessibilityWindowName: title,
          onHideDialog() {
            activeId = null;
            resolve();
          },
          Content: () =>
            extension.React.createElement(SimpleModal, {
              dialogId,
              testID: dialogId,
              header: title,
              body: message,
              bodyScrollable: false,
              rightButton: {
                id: "ok",
                type: "primary",
                text: "OK",
                callback() {},
              },
            }),
        })
        .catch((error) => {
          activeId = null;
          reject(error);
        });
    });
}
