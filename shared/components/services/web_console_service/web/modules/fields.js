(() => {
  "use strict";

  const PAGE_STATE = Object.freeze({
    loading: "loading",
    synced: "synced",
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
  const INTEGER_LIMITS = {
    int32: { min: -2147483648, max: 2147483647 },
    uint32: { min: 0, max: 4294967295 },
  };
  const UINT64_PATTERN = /^(0|[1-9]\d{0,19})$/;
  const POLL_DEADLINE_MS = 30000;
  const PENDING_STORAGE_KEY = "webConsolePendingOperation";
  const PENDING_FACT_KEYS = Object.freeze(["section", "action", "request", "start"]);
  const consoleApi = window.webConsole;
  const providers = new Map();
  const preservedDrafts = new Map();
  const preservedActionInputs = new Map();
  const byId = (id) => document.getElementById(id);
  const hasOwn = (object, key) =>
    object !== null && object !== undefined &&
    Object.prototype.hasOwnProperty.call(object, key);
  let sections = [];
  let activeSection = null;
  let settingsRenderer = null;
  let baselineVersion = null;
  let pageState = PAGE_STATE.synced;
  let generation = 0;
  let pollTimer = null;
  let pendingFact = null;
  let leaveResolver = null;
  let leaveTrigger = null;
  let fieldSequence = 0;
  let descriptionSequence = 0;

  function moduleMessage(sectionPart, key, ...args) {
    const message = sectionPart && sectionPart.adapter &&
      sectionPart.adapter.messages && sectionPart.adapter.messages[key];
    if (typeof message === "function") return message(...args);
    return typeof message === "string" ? message : "";
  }

  function actionInputKey(sectionId, actionId) {
    return `${sectionId}\u0000${actionId}`;
  }

  function optionParts(option) {
    if (option && typeof option === "object") {
      return {
        value: String(option.value ?? ""),
        jsonValue: option.value,
        label: String(option.label ?? option.value ?? ""),
      };
    }
    return { value: String(option), jsonValue: option, label: String(option) };
  }

  function valueEntries(values) {
    const entries = new Map();
    for (const entry of Array.isArray(values) ? values : []) {
      if (entry && typeof entry.id === "string" && !entries.has(entry.id)) {
        entries.set(entry.id, entry);
      }
    }
    return entries;
  }

  function formatDisplayValue(field, entry) {
    if (!hasOwn(entry, "value")) return "—";
    const value = entry.value;
    let text;
    if (field.type === "bool") {
      text = value === true ? "已启用" : "未启用";
    } else if (field.type === "enum" && Array.isArray(field.options)) {
      const match = field.options.map(optionParts).find(
        (option) => option.value === String(value),
      );
      text = match ? match.label : String(value);
    } else if (field.format === "duration_seconds") {
      const minutes = Math.max(0, Math.floor(Number(value) / 60));
      text = minutes < 60
        ? `${minutes} 分钟`
        : `${Math.floor(minutes / 60)} 小时 ${minutes % 60} 分`;
    } else if (field.format === "date_time") {
      const date = new Date(value);
      text = Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString();
    } else {
      text = value === null || value === undefined ? "—" : String(value);
    }
    return typeof field.unit === "string" && field.unit && text !== "—"
      ? `${text} ${field.unit}` : text;
  }

  function validateFields(fields, context) {
    if (!Array.isArray(fields)) throw new Error(`${context}缺少字段描述。`);
    const seen = new Set();
    for (const field of fields) {
      if (!field || typeof field.id !== "string" ||
          typeof field.label !== "string" || seen.has(field.id)) {
        throw new Error(`${context}字段描述无效。`);
      }
      seen.add(field.id);
    }
    return fields;
  }

  function normalizeSections(moduleCapability, kind) {
    if (!moduleCapability || !Array.isArray(moduleCapability.sections)) {
      throw new Error("设置中心模块缺少类别描述。");
    }
    const seen = new Set();
    for (const section of moduleCapability.sections) {
      if (!section || typeof section.id !== "string" ||
          typeof section.label !== "string" || seen.has(section.id)) {
        throw new Error("设置中心类别描述无效。");
      }
      seen.add(section.id);
      if (kind === "actions") {
        if (!Array.isArray(section.actions)) throw new Error("输入类别描述无效。");
        const actionIds = new Set();
        for (const action of section.actions) {
          if (!action || typeof action.id !== "string" ||
              typeof action.label !== "string" || actionIds.has(action.id)) {
            throw new Error("输入描述无效。");
          }
          actionIds.add(action.id);
          validateFields(action.inputs, "输入");
        }
      } else {
        validateFields(section.fields, "设置中心类别");
      }
    }
    return moduleCapability.sections;
  }

  function mergeSectionCapabilities() {
    const merged = new Map();
    for (const [kind, record] of providers) {
      for (const source of normalizeSections(record.capability, kind)) {
        let section = merged.get(source.id);
        if (!section) {
          section = {
            id: source.id,
            label: source.label,
            description: source.description || "",
            settings: null,
            status: null,
            actions: null,
          };
          merged.set(source.id, section);
        }
        if (!section.description && source.description) section.description = source.description;
        section[kind] = { ...source, adapter: record.adapter };
      }
    }
    return [...merged.values()];
  }

  function rawInputValue(field, input) {
    return field.type === "bool" ? input.checked : input.value;
  }

  function setRawInputValue(field, input, value) {
    if (field.type === "bool") input.checked = value === true;
    else input.value = String(value ?? "");
  }

  function validateInput(field, input) {
    const raw = rawInputValue(field, input);
    if (field.type === "bool") return raw;
    if (field.type === "int32" || field.type === "uint32") {
      const pattern = field.type === "uint32" ? /^(0|[1-9]\d*)$/ : /^-?(0|[1-9]\d*)$/;
      if (!pattern.test(raw)) throw new Error("请输入有效整数。");
      const value = Number(raw);
      const limits = INTEGER_LIMITS[field.type];
      const minimum = Number.isInteger(field.min) ? field.min : limits.min;
      const maximum = Number.isInteger(field.max) ? field.max : limits.max;
      if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
        throw new Error(`请输入 ${minimum} 到 ${maximum} 之间的整数。`);
      }
      if (Number.isInteger(field.step) && field.step > 0 &&
          (value - minimum) % field.step !== 0) {
        throw new Error(`数值步进必须为 ${field.step}。`);
      }
      return value;
    }
    if (field.type === "enum") {
      const selected = (Array.isArray(field.options) ? field.options : [])
        .map(optionParts).find((option) => option.value === raw);
      if (!selected) throw new Error("请选择有效选项。");
      return selected.jsonValue;
    }
    if (field.type === "string") {
      if (Number.isInteger(field.maxBytes) &&
          new TextEncoder().encode(raw).length > field.maxBytes) {
        throw new Error(`内容不能超过 ${field.maxBytes} 字节。`);
      }
      return raw;
    }
    throw new Error(`不支持的字段类型：${field.type}`);
  }

  function createRegularInput(field, entry) {
    const input = document.createElement("input");
    if (field.type === "bool") {
      input.type = "checkbox";
      input.checked = hasOwn(entry, "value") && entry.value === true;
    } else if (field.type === "enum") {
      const select = document.createElement("select");
      for (const rawOption of Array.isArray(field.options) ? field.options : []) {
        const parts = optionParts(rawOption);
        const option = document.createElement("option");
        option.value = parts.value;
        option.textContent = parts.label;
        select.append(option);
      }
      if (hasOwn(entry, "value")) select.value = String(entry.value);
      return select;
    } else if (field.type === "int32" || field.type === "uint32") {
      input.type = "number";
      if (Number.isInteger(field.min)) input.min = String(field.min);
      if (Number.isInteger(field.max)) input.max = String(field.max);
      input.step = String(Number.isInteger(field.step) && field.step > 0 ? field.step : 1);
      if (hasOwn(entry, "value")) input.value = String(entry.value);
    } else {
      input.type = "text";
      if (Number.isInteger(field.maxBytes)) input.maxLength = Math.max(0, field.maxBytes);
      if (hasOwn(entry, "value")) input.value = String(entry.value);
    }
    return input;
  }

  function createNumberStepper(field, entry) {
    const root = document.createElement("div");
    root.className = "number-stepper";
    const minus = document.createElement("button");
    minus.type = "button";
    minus.textContent = "−";
    const input = createRegularInput(field, entry);
    const plus = document.createElement("button");
    plus.type = "button";
    plus.textContent = "+";
    const adjust = (direction) => {
      const step = Number.isInteger(field.step) && field.step > 0 ? field.step : 1;
      const fallback = Number.isInteger(field.min) ? field.min : 0;
      let value = Number.isFinite(Number(input.value)) ? Number(input.value) : fallback;
      value += direction * step;
      if (Number.isInteger(field.min)) value = Math.max(field.min, value);
      if (Number.isInteger(field.max)) value = Math.min(field.max, value);
      input.value = String(value);
      input.dispatchEvent(new Event("input", { bubbles: true }));
    };
    minus.addEventListener("click", () => adjust(-1));
    plus.addEventListener("click", () => adjust(1));
    root.append(minus, input, plus);
    return {
      root,
      input,
      setDisabled(disabled) {
        minus.disabled = disabled;
        input.disabled = disabled;
        plus.disabled = disabled;
      },
    };
  }

  function appendFieldHeading(row, field, input) {
    const heading = document.createElement("div");
    heading.className = "field-heading";
    const label = document.createElement(input ? "label" : "span");
    label.className = "field-label";
    label.textContent = field.label;
    if (input) label.htmlFor = input.id;
    heading.append(label);
    if (typeof field.description === "string" && field.description) {
      const description = document.createElement("p");
      description.id = `console-field-description-${++descriptionSequence}`;
      description.className = "field-description";
      description.textContent = field.description;
      heading.append(description);
      if (input) input.setAttribute("aria-describedby", description.id);
    }
    if (typeof field.effect === "string" && field.effect && field.effect !== "none") {
      const effect = document.createElement("span");
      effect.className = "field-effect";
      effect.textContent = `生效方式：${field.effect}`;
      heading.append(effect);
    }
    row.prepend(heading);
  }

  function renderFields(container, fields, values, options = {}) {
    const editable = options.editable === true;
    const entries = valueEntries(values);
    const controls = new Map();
    container.replaceChildren();
    validateFields(fields, "页面");

    for (const field of fields) {
      const entry = entries.get(field.id);
      const row = document.createElement("section");
      row.className = "field-row";
      row.dataset.fieldId = field.id;
      const control = document.createElement("div");
      control.className = "field-control";
      const error = document.createElement("p");
      error.className = "field-error";
      error.setAttribute("role", "alert");

      if (field.secret === true) {
        appendFieldHeading(row, field, null);
        const badge = document.createElement("span");
        badge.className = "field-badge";
        badge.textContent = entry && entry.configured === true ? "已配置" : "未配置";
        control.append(badge);
      } else if (editable && field.writable === true) {
        let editor;
        if ((field.type === "int32" || field.type === "uint32")) {
          editor = createNumberStepper(field, entry);
        } else if (field.type === "string" && typeof field.fileSuffix === "string" &&
                   consoleApi.files && typeof consoleApi.files.createFieldPicker === "function") {
          editor = consoleApi.files.createFieldPicker(field, entry);
        } else {
          const regularInput = createRegularInput(field, entry);
          if (field.type === "string" && typeof field.fileSuffix === "string") {
            regularInput.readOnly = true;
          }
          editor = {
            root: regularInput,
            input: regularInput,
            setDisabled(disabled) { regularInput.disabled = disabled; },
          };
        }
        const input = editor.input;
        input.id = `console-field-${++fieldSequence}`;
        appendFieldHeading(row, field, input);
        control.append(editor.root);
        const state = {
          field,
          editor,
          input,
          error,
          initialRaw: rawInputValue(field, input),
          touched: false,
        };
        const markTouched = () => {
          state.touched = rawInputValue(field, input) !== state.initialRaw;
          error.textContent = "";
          if (typeof options.onDirtyChange === "function") options.onDirtyChange();
        };
        input.addEventListener(field.type === "bool" || field.type === "enum" ? "change" : "input", markTouched);
        controls.set(field.id, state);
      } else {
        appendFieldHeading(row, field, null);
        const output = document.createElement("output");
        output.textContent = field.readable === false ? "不可读取" : formatDisplayValue(field, entry);
        control.append(output);
      }
      row.append(control, error);
      container.append(row);
    }

    function readValues(changedOnly) {
      const result = [];
      let firstError = null;
      for (const [id, state] of controls) {
        state.error.textContent = "";
        if (changedOnly && !state.touched) continue;
        try {
          result.push({ id, value: validateInput(state.field, state.input) });
        } catch (error) {
          state.error.textContent = error.message;
          if (!firstError) firstError = error;
        }
      }
      if (firstError) throw firstError;
      return result;
    }

    return {
      hasChanges: () => [...controls.values()].some((state) => state.touched),
      changeCount: () => [...controls.values()].filter((state) => state.touched).length,
      readChanges: () => readValues(true),
      readAllValues: () => readValues(false),
      exportDraft() {
        return [...controls.entries()]
          .filter(([, state]) => state.touched)
          .map(([id, state]) => ({ id, value: rawInputValue(state.field, state.input) }));
      },
      applyDraft(draft) {
        for (const item of Array.isArray(draft) ? draft : []) {
          const state = controls.get(item.id);
          if (!state) continue;
          setRawInputValue(state.field, state.input, item.value);
          state.touched = rawInputValue(state.field, state.input) !== state.initialRaw;
        }
        if (typeof options.onDirtyChange === "function") options.onDirtyChange();
      },
      setErrors(errors) {
        for (const state of controls.values()) state.error.textContent = "";
        for (const item of Array.isArray(errors) ? errors : []) {
          const state = item && controls.get(item.id);
          if (state && typeof item.message === "string") state.error.textContent = item.message;
        }
      },
      setDisabled(disabled) {
        for (const state of controls.values()) state.editor.setDisabled(disabled);
      },
    };
  }

  function setLiveMessage(text, state = "info") {
    consoleApi.setNotice(byId("settingsLiveMessage"), text, state);
  }

  function updateSettingsControls() {
    const changed = settingsRenderer ? settingsRenderer.changeCount() : 0;
    const busy = pageState === PAGE_STATE.loading || pageState === PAGE_STATE.saving;
    const save = byId("settingsSave");
    const undo = byId("settingsUndo");
    const check = byId("pendingResultCheck");
    if (save) save.disabled = busy || changed === 0;
    if (undo) undo.disabled = busy || changed === 0;
    if (check) check.classList.toggle("hidden", pageState !== PAGE_STATE.networkUnknown || !pendingFact);
    const count = byId("settingsChangeCount");
    if (count) count.textContent = changed === 0 ? "尚无更改" : `已修改 ${changed} 项`;
    if (settingsRenderer) settingsRenderer.setDisabled(busy);
  }

  function setPageState(nextState, message = "", noticeState = "info") {
    pageState = nextState;
    byId("settingsDetail").dataset.state = nextState;
    setLiveMessage(message, noticeState);
    if (nextState !== PAGE_STATE.saving) {
      document.querySelectorAll("#actionsList .action-panel > button").forEach((button) => {
        button.disabled = false;
      });
    }
    updateSettingsControls();
  }

  function cancelPolling() {
    if (pollTimer !== null) window.clearTimeout(pollTimer);
    pollTimer = null;
  }

  function writePendingFact(fact) {
    const clean = Object.fromEntries(PENDING_FACT_KEYS.map((key) => [key, fact[key]]));
    pendingFact = clean;
    sessionStorage.setItem(PENDING_STORAGE_KEY, JSON.stringify(clean));
  }

  function clearPendingFact() {
    pendingFact = null;
    sessionStorage.removeItem(PENDING_STORAGE_KEY);
  }

  function readPendingFact() {
    try {
      const fact = JSON.parse(sessionStorage.getItem(PENDING_STORAGE_KEY) || "null");
      if (!fact || typeof fact.section !== "string" || typeof fact.action !== "string" ||
          !UINT64_PATTERN.test(fact.request || "") || !Number.isFinite(fact.start)) return null;
      return Object.fromEntries(PENDING_FACT_KEYS.map((key) => [key, fact[key]]));
    } catch (_) {
      return null;
    }
  }

  function pollDelay(payload) {
    const requested = Number(payload && payload.pollAfterMs);
    if (!Number.isFinite(requested)) return 600;
    return Math.max(250, Math.min(3000, Math.round(requested)));
  }

  function summaryFields(sectionPart, payload) {
    const entries = valueEntries(payload.values);
    return sectionPart.fields
      .filter((field) => typeof field.summary === "string" && field.summary)
      .map((field) => {
        const value = formatDisplayValue(field, entries.get(field.id));
        return field.summary === field.label ? value : `${field.summary} ${value}`;
      });
  }

  async function renderSectionSummary(section, target) {
    const requests = [];
    if (section.settings) {
      requests.push(section.settings.adapter.getSnapshot(section.id).then(
        (payload) => summaryFields(section.settings, payload),
      ));
    }
    if (section.status) {
      requests.push(section.status.adapter.getSnapshot(section.id).then(
        (payload) => summaryFields(section.status, payload),
      ));
    }
    if (requests.length === 0) {
      target.textContent = section.description || "查看详情";
      return;
    }
    const results = await Promise.allSettled(requests);
    const values = results
      .filter((result) => result.status === "fulfilled")
      .flatMap((result) => result.value);
    if (values.length > 0) target.textContent = values.join(" · ");
    else if (results.every((result) => result.status === "rejected")) {
      target.textContent = "暂时无法读取";
    } else target.textContent = section.description || "查看详情";
  }

  function renderHome() {
    const list = byId("settingsSectionList");
    list.replaceChildren();
    byId("settingsHomeEmpty").classList.toggle("hidden", sections.length !== 0);
    for (const section of sections) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "settings-section-link";
      const copy = document.createElement("span");
      const title = document.createElement("strong");
      title.textContent = section.label;
      const summary = document.createElement("span");
      summary.className = "settings-section-summary";
      summary.textContent = "正在读取…";
      const arrow = document.createElement("span");
      arrow.className = "settings-section-arrow";
      arrow.textContent = "›";
      arrow.setAttribute("aria-hidden", "true");
      copy.append(title, summary);
      button.append(copy, arrow);
      button.addEventListener("click", () => openSection(section, true));
      list.append(button);
      renderSectionSummary(section, summary).catch(() => {
        summary.textContent = "暂时无法读取";
      });
    }
  }

  function showHome() {
    cancelPolling();
    if (settingsRenderer && settingsRenderer.hasChanges() && activeSection) {
      preservedDrafts.set(activeSection.id, settingsRenderer.exportDraft());
    }
    activeSection = null;
    settingsRenderer = null;
    baselineVersion = null;
    byId("settingsDetail").classList.add("hidden");
    byId("settingsHome").classList.remove("hidden");
    renderHome();
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
    const renderer = renderFields(
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

  function renderActions(section) {
    const region = byId("actionsRegion");
    const list = byId("actionsList");
    if (!region || !list) return;
    list.replaceChildren();
    region.classList.toggle("hidden", !section.actions || section.actions.actions.length === 0);
    if (!section.actions) return;
    for (const action of section.actions.actions) list.append(renderAction(section, action));
  }

  async function loadDetail(section, options = {}) {
    const currentGeneration = ++generation;
    cancelPolling();
    activeSection = section;
    byId("settingsHome").classList.add("hidden");
    byId("settingsDetail").classList.remove("hidden");
    byId("settingsDetailTitle").textContent = section.label;
    byId("settingsDetailDescription").textContent = section.description;
    const settingsRegion = byId("settingsFieldsRegion");
    const statusRegion = byId("statusFieldsRegion");
    if (settingsRegion) settingsRegion.classList.toggle("hidden", !section.settings);
    if (statusRegion) statusRegion.classList.toggle("hidden", !section.status);
    setPageState(PAGE_STATE.loading, "正在读取设备数据…");
    const tasks = [];
    if (section.settings) tasks.push(section.settings.adapter.getSnapshot(section.id));
    if (section.status) tasks.push(section.status.adapter.getSnapshot(section.id));
    const results = await Promise.allSettled(tasks);
    if (currentGeneration !== generation || activeSection !== section) return;
    let resultIndex = 0;
    let failed = false;
    if (section.settings) {
      const result = results[resultIndex++];
      if (result.status === "fulfilled") {
        baselineVersion = result.value.version;
        settingsRenderer = renderFields(
          byId("settingsFields"), section.settings.fields, result.value.values,
          { editable: true, onDirtyChange: onDraftChange },
        );
        const draft = options.draft || preservedDrafts.get(section.id);
        if (draft) settingsRenderer.applyDraft(draft);
      } else {
        failed = true;
        settingsRenderer = null;
        byId("settingsFields").replaceChildren();
      }
    } else {
      baselineVersion = null;
      settingsRenderer = null;
      byId("settingsFields").replaceChildren();
    }
    if (section.status) {
      const result = results[resultIndex++];
      if (result.status === "fulfilled") {
        renderFields(byId("statusFields"), section.status.fields, result.value.values);
      } else {
        failed = true;
        byId("statusFields").replaceChildren();
      }
    } else byId("statusFields").replaceChildren();
    renderActions(section);
    const hasDraft = settingsRenderer && settingsRenderer.hasChanges();
    if (options.conflict) {
      setPageState(
        PAGE_STATE.versionConflict,
        moduleMessage(section.settings, "conflict"),
      );
    } else if (failed) {
      setPageState(PAGE_STATE.networkUnknown, "部分数据暂时无法读取，请稍后刷新。");
    } else if (hasDraft) {
      setPageState(PAGE_STATE.dirty, moduleMessage(section.settings, "dirty"));
    } else if (options.succeeded) {
      setPageState(PAGE_STATE.succeeded, moduleMessage(section.settings, "succeeded"), "success");
    } else setPageState(PAGE_STATE.synced, "");
    const stored = readPendingFact();
    if (stored && stored.section === section.id) {
      pendingFact = stored;
      setPageState(PAGE_STATE.saving, "正在继续查询已受理请求的结果…");
      if (stored.action === "settings" && section.settings) pollSettings(stored);
      else if (section.actions) pollAction(stored);
    }
  }

  function onDraftChange() {
    if (!settingsRenderer) return;
    if (settingsRenderer.hasChanges()) {
      preservedDrafts.set(activeSection.id, settingsRenderer.exportDraft());
      if (pageState !== PAGE_STATE.saving && pageState !== PAGE_STATE.versionConflict) {
        setPageState(PAGE_STATE.dirty, moduleMessage(activeSection.settings, "dirty"));
      } else updateSettingsControls();
    } else {
      preservedDrafts.delete(activeSection.id);
      setPageState(PAGE_STATE.synced, "");
    }
  }

  async function openSection(section, pushHistory) {
    if (pushHistory) history.pushState({ settingsSection: section.id }, "");
    await loadDetail(section);
  }

  async function refreshBaselinePreservingDraft() {
    // reload authoritative snapshot while the local draft remains in memory
    const draft = settingsRenderer ? settingsRenderer.exportDraft() : [];
    preservedDrafts.set(activeSection.id, draft);
    await loadDetail(activeSection, { draft, conflict: true });
  }

  function reasonState(reason) {
    if (reason === "version_conflict") return PAGE_STATE.versionConflict;
    if (reason === "owner_busy") return PAGE_STATE.ownerBusy;
    if (reason === "validation_failed") return PAGE_STATE.validationError;
    return PAGE_STATE.networkUnknown;
  }

  async function pollSettings(fact, manual = false) {
    if (!activeSection || !activeSection.settings || fact.section !== activeSection.id) return;
    const deadlineReached = Date.now() - fact.start >= POLL_DEADLINE_MS;
    if (deadlineReached && !manual) {
      setPageState(
        PAGE_STATE.networkUnknown,
        moduleMessage(activeSection.settings, "deadline"),
      );
      return;
    }
    try {
      const payload = await activeSection.settings.adapter.getResult(fact.section, fact.request);
      if (payload.state === "pending") {
        if (deadlineReached || manual) {
          setPageState(
            PAGE_STATE.networkUnknown,
            moduleMessage(activeSection.settings, "pending"),
            "info",
          );
        } else {
          pollTimer = window.setTimeout(() => pollSettings(fact), pollDelay(payload));
        }
        return;
      }
      clearPendingFact();
      if (payload.state === "succeeded") {
        preservedDrafts.delete(activeSection.id);
        await loadDetail(activeSection, { succeeded: true });
        if (leaveResolver) closeDraftDialog(LEAVE_RESULT.save);
        return;
      }
      const state = reasonState(payload.reason);
      if (state === PAGE_STATE.versionConflict) {
        await refreshBaselinePreservingDraft();
      } else {
        setPageState(state, payload.message || moduleMessage(activeSection.settings, "failed"));
      }
      enableDraftDialog();
    } catch (error) {
      if (!consoleApi.getToken()) {
        setPageState(
          PAGE_STATE.sessionExpired,
          moduleMessage(activeSection.settings, "sessionExpired"),
        );
      } else {
        setPageState(
          PAGE_STATE.networkUnknown,
          moduleMessage(activeSection.settings, "resultUnknown"),
        );
      }
      enableDraftDialog();
    }
  }

  async function saveActive() {
    if (!activeSection || !activeSection.settings || !settingsRenderer ||
        pageState === PAGE_STATE.saving) return false;
    let changes;
    try {
      changes = settingsRenderer.readChanges();
    } catch (error) {
      setPageState(PAGE_STATE.validationError, error.message, "error");
      return false;
    }
    if (changes.length === 0) return true;
    setPageState(PAGE_STATE.saving, moduleMessage(activeSection.settings, "submitting"));
    try {
      const payload = await activeSection.settings.adapter.submit(
        activeSection.id, baselineVersion, changes,
      );
      const fact = {
        section: activeSection.id,
        action: "settings",
        request: payload.requestId,
        start: Date.now(),
      };
      writePendingFact(fact);
      pollSettings(fact);
      return null;
    } catch (error) {
      if (!consoleApi.getToken()) {
        setPageState(
          PAGE_STATE.sessionExpired,
          moduleMessage(activeSection.settings, "sessionExpired"),
        );
      } else if (error.status === 422) {
        if (settingsRenderer) settingsRenderer.setErrors(error.payload && error.payload.errors);
        setPageState(PAGE_STATE.validationError, error.message, "error");
      } else if (error.status === 409 && error.payload &&
                 error.payload.error === "version_conflict") {
        await refreshBaselinePreservingDraft();
      } else if (error.status === 409) {
        setPageState(PAGE_STATE.ownerBusy, error.message, "error");
      } else {
        setPageState(
          PAGE_STATE.networkUnknown,
          moduleMessage(activeSection.settings, "submitUnknown"),
          "error",
        );
      }
      return false;
    }
  }

  function actionFailureState(error, hasToken) {
    if (!hasToken) return PAGE_STATE.sessionExpired;
    if (error.status === 422) return PAGE_STATE.validationError;
    if (error.status === 409) return PAGE_STATE.ownerBusy;
    return PAGE_STATE.networkUnknown;
  }

  async function submitAction(section, action, renderer, button) {
    if (!section.actions || pageState === PAGE_STATE.saving) return;
    let inputs;
    try {
      inputs = renderer.readAllValues();
    } catch (error) {
      setPageState(PAGE_STATE.validationError, error.message, "error");
      return;
    }
    const inputKey = actionInputKey(section.id, action.id);
    preservedActionInputs.set(inputKey, inputs);
    button.disabled = true;
    setPageState(
      PAGE_STATE.saving,
      moduleMessage(section.actions, "submitting", action.label),
    );
    try {
      const payload = await section.actions.adapter.submit(section.id, action.id, inputs);
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
        ? moduleMessage(section.actions, "sessionExpired")
        : (state === PAGE_STATE.networkUnknown
          ? moduleMessage(section.actions, "unknown") : error.message);
      setPageState(
        state,
        message,
        "error",
      );
    }
  }

  async function pollAction(fact, manual = false) {
    if (!activeSection || !activeSection.actions || fact.section !== activeSection.id) return;
    const deadlineReached = Date.now() - fact.start >= POLL_DEADLINE_MS;
    if (deadlineReached && !manual) {
      setPageState(
        PAGE_STATE.networkUnknown,
        moduleMessage(activeSection.actions, "unknown"),
      );
      return;
    }
    try {
      const payload = await activeSection.actions.adapter.getResult(
        fact.section, fact.action, fact.request,
      );
      if (payload.state === "pending") {
        if (deadlineReached || manual) {
          setPageState(
            PAGE_STATE.networkUnknown,
            moduleMessage(activeSection.actions, "pending"),
            "info",
          );
        } else {
          pollTimer = window.setTimeout(() => pollAction(fact), pollDelay(payload));
        }
        return;
      }
      clearPendingFact();
      if (payload.state === "succeeded") {
        preservedActionInputs.delete(actionInputKey(fact.section, fact.action));
        setPageState(
          PAGE_STATE.succeeded,
          moduleMessage(activeSection.actions, "succeeded"),
          "success",
        );
      } else {
        setPageState(
          reasonState(payload.reason),
          payload.message || moduleMessage(activeSection.actions, "failed"),
          "error",
        );
      }
    } catch (error) {
      if (!consoleApi.getToken()) {
        setPageState(
          PAGE_STATE.sessionExpired,
          moduleMessage(activeSection.actions, "sessionExpired"),
          "error",
        );
      } else {
        setPageState(
          PAGE_STATE.networkUnknown,
          moduleMessage(activeSection.actions, "unknown"),
          "error",
        );
      }
    }
  }

  function discardActiveDraft() {
    if (activeSection) preservedDrafts.delete(activeSection.id);
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
    for (const button of focusableElements()) button.disabled = false;
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

  async function requestReturnHome() {
    const result = await confirmLeave(byId("settingsBack"));
    if (!handleLeaveResult(result)) return;
    history.replaceState(null, "");
    showHome();
  }

  function mountModule(kind, capability, adapter) {
    providers.set(kind, { capability, adapter });
    const module = byId(`${kind}Module`);
    const host = byId(`${kind}ModuleHost`);
    if (module && host) {
      host.append(module);
      module.classList.remove("hidden");
    }
    sections = mergeSectionCapabilities();
    if (!activeSection) renderHome();
  }

  function unmountModule(kind) {
    if (settingsRenderer && settingsRenderer.hasChanges() && activeSection) {
      preservedDrafts.set(activeSection.id, settingsRenderer.exportDraft());
    }
    providers.delete(kind);
    cancelPolling();
  }

  byId("settingsBack").addEventListener("click", requestReturnHome);
  const settingsForm = byId("settingsForm");
  if (settingsForm) {
    settingsForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      await saveActive();
    });
  }
  const undo = byId("settingsUndo");
  if (undo) undo.addEventListener("click", () => {
    preservedDrafts.delete(activeSection.id);
    loadDetail(activeSection);
  });
  const checkResult = byId("pendingResultCheck");
  if (checkResult) checkResult.addEventListener("click", () => {
    if (!pendingFact) return;
    if (pendingFact.action === "settings") pollSettings(pendingFact, true);
    else pollAction(pendingFact, true);
  });
  const refresh = byId("statusRefresh");
  if (refresh) refresh.addEventListener("click", () => {
    if (activeSection) loadDetail(activeSection);
  });
  const draftDialog = byId("draftDialog");
  if (draftDialog) {
    byId("draftSaveLeave").addEventListener("click", async () => {
      for (const button of focusableElements()) button.disabled = true;
      const result = await saveActive();
      if (result === true) closeDraftDialog(LEAVE_RESULT.save);
      else if (result === false) enableDraftDialog();
    });
    byId("draftDiscardLeave").addEventListener("click", () => {
      closeDraftDialog(LEAVE_RESULT.discard);
    });
    byId("draftCancelLeave").addEventListener(
      "click", () => closeDraftDialog(LEAVE_RESULT.cancel),
    );
    draftDialog.addEventListener("keydown", (event) => {
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
  }
  window.addEventListener("beforeunload", (event) => {
    if (!settingsRenderer || !settingsRenderer.hasChanges()) return;
    event.preventDefault();
    event.returnValue = "";
  });
  window.addEventListener("popstate", async (event) => {
    if (!activeSection) return;
    const result = await confirmLeave(byId("settingsBack"));
    if (!handleLeaveResult(result)) {
      history.pushState({ settingsSection: activeSection.id }, "");
      return;
    }
    if (event.state && typeof event.state.settingsSection === "string") {
      const section = sections.find((item) => item.id === event.state.settingsSection);
      if (section) await openSection(section, false);
    } else showHome();
  });

  window.webConsole.fields = Object.freeze({
    confirmLeave,
    handleLeaveResult,
    mountModule,
    unmountModule,
  });
})();
