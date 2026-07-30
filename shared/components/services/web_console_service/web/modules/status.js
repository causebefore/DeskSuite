(() => {
  "use strict";

  const STATUS_ENDPOINT = "/api/status";
  const consoleApi = window.webConsole;
  const byId = (id) => document.getElementById(id);
  const sectionSelect = byId("statusSection");
  const refreshButton = byId("statusRefresh");
  const message = byId("statusMessage");
  const updatedAt = byId("statusUpdatedAt");
  const fieldsContainer = byId("statusFields");
  let capability = null;
  let active = false;
  let generation = 0;

  function setMessage(text, state = "error") {
    consoleApi.setNotice(message, text, state);
  }

  function validateSections(moduleCapability) {
    if (!moduleCapability || !Array.isArray(moduleCapability.sections)) {
      throw new Error("状态模块缺少状态分区描述。");
    }
    const seen = new Set();
    for (const section of moduleCapability.sections) {
      if (!section || typeof section.id !== "string" ||
          typeof section.label !== "string" || !Array.isArray(section.fields) ||
          seen.has(section.id)) {
        throw new Error("状态模块的状态分区描述无效。");
      }
      seen.add(section.id);
    }
    return moduleCapability.sections;
  }

  function currentSection() {
    return capability.sections.find((section) => section.id === sectionSelect.value);
  }

  function renderSectionOptions(sections) {
    const previous = sectionSelect.value;
    sectionSelect.replaceChildren();
    for (const section of sections) {
      const option = document.createElement("option");
      option.value = section.id;
      option.textContent = section.label;
      sectionSelect.append(option);
    }
    if (sections.some((section) => section.id === previous)) {
      sectionSelect.value = previous;
    }
  }

  function validateSnapshot(payload, section) {
    if (!payload || !Array.isArray(payload.values) ||
        (payload.section !== undefined && payload.section !== section.id)) {
      throw new Error("设备返回的状态快照无效。");
    }
    return payload;
  }

  async function refreshStatus() {
    const section = currentSection();
    if (!active || !section) return;
    const requestGeneration = ++generation;
    sectionSelect.disabled = true;
    refreshButton.disabled = true;
    setMessage("正在读取状态…", "info");
    try {
      const response = await consoleApi.apiFetch(
        `${STATUS_ENDPOINT}?section=${encodeURIComponent(section.id)}`,
      );
      if (!response.ok) {
        throw new Error(await consoleApi.readApiError(response, "无法读取状态"));
      }
      const payload = validateSnapshot(await response.json(), section);
      if (!active || requestGeneration !== generation) return;
      consoleApi.fields.render(fieldsContainer, section.fields, payload.values);
      updatedAt.textContent = `本次读取：${new Date().toLocaleString()}`;
      setMessage("", "info");
    } catch (error) {
      if (active && requestGeneration === generation && consoleApi.getToken()) {
        fieldsContainer.replaceChildren();
        updatedAt.textContent = "";
        setMessage(error.message);
      }
    } finally {
      if (active && requestGeneration === generation) {
        sectionSelect.disabled = false;
        refreshButton.disabled = false;
      }
    }
  }

  sectionSelect.addEventListener("change", refreshStatus);
  refreshButton.addEventListener("click", refreshStatus);

  consoleApi.registerModule({
    id: "status",
    rootId: "statusView",
    async mount(moduleCapability) {
      active = true;
      capability = { ...moduleCapability, sections: validateSections(moduleCapability) };
      renderSectionOptions(capability.sections);
      if (capability.sections.length === 0) {
        fieldsContainer.replaceChildren();
        updatedAt.textContent = "";
        setMessage("设备未公开可读取的状态分区。", "info");
        return;
      }
      await refreshStatus();
    },
    unmount() {
      active = false;
      generation += 1;
    },
  });
})();
