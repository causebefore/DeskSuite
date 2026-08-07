(() => {
  "use strict";

  const ACTIONS_ENDPOINT = "/api/actions";
  const ACTION_RESULT_ENDPOINT = "/api/actions/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const consoleApi = window.webConsole;
  const messages = Object.freeze({
    submitting: (label) => `正在执行“${label}”…`,
    unknown: "操作结果暂时未知，请勿重复提交。",
    pending: "原操作仍在处理中，可稍后再次查询。",
    succeeded: "操作已完成。",
    failed: "操作未完成。",
    sessionExpired: "登录已失效，请重新登录后查询原操作。",
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

  const adapter = Object.freeze({
    messages,
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
