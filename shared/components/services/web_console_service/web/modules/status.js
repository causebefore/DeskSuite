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

  function currentSection() {
    return consoleApi.fields.selectedSection(capability, sectionSelect);
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
    navigation: { id: "management", label: "设备管理" },
    async mount(moduleCapability) {
      active = true;
      capability = {
        ...moduleCapability,
        sections: consoleApi.fields.normalizeSections(moduleCapability, "状态"),
      };
      consoleApi.fields.renderSectionOptions(sectionSelect, capability.sections);
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
