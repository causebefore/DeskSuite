(() => {
  "use strict";

  const SETTINGS_ENDPOINT = "/api/settings";
  const RESULT_ENDPOINT = "/api/settings/result";
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const consoleApi = window.webConsole;
  const byId = (id) => document.getElementById(id);
  const root = byId("settingsView");
  const sectionSelect = byId("settingsSection");
  const reloadButton = byId("settingsReload");
  const saveButton = byId("settingsSave");
  const message = byId("settingsMessage");
  const fieldsContainer = byId("settingsFields");
  let capability = null;
  let renderer = null;
  let version = null;
  let active = false;
  let busy = false;
  let generation = 0;
  let pollTimer = null;

  function setMessage(text, state = "error") {
    consoleApi.setNotice(message, text, state);
  }

  function currentSection() {
    return consoleApi.fields.selectedSection(capability, sectionSelect);
  }

  function cancelPolling() {
    if (pollTimer !== null) window.clearTimeout(pollTimer);
    pollTimer = null;
  }

  function updateControls() {
    sectionSelect.disabled = busy;
    reloadButton.disabled = busy;
    saveButton.disabled = busy || !renderer || !renderer.hasChanges();
    root.classList.toggle("pending", busy);
    if (renderer) renderer.setDisabled(busy);
  }

  function setBusy(nextBusy) {
    busy = nextBusy;
    updateControls();
  }

  function validateSnapshot(payload, section) {
    if (!payload || !UINT64_PATTERN.test(payload.version || "") ||
        !Array.isArray(payload.values) ||
        (payload.section !== undefined && payload.section !== section.id)) {
      throw new Error("设备返回的配置快照无效。");
    }
    return payload;
  }

  async function loadSection(successMessage = "") {
    const section = currentSection();
    if (!active || !section) return;
    const requestGeneration = ++generation;
    cancelPolling();
    setBusy(true);
    setMessage("正在读取配置…", "info");
    try {
      const response = await consoleApi.apiFetch(
        `${SETTINGS_ENDPOINT}?section=${encodeURIComponent(section.id)}`,
      );
      if (!response.ok) {
        throw new Error(await consoleApi.readApiError(response, "无法读取配置"));
      }
      const payload = validateSnapshot(await response.json(), section);
      if (!active || requestGeneration !== generation) return;
      version = payload.version;
      renderer = consoleApi.fields.render(
        fieldsContainer,
        section.fields,
        payload.values,
        {
          editable: true,
          onDirtyChange: updateControls,
        },
      );
      setMessage(successMessage, successMessage ? "success" : "info");
    } catch (error) {
      if (active && requestGeneration === generation && consoleApi.getToken()) {
        renderer = null;
        version = null;
        fieldsContainer.replaceChildren();
        setMessage(error.message);
      }
    } finally {
      if (active && requestGeneration === generation) setBusy(false);
    }
  }

  function pollDelay(payload) {
    const requested = Number(payload && payload.pollAfterMs);
    if (!Number.isFinite(requested)) return 500;
    return Math.max(250, Math.min(5000, Math.round(requested)));
  }

  function schedulePoll(sectionId, requestId, requestGeneration, payload) {
    cancelPolling();
    pollTimer = window.setTimeout(
      () => pollResult(sectionId, requestId, requestGeneration),
      pollDelay(payload),
    );
  }

  async function pollResult(sectionId, requestId, requestGeneration) {
    if (!active || requestGeneration !== generation) return;
    try {
      const response = await consoleApi.apiFetch(
        `${RESULT_ENDPOINT}?section=${encodeURIComponent(sectionId)}` +
        `&request=${encodeURIComponent(requestId)}`,
      );
      const payload = await response.json().catch(() => null);
      if (!active || requestGeneration !== generation) return;
      if (response.status === 202) {
        if (!payload || payload.state !== "pending") {
          throw new Error("设备返回的配置任务状态无效。");
        }
        schedulePoll(sectionId, requestId, requestGeneration, payload);
        return;
      }
      if (!response.ok) {
        throw new Error(
          payload && typeof payload.message === "string"
            ? payload.message
            : `无法读取配置任务结果（${response.status}）`,
        );
      }
      if (!payload || (payload.state !== "succeeded" && payload.state !== "failed")) {
        throw new Error("设备返回的配置任务结果无效。");
      }
      if (payload.state === "succeeded") {
        setBusy(false);
        await loadSection("配置已保存并重新读取。");
        return;
      }
      if (renderer) renderer.setErrors(payload.errors);
      setBusy(false);
      setMessage(
        typeof payload.message === "string" && payload.message
          ? payload.message
          : "配置应用失败。",
      );
    } catch (error) {
      if (active && requestGeneration === generation && consoleApi.getToken()) {
        setBusy(false);
        setMessage(error.message);
      }
    }
  }

  async function saveChanges(event) {
    event.preventDefault();
    const section = currentSection();
    if (!active || busy || !section || !renderer || !version) return;
    let changes;
    try {
      changes = renderer.readChanges();
    } catch (error) {
      setMessage(error.message);
      return;
    }
    if (changes.length === 0) {
      setMessage("没有需要保存的更改。", "info");
      updateControls();
      return;
    }

    const requestGeneration = ++generation;
    setBusy(true);
    setMessage("正在提交配置…", "info");
    try {
      const response = await consoleApi.apiFetch(
        `${SETTINGS_ENDPOINT}?section=${encodeURIComponent(section.id)}`,
        {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ expectedVersion: version, changes }),
        },
      );
      const payload = await response.json().catch(() => null);
      if (!active || requestGeneration !== generation) return;
      if (response.status === 409 && payload && payload.error === "version_conflict") {
        const currentVersion =
          UINT64_PATTERN.test(payload.currentVersion || "")
            ? payload.currentVersion
            : null;
        setBusy(false);
        await loadSection(
          currentVersion
            ? `配置已被其他请求更新到版本 ${currentVersion}，已重新读取。`
            : "配置已被其他请求更新，已重新读取。",
        );
        return;
      }
      if (response.status === 409) {
        throw new Error(
          payload && typeof payload.message === "string"
            ? payload.message
            : "当前状态暂不接受配置更新，请稍后重试。",
        );
      }
      if (response.status === 422) {
        if (renderer) renderer.setErrors(payload && payload.errors);
        throw new Error(
          payload && typeof payload.message === "string"
            ? payload.message
            : "配置内容校验失败。",
        );
      }
      if (response.status !== 202) {
        throw new Error(
          payload && typeof payload.message === "string"
            ? payload.message
            : `配置提交失败（${response.status}）`,
        );
      }
      if (!payload || payload.state !== "pending" ||
          !UINT64_PATTERN.test(payload.requestId || "")) {
        throw new Error("设备返回的配置任务标识无效。");
      }
      setMessage("配置已受理，正在等待应用结果…", "info");
      schedulePoll(section.id, payload.requestId, requestGeneration, payload);
    } catch (error) {
      if (active && requestGeneration === generation && consoleApi.getToken()) {
        setBusy(false);
        setMessage(error.message);
      }
    }
  }

  sectionSelect.addEventListener("change", () => loadSection());
  reloadButton.addEventListener("click", () => loadSection());
  byId("settingsForm").addEventListener("submit", saveChanges);
  saveButton.addEventListener("click", () => byId("settingsForm").requestSubmit());

  consoleApi.registerModule({
    id: "settings",
    rootId: "settingsView",
    navigation: { id: "management", label: "设备管理" },
    async mount(moduleCapability) {
      active = true;
      capability = {
        ...moduleCapability,
        sections: consoleApi.fields.normalizeSections(moduleCapability, "设置"),
      };
      consoleApi.fields.renderSectionOptions(sectionSelect, capability.sections);
      if (capability.sections.length === 0) {
        fieldsContainer.replaceChildren();
        setMessage("设备未公开可配置的分区。", "info");
        updateControls();
        return;
      }
      await loadSection();
    },
    unmount() {
      active = false;
      generation += 1;
      cancelPolling();
      busy = false;
      if (renderer) renderer.setDisabled(false);
    },
  });
})();
