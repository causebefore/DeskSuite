(() => {
  "use strict";

  const SETTINGS_ENDPOINT = "/api/settings";
  const RESULT_ENDPOINT = "/api/settings/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const consoleApi = window.webConsole;

  async function responseError(response, payload, fallback) {
    const error = new Error(
      payload && typeof payload.message === "string" && payload.message
        ? payload.message : `${fallback}（${response.status}）`,
    );
    error.status = response.status;
    error.payload = payload;
    return error;
  }

  function validateSnapshot(payload, sectionId) {
    if (!payload || !UINT64_PATTERN.test(payload.version || "") ||
        !Array.isArray(payload.values) ||
        (payload.section !== undefined && payload.section !== sectionId)) {
      throw new Error("设备返回的设置快照无效。");
    }
    return payload;
  }

  const adapter = Object.freeze({
    async getSnapshot(sectionId) {
      const response = await consoleApi.apiFetch(
        `${SETTINGS_ENDPOINT}?section=${encodeURIComponent(sectionId)}`,
      );
      if (!response.ok) throw new Error(await consoleApi.readApiError(response, "无法读取设置"));
      return validateSnapshot(await response.json(), sectionId);
    },
    async submit(sectionId, version, changes) {
      const response = await consoleApi.apiFetch(
        `${SETTINGS_ENDPOINT}?section=${encodeURIComponent(sectionId)}`,
        {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ expectedVersion: version, changes }),
        },
      );
      const payload = await response.json().catch(() => null);
      if (response.status === 409 && payload && payload.error === "version_conflict") {
        throw await responseError(response, payload, "设置版本冲突");
      }
      if (response.status !== 202) {
        throw await responseError(response, payload, "设置提交失败");
      }
      if (!payload || payload.state !== "pending" ||
          !UINT64_PATTERN.test(payload.requestId || "")) {
        throw new Error("设备返回的设置任务标识无效。");
      }
      return payload;
    },
    async getResult(sectionId, requestId) {
      const response = await consoleApi.apiFetch(
        `${RESULT_ENDPOINT}?section=${encodeURIComponent(sectionId)}` +
        `&request=${encodeURIComponent(requestId)}`,
      );
      const payload = await response.json().catch(() => null);
      if (!response.ok && response.status !== 202) {
        throw await responseError(response, payload, "无法查询设置结果");
      }
      if (!payload || !["pending", "succeeded", "failed"].includes(payload.state)) {
        throw new Error("设备返回的设置任务结果无效。");
      }
      return payload;
    },
  });

  consoleApi.registerModule({
    id: "settings",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("settings", capability, adapter);
    },
    unmount() {
      consoleApi.fields.unmountModule("settings");
    },
  });
})();
