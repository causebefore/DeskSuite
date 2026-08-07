(() => {
  "use strict";

  const STATUS_ENDPOINT = "/api/status";
  const consoleApi = window.webConsole;

  const adapter = Object.freeze({
    async getSnapshot(sectionId) {
      const response = await consoleApi.apiFetch(
        `${STATUS_ENDPOINT}?section=${encodeURIComponent(sectionId)}`,
      );
      if (!response.ok) throw new Error(await consoleApi.readApiError(response, "无法读取状态"));
      const payload = await response.json();
      if (!payload || !Array.isArray(payload.values) ||
          (payload.section !== undefined && payload.section !== sectionId)) {
        throw new Error("设备返回的状态快照无效。");
      }
      return payload;
    },
  });

  consoleApi.registerModule({
    id: "status",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("status", capability, adapter);
    },
    unmount() {
      consoleApi.fields.unmountModule("status");
    },
  });
})();
