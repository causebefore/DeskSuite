(() => {
  "use strict";

  const ACTIONS_ENDPOINT = "/api/actions";
  const ACTION_RESULT_ENDPOINT = "/api/actions/result";
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

  const adapter = Object.freeze({
    async submit(sectionId, actionId, inputs) {
      const response = await consoleApi.apiFetch(
        `${ACTIONS_ENDPOINT}?section=${encodeURIComponent(sectionId)}` +
        `&action=${encodeURIComponent(actionId)}`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ inputs }),
        },
      );
      const payload = await response.json().catch(() => null);
      if (response.status !== 202) {
        throw await responseError(response, payload, "操作提交失败");
      }
      if (!payload || payload.state !== "pending" ||
          !UINT64_PATTERN.test(payload.requestId || "")) {
        throw new Error("设备返回的操作请求标识无效。");
      }
      return payload;
    },
    async getResult(sectionId, actionId, requestId) {
      const response = await consoleApi.apiFetch(
        `${ACTION_RESULT_ENDPOINT}?section=${encodeURIComponent(sectionId)}` +
        `&action=${encodeURIComponent(actionId)}` +
        `&request=${encodeURIComponent(requestId)}`,
      );
      const payload = await response.json().catch(() => null);
      if (!response.ok && response.status !== 202) {
        throw await responseError(response, payload, "无法查询操作结果");
      }
      if (!payload || !["pending", "succeeded", "failed"].includes(payload.state)) {
        throw new Error("设备返回的操作结果无效。");
      }
      return payload;
    },
  });

  consoleApi.registerModule({
    id: "actions",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("actions", capability, adapter);
    },
    unmount() {
      consoleApi.fields.unmountModule("actions");
    },
  });
})();
