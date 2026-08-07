(() => {
  "use strict";

  const SETTINGS_ENDPOINT = "/api/settings";
  const RESULT_ENDPOINT = "/api/settings/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const consoleApi = window.webConsole;
  const messages = Object.freeze({
    conflict: "设备设置已更新；已刷新设备基线并保留本地草稿，请确认后再次保存。",
    dirty: "本地更改尚未保存。",
    succeeded: "保存成功，已重新读取设备设置。",
    deadline: "保存结果暂时未知。请继续查询原请求，不要重复提交。",
    pending: "原请求仍在处理中，可稍后再次查询。",
    failed: "设备未能应用更改。",
    sessionExpired: "登录已失效，本地草稿仍保留在当前页面内存中。",
    resultUnknown: "保存结果暂时未知，请继续查询原请求。",
    submitting: "正在提交并等待设备确认…",
    submitUnknown: "提交结果暂时未知。为避免重复写入，请先重新读取或确认原请求结果。",
  });

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
    messages,
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
