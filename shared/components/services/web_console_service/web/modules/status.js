(() => {
  "use strict";

  const STATUS_ENDPOINT = "/api/status";
  const consoleApi = window.webConsole;
  let activeContext = null;

  const adapter = Object.freeze({
    async getSnapshot(sectionId) {
      const response = await consoleApi.apiFetch(
        `${STATUS_ENDPOINT}?section=${encodeURIComponent(sectionId)}`,
      );
      if (!response.ok) throw new Error(await consoleApi.readApiError(response, "无法读取状态"));
      const payload = await response.json();
      if (!payload || !Array.isArray(payload.values) ||
          (payload.section !== undefined && payload.section !== sectionId)) {
        throw new Error("设备返回的状态数据无效。");
      }
      return payload;
    },
  });

  async function loadStatus(part, context) {
    activeContext = context;
    byId("statusFieldsRegion").classList.remove("hidden");
    const payload = await adapter.getSnapshot(context.section.id);
    if (context.view.isCurrent()) {
      context.view.renderFields(byId("statusFields"), part.fields, payload.values);
    }
    return null;
  }

  const byId = (id) => document.getElementById(id);
  const controller = Object.freeze({
    getSummary: (part) => adapter.getSnapshot(part.id),
    loadDetail: loadStatus,
    resetDetail() {
      activeContext = null;
      byId("statusFields").replaceChildren();
      byId("statusFieldsRegion").classList.add("hidden");
    },
    unmount() { this.resetDetail(); },
  });

  byId("statusRefresh").addEventListener("click", () => {
    if (activeContext) consoleApi.fields.reloadActive();
  });

  consoleApi.registerModule({
    id: "status",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("status", capability, adapter, controller);
    },
    unmount() {
      consoleApi.fields.unmountModule("status");
    },
  });
})();
