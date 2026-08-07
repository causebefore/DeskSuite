(() => {
  "use strict";

  const SETTINGS_ENDPOINT = "/api/settings";
  const RESULT_ENDPOINT = "/api/settings/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const POLL_DEADLINE_MS = 30000;
  const PENDING_STORAGE_KEY = "webConsolePendingOperation";
  const PENDING_FACT_KEYS = Object.freeze(["section", "action", "request", "start"]);
  const PAGE_STATE = Object.freeze({
    dirty: "dirty",
    saving: "saving",
    succeeded: "succeeded",
    validationError: "validation_error",
    ownerBusy: "owner_busy",
    networkUnknown: "network_unknown",
    versionConflict: "version_conflict",
    sessionExpired: "session_expired",
  });
  const LEAVE_RESULT = Object.freeze({
    proceed: "proceed",
    save: "save",
    discard: "discard",
    cancel: "cancel",
  });
  const messages = Object.freeze({
    conflict: "设置已更新；刷新基线并保留草稿，请确认后保存。",
    dirty: "本地更改尚未保存。",
    succeeded: "保存成功，已重新读取设备设置。",
    deadline: "保存结果暂时未知，请查询原请求，勿重复提交。",
    pending: "原请求仍在处理中，请稍后查询。",
    failed: "设备未能应用更改。",
    sessionExpired: "登录已失效，草稿仍保留在内存中。",
    submitting: "正在提交并等待设备确认…",
    submitUnknown: "提交结果未知，请勿重复写入；请查询原请求。",
  });
  const consoleApi = window.webConsole;
  const preservedDrafts = new Map();
  const byId = (id) => document.getElementById(id);
  let activePart = null;
  let activeContext = null;
  let activeSectionId = null;
  let settingsRenderer = null;
  let baselineVersion = null;
  let pendingFact = null;
  let pollTimer = null;
  let leaveResolver = null;
  let leaveTrigger = null;

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
    if (viewReady()) activeContext.view.setPendingCheck(() => pollSettings(pendingFact, true));
  }

  function clearPendingFact() {
    pendingFact = null;
    sessionStorage.removeItem(PENDING_STORAGE_KEY);
    if (viewReady()) activeContext.view.setPendingCheck(null);
  }

  function readPendingFact() {
    try {
      const fact = JSON.parse(sessionStorage.getItem(PENDING_STORAGE_KEY) || "null");
      if (!fact || typeof fact.section !== "string" || fact.action !== "settings" ||
          !UINT64_PATTERN.test(fact.request || "") || !Number.isFinite(fact.start)) return null;
      return Object.fromEntries(PENDING_FACT_KEYS.map((key) => [key, fact[key]]));
    } catch (_) {
      return null;
    }
  }

  function viewReady() {
    return activeContext && activeContext.view && activeContext.view.isCurrent();
  }

  function reasonState(reason) {
    if (reason === "version_conflict") return PAGE_STATE.versionConflict;
    if (reason === "owner_busy") return PAGE_STATE.ownerBusy;
    if (reason === "validation_failed") return PAGE_STATE.validationError;
    return PAGE_STATE.networkUnknown;
  }

  function updateControls(state) {
    const changed = settingsRenderer ? settingsRenderer.changeCount() : 0;
    const busy = state === "loading" || state === PAGE_STATE.saving;
    const save = byId("settingsSave");
    const undo = byId("settingsUndo");
    if (save) save.disabled = busy || changed === 0;
    if (undo) undo.disabled = busy || changed === 0;
    const count = byId("settingsChangeCount");
    if (count) count.textContent = changed === 0 ? "尚无更改" : `已修改 ${changed} 项`;
    if (settingsRenderer) settingsRenderer.setDisabled(busy);
  }

  function onDraftChange() {
    if (!settingsRenderer || !viewReady()) return;
    if (settingsRenderer.hasChanges()) {
      preservedDrafts.set(activeSectionId, settingsRenderer.exportDraft());
      if (activeContext.view.getPageState() !== PAGE_STATE.saving &&
          activeContext.view.getPageState() !== PAGE_STATE.versionConflict) {
        activeContext.view.setPageState(PAGE_STATE.dirty, messages.dirty);
      } else updateControls(activeContext.view.getPageState());
    } else {
      preservedDrafts.delete(activeSectionId);
      activeContext.view.setPageState("synced", "");
    }
  }

  async function loadSettings(part, context) {
    cancelPolling();
    activePart = part;
    activeContext = context;
    activeSectionId = context.section.id;
    byId("settingsFieldsRegion").classList.remove("hidden");
    byId("settingsControls").classList.remove("hidden");
    let payload;
    try {
      payload = await adapter.getSnapshot(activeSectionId);
    } catch (error) {
      byId("settingsFields").replaceChildren();
      settingsRenderer = null;
      throw error;
    }
    if (!context.view.isCurrent()) return null;
    baselineVersion = payload.version;
    settingsRenderer = context.view.renderFields(
      byId("settingsFields"), part.fields, payload.values,
      { editable: true, onDirtyChange: onDraftChange },
    );
    const draft = context.options.draft || preservedDrafts.get(activeSectionId);
    if (draft) settingsRenderer.applyDraft(draft);
    const stored = readPendingFact();
    if (stored && stored.section === activeSectionId) {
      pendingFact = stored;
      context.view.setPendingCheck(() => pollSettings(pendingFact, true));
      Promise.resolve().then(() => pollSettings(stored));
      return { state: PAGE_STATE.saving };
    }
    if (context.options.conflict) {
      return { state: PAGE_STATE.versionConflict, message: messages.conflict };
    }
    if (settingsRenderer.hasChanges()) {
      return { state: PAGE_STATE.dirty, message: messages.dirty };
    }
    if (context.options.succeeded) {
      return {
        state: PAGE_STATE.succeeded,
        message: messages.succeeded,
        notice: "success",
      };
    }
    return null;
  }

  async function refreshBaselinePreservingDraft() {
    // reload authoritative snapshot while the local draft remains in memory
    const draft = settingsRenderer ? settingsRenderer.exportDraft() : [];
    preservedDrafts.set(activeSectionId, draft);
    const presentation = await loadSettings(activePart, {
      ...activeContext,
      options: { draft, conflict: true },
    });
    if (presentation && viewReady()) {
      activeContext.view.setPageState(presentation.state, presentation.message);
    }
  }

  async function pollSettings(fact, manual = false) {
    if (!activeSectionId || fact.section !== activeSectionId) return;
    const deadlineReached = Date.now() - fact.start >= POLL_DEADLINE_MS;
    if (deadlineReached && !manual) {
      activeContext.view.setPageState(
        PAGE_STATE.networkUnknown,
        messages.deadline,
      );
      enableDraftDialog();
      return;
    }
    try {
      const payload = await adapter.getResult(fact.section, fact.request);
      if (payload.state === "pending") {
        if (deadlineReached || manual) {
          activeContext.view.setPageState(PAGE_STATE.networkUnknown, messages.pending, "info");
          enableDraftDialog();
        } else {
          pollTimer = window.setTimeout(() => pollSettings(fact), pollDelay(payload));
        }
        return;
      }
      clearPendingFact();
      if (payload.state === "succeeded") {
        preservedDrafts.delete(activeSectionId);
        const presentation = await loadSettings(activePart, {
          ...activeContext,
          options: { succeeded: true },
        });
        if (presentation && viewReady()) {
          activeContext.view.setPageState(
            presentation.state, presentation.message, presentation.notice,
          );
        }
        if (leaveResolver) closeDraftDialog(LEAVE_RESULT.save);
        return;
      }
      const state = reasonState(payload.reason);
      if (state === PAGE_STATE.versionConflict) {
        await refreshBaselinePreservingDraft();
      } else if (viewReady()) {
        activeContext.view.setPageState(state, payload.message || messages.failed, "error");
      }
      enableDraftDialog();
    } catch (_) {
      if (viewReady()) {
        const expired = !consoleApi.getToken();
        activeContext.view.setPageState(
          expired ? PAGE_STATE.sessionExpired : PAGE_STATE.networkUnknown,
          expired ? messages.sessionExpired : messages.deadline,
          "error",
        );
      }
      enableDraftDialog();
    }
  }

  async function saveActive() {
    if (!settingsRenderer || !activeSectionId || !viewReady() ||
        activeContext.view.getPageState() === PAGE_STATE.saving) return false;
    let changes;
    try {
      changes = settingsRenderer.readChanges();
    } catch (error) {
      activeContext.view.setPageState(PAGE_STATE.validationError, error.message, "error");
      return false;
    }
    if (changes.length === 0) return true;
    activeContext.view.setPageState(PAGE_STATE.saving, messages.submitting);
    try {
      const payload = await adapter.submit(activeSectionId, baselineVersion, changes);
      const fact = {
        section: activeSectionId,
        action: "settings",
        request: payload.requestId,
        start: Date.now(),
      };
      writePendingFact(fact);
      pollSettings(fact);
      return null;
    } catch (error) {
      if (!consoleApi.getToken()) {
        activeContext.view.setPageState(PAGE_STATE.sessionExpired, messages.sessionExpired, "error");
      } else if (error.status === 422) {
        settingsRenderer.setErrors(error.payload && error.payload.errors);
        activeContext.view.setPageState(PAGE_STATE.validationError, error.message, "error");
      } else if (error.status === 409 && error.payload &&
                 error.payload.error === "version_conflict") {
        await refreshBaselinePreservingDraft();
      } else if (error.status === 409) {
        activeContext.view.setPageState(PAGE_STATE.ownerBusy, error.message, "error");
      } else {
        activeContext.view.setPageState(PAGE_STATE.networkUnknown, messages.submitUnknown, "error");
      }
      return false;
    }
  }

  function discardActiveDraft() {
    if (activeSectionId) preservedDrafts.delete(activeSectionId);
    settingsRenderer = null;
  }

  function handleLeaveResult(result) {
    if (result === LEAVE_RESULT.discard) discardActiveDraft();
    return result !== LEAVE_RESULT.cancel;
  }

  function focusableElements() {
    return [...byId("draftDialog").querySelectorAll("button:not(:disabled)")];
  }

  function enableDraftDialog() {
    for (const button of byId("draftDialog").querySelectorAll("button")) button.disabled = false;
  }

  function closeDraftDialog(result) {
    const resolver = leaveResolver;
    const trigger = leaveTrigger;
    leaveResolver = null;
    leaveTrigger = null;
    byId("draftDialog").classList.add("hidden");
    if (trigger && typeof trigger.focus === "function") trigger.focus();
    if (resolver) resolver(result);
  }

  function confirmLeave(trigger) {
    if (!settingsRenderer || !settingsRenderer.hasChanges()) {
      return Promise.resolve(LEAVE_RESULT.proceed);
    }
    if (leaveResolver) return Promise.resolve(LEAVE_RESULT.cancel);
    leaveTrigger = trigger || document.activeElement;
    byId("draftDialog").classList.remove("hidden");
    byId("draftSaveLeave").focus();
    return new Promise((resolve) => { leaveResolver = resolve; });
  }

  function resetDetail() {
    cancelPolling();
    if (activeContext && activeContext.view) activeContext.view.setPendingCheck(null);
    settingsRenderer = null;
    baselineVersion = null;
    activePart = null;
    activeContext = null;
    activeSectionId = null;
    byId("settingsFields").replaceChildren();
    byId("settingsFieldsRegion").classList.add("hidden");
    byId("settingsControls").classList.add("hidden");
    if (leaveResolver) closeDraftDialog(LEAVE_RESULT.cancel);
  }

  const controller = Object.freeze({
    getSummary: (part) => adapter.getSnapshot(part.id),
    loadDetail: loadSettings,
    updateControls,
    confirmLeave,
    handleLeaveResult,
    hasUnsaved: () => Boolean(settingsRenderer && settingsRenderer.hasChanges()),
    resetDetail,
    unmount() {
      if (settingsRenderer && settingsRenderer.hasChanges() && activeSectionId) {
        preservedDrafts.set(activeSectionId, settingsRenderer.exportDraft());
      }
      resetDetail();
    },
  });

  byId("settingsForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    await saveActive();
  });
  byId("settingsUndo").addEventListener("click", () => {
    if (activeSectionId) preservedDrafts.delete(activeSectionId);
    if (viewReady()) activeContext.view.reload();
  });
  byId("draftSaveLeave").addEventListener("click", async () => {
    for (const button of focusableElements()) button.disabled = true;
    const result = await saveActive();
    if (result === true) closeDraftDialog(LEAVE_RESULT.save);
    else if (result === false) enableDraftDialog();
  });
  byId("draftDiscardLeave").addEventListener(
    "click", () => closeDraftDialog(LEAVE_RESULT.discard),
  );
  byId("draftCancelLeave").addEventListener(
    "click", () => closeDraftDialog(LEAVE_RESULT.cancel),
  );
  byId("draftDialog").addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeDraftDialog(LEAVE_RESULT.cancel);
      return;
    }
    if (event.key !== "Tab") return;
    const items = focusableElements();
    if (items.length === 0) return;
    const current = items.indexOf(document.activeElement);
    const next = event.shiftKey
      ? (current <= 0 ? items.length - 1 : current - 1)
      : (current >= items.length - 1 ? 0 : current + 1);
    event.preventDefault();
    items[next].focus();
  });

  consoleApi.registerModule({
    id: "settings",
    rootId: "settingsCenter",
    async mount(capability) {
      consoleApi.fields.mountModule("settings", capability, adapter, controller);
    },
    unmount() {
      consoleApi.fields.unmountModule("settings");
    },
  });
})();
