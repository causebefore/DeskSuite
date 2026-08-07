(() => {
  "use strict";

  const ACTIONS_ENDPOINT = "/api/actions";
  const ACTION_RESULT_ENDPOINT = "/api/actions/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const POLL_DEADLINE_MS = 30000;
  const PENDING_STORAGE_KEY = "webConsolePendingOperation";
  const PENDING_FACT_KEYS = Object.freeze(["section", "action", "request", "start"]);
  const PAGE_STATE = Object.freeze({
    saving: "saving",
    succeeded: "succeeded",
    validationError: "validation_error",
    ownerBusy: "owner_busy",
    networkUnknown: "network_unknown",
    sessionExpired: "session_expired",
  });
  const messages = Object.freeze({
    submitting: (label) => `正在执行“${label}”…`,
    unknown: "操作结果暂时未知，请勿重复提交。",
    pending: "原操作仍在处理中，请稍后查询。",
    succeeded: "操作已完成。",
    failed: "操作未完成。",
    sessionExpired: "登录已失效，请重新登录后查询。",
  });
  const consoleApi = window.webConsole;
  const preservedActionInputs = new Map();
  const byId = (id) => document.getElementById(id);
  let activeContext = null;
  let activeSectionId = null;
  let pendingFact = null;
  let pollTimer = null;

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

  function actionInputKey(sectionId, actionId) {
    return `${sectionId}\u0000${actionId}`;
  }

  function viewReady() {
    return activeContext && activeContext.view && activeContext.view.isCurrent();
  }

  function cancelPolling() {
    if (pollTimer !== null) window.clearTimeout(pollTimer);
    pollTimer = null;
  }

  function pollDelay(payload) {
    const requested = Number(payload && payload.pollAfterMs);
    if (!Number.isFinite(requested)) return 600;
    return Math.max(250, Math.min(3000, Math.round(requested)));
  }

  function writePendingFact(fact) {
    pendingFact = Object.fromEntries(PENDING_FACT_KEYS.map((key) => [key, fact[key]]));
    sessionStorage.setItem(PENDING_STORAGE_KEY, JSON.stringify(pendingFact));
    if (viewReady()) activeContext.view.setPendingCheck(() => pollAction(pendingFact, true));
  }

  function clearPendingFact() {
    pendingFact = null;
    sessionStorage.removeItem(PENDING_STORAGE_KEY);
    if (viewReady()) activeContext.view.setPendingCheck(null);
  }

  function readPendingFact() {
    try {
      const fact = JSON.parse(sessionStorage.getItem(PENDING_STORAGE_KEY) || "null");
      if (!fact || typeof fact.section !== "string" || fact.action === "settings" ||
          typeof fact.action !== "string" || !UINT64_PATTERN.test(fact.request || "") ||
          !Number.isFinite(fact.start)) return null;
      return Object.fromEntries(PENDING_FACT_KEYS.map((key) => [key, fact[key]]));
    } catch (_) {
      return null;
    }
  }

  function reasonState(reason) {
    if (reason === "owner_busy") return PAGE_STATE.ownerBusy;
    if (reason === "validation_failed") return PAGE_STATE.validationError;
    return PAGE_STATE.networkUnknown;
  }

  function renderAction(section, action) {
    const card = document.createElement("section");
    card.className = "action-entry";
    const open = document.createElement("button");
    open.type = "button";
    open.className = "action-open";
    open.textContent = action.label;
    const panel = document.createElement("div");
    panel.className = "action-panel hidden";
    if (action.description) {
      const description = document.createElement("p");
      description.textContent = action.description;
      panel.append(description);
    }
    const fields = document.createElement("div");
    fields.className = "field-list";
    const key = actionInputKey(section.id, action.id);
    const renderer = activeContext.view.renderFields(
      fields, action.inputs, preservedActionInputs.get(key) || [], { editable: true },
    );
    const run = document.createElement("button");
    run.type = "button";
    run.textContent = action.label;
    run.addEventListener("click", () => submitAction(section, action, renderer, run));
    open.addEventListener("click", () => {
      const opening = panel.classList.contains("hidden");
      panel.classList.toggle("hidden", !opening);
      open.setAttribute("aria-expanded", String(opening));
      if (opening) {
        const first = panel.querySelector("input, select, button");
        if (first) first.focus();
      }
    });
    open.setAttribute("aria-expanded", "false");
    panel.append(fields, run);
    card.append(open, panel);
    return card;
  }

  function renderActions(part) {
    const list = byId("actionsList");
    list.replaceChildren();
    byId("actionsRegion").classList.toggle("hidden", part.actions.length === 0);
    for (const action of part.actions) list.append(renderAction(part, action));
  }

  function actionFailureState(error, hasToken) {
    if (!hasToken) return PAGE_STATE.sessionExpired;
    if (error.status === 422) return PAGE_STATE.validationError;
    if (error.status === 409) return PAGE_STATE.ownerBusy;
    return PAGE_STATE.networkUnknown;
  }

  async function submitAction(section, action, renderer, button) {
    if (!viewReady() || activeContext.view.getPageState() === PAGE_STATE.saving) return;
    let inputs;
    try {
      inputs = renderer.readAllValues();
    } catch (error) {
      activeContext.view.setPageState(PAGE_STATE.validationError, error.message, "error");
      return;
    }
    const inputKey = actionInputKey(section.id, action.id);
    preservedActionInputs.set(inputKey, inputs);
    button.disabled = true;
    activeContext.view.setPageState(PAGE_STATE.saving, messages.submitting(action.label));
    try {
      const payload = await adapter.submit(section.id, action.id, inputs);
      const fact = {
        section: section.id,
        action: action.id,
        request: payload.requestId,
        start: Date.now(),
      };
      writePendingFact(fact);
      pollAction(fact);
    } catch (error) {
      button.disabled = false;
      const state = actionFailureState(error, Boolean(consoleApi.getToken()));
      if (state === PAGE_STATE.validationError) {
        renderer.setErrors(error.payload && error.payload.errors);
      }
      const message = state === PAGE_STATE.sessionExpired
        ? messages.sessionExpired
        : (state === PAGE_STATE.networkUnknown ? messages.unknown : error.message);
      activeContext.view.setPageState(state, message, "error");
    }
  }

  async function pollAction(fact, manual = false) {
    if (!activeSectionId || fact.section !== activeSectionId) return;
    const deadlineReached = Date.now() - fact.start >= POLL_DEADLINE_MS;
    if (deadlineReached && !manual) {
      activeContext.view.setPageState(PAGE_STATE.networkUnknown, messages.unknown);
      return;
    }
    try {
      const payload = await adapter.getResult(fact.section, fact.action, fact.request);
      if (payload.state === "pending") {
        if (deadlineReached || manual) {
          activeContext.view.setPageState(PAGE_STATE.networkUnknown, messages.pending, "info");
        } else {
          pollTimer = window.setTimeout(() => pollAction(fact), pollDelay(payload));
        }
        return;
      }
      clearPendingFact();
      if (payload.state === "succeeded") {
        preservedActionInputs.delete(actionInputKey(fact.section, fact.action));
        activeContext.view.setPageState(PAGE_STATE.succeeded, messages.succeeded, "success");
      } else {
        activeContext.view.setPageState(
          reasonState(payload.reason), payload.message || messages.failed, "error",
        );
      }
    } catch (_) {
      const expired = !consoleApi.getToken();
      activeContext.view.setPageState(
        expired ? PAGE_STATE.sessionExpired : PAGE_STATE.networkUnknown,
        expired ? messages.sessionExpired : messages.unknown,
        "error",
      );
    }
  }

  function updateControls(state) {
    const busy = state === PAGE_STATE.saving;
    byId("actionsList").querySelectorAll(".action-panel > button").forEach((button) => {
      button.disabled = busy;
    });
  }

  function loadActions(part, context) {
    cancelPolling();
    activeContext = context;
    activeSectionId = context.section.id;
    byId("actionsRegion").classList.remove("hidden");
    renderActions(part);
    const stored = readPendingFact();
    if (stored && stored.section === activeSectionId) {
      pendingFact = stored;
      context.view.setPendingCheck(() => pollAction(pendingFact, true));
      Promise.resolve().then(() => pollAction(stored));
      return { state: PAGE_STATE.saving };
    }
    return null;
  }

  function resetDetail() {
    cancelPolling();
    if (activeContext && activeContext.view) activeContext.view.setPendingCheck(null);
    activeContext = null;
    activeSectionId = null;
    byId("actionsList").replaceChildren();
    byId("actionsRegion").classList.add("hidden");
  }

  const controller = Object.freeze({
    loadDetail: loadActions,
    updateControls,
    resetDetail,
    unmount: resetDetail,
  });

  consoleApi.registerModule({
    id: "actions",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("actions", capability, adapter, controller);
    },
    unmount() {
      consoleApi.fields.unmountModule("actions");
    },
  });
})();
