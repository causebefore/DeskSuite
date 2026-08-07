(() => {
  "use strict";

  const INTEGER_LIMITS = {
    int32: { min: -2147483648, max: 2147483647 },
    uint32: { min: 0, max: 4294967295 },
  };
  const consoleApi = window.webConsole;
  const providers = new Map();
  const byId = (id) => document.getElementById(id);
  const hasOwn = (object, key) =>
    object !== null && object !== undefined &&
    Object.prototype.hasOwnProperty.call(object, key);
  let sections = [];
  let activeSection = null;
  let pageState = "synced";
  let generation = 0;
  let pendingCheckHandler = null;
  let fieldSequence = 0;
  let descriptionSequence = 0;

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
        section[kind] = {
          ...source,
          adapter: record.adapter,
          controller: record.controller,
        };
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
        if (field.type === "int32" || field.type === "uint32") {
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
        input.addEventListener(
          field.type === "bool" || field.type === "enum" ? "change" : "input",
          markTouched,
        );
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

  function setPendingCheck(handler) {
    pendingCheckHandler = typeof handler === "function" ? handler : null;
    byId("pendingResultCheck").classList.toggle("hidden", pendingCheckHandler === null);
  }

  function setPageState(nextState, message = "", noticeState = "info") {
    pageState = nextState;
    byId("settingsDetail").dataset.state = nextState;
    consoleApi.setNotice(byId("settingsLiveMessage"), message, noticeState);
    for (const record of providers.values()) {
      if (record.controller && typeof record.controller.updateControls === "function") {
        record.controller.updateControls(nextState);
      }
    }
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
    for (const kind of ["settings", "status", "actions"]) {
      const part = section[kind];
      if (!part || typeof part.controller.getSummary !== "function") continue;
      requests.push(part.controller.getSummary(part).then(
        (payload) => summaryFields(part, payload),
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

  function resetDetail(preserve) {
    generation += 1;
    setPendingCheck(null);
    for (const record of providers.values()) {
      const controller = record.controller;
      if (!controller) continue;
      const method = preserve ? controller.unmount : controller.resetDetail;
      if (typeof method === "function") method.call(controller);
    }
    activeSection = null;
    pageState = "synced";
    byId("settingsDetailTitle").textContent = "";
    byId("settingsDetailDescription").textContent = "";
    consoleApi.setNotice(byId("settingsLiveMessage"), "");
    byId("settingsDetail").classList.add("hidden");
    byId("settingsHome").classList.remove("hidden");
  }

  function showHome() {
    resetDetail(false);
    renderHome();
  }

  async function loadDetail(section, options = {}) {
    const currentGeneration = ++generation;
    activeSection = section;
    setPendingCheck(null);
    byId("settingsHome").classList.add("hidden");
    byId("settingsDetail").classList.remove("hidden");
    byId("settingsDetailTitle").textContent = section.label;
    byId("settingsDetailDescription").textContent = section.description;
    setPageState("loading", "正在读取设备数据…");
    const view = Object.freeze({
      renderFields,
      setPageState,
      getPageState: () => pageState,
      setPendingCheck,
      isCurrent: () => currentGeneration === generation && activeSection === section,
      reload: () => loadDetail(section),
    });
    const tasks = [];
    for (const kind of ["settings", "status", "actions"]) {
      const part = section[kind];
      if (part && typeof part.controller.loadDetail === "function") {
        tasks.push(part.controller.loadDetail(part, { section, options, view }));
      }
    }
    const results = await Promise.allSettled(tasks);
    if (!view.isCurrent()) return;
    const presentations = results
      .filter((result) => result.status === "fulfilled" && result.value)
      .map((result) => result.value);
    if (results.some((result) => result.status === "rejected") && presentations.length === 0) {
      setPageState("network_unknown", "部分数据暂时无法读取，请稍后刷新。");
    } else if (presentations.length > 0) {
      const selected = presentations[presentations.length - 1];
      setPageState(
        selected.state,
        selected.message || (selected.state === "saving" ? "正在查询原请求…" : ""),
        selected.notice || "info",
      );
    } else setPageState("synced", "");
  }

  async function openSection(section, pushHistory) {
    if (pushHistory) history.pushState({ settingsSection: section.id }, "");
    await loadDetail(section);
  }

  function leaveController(method, ...args) {
    for (const record of providers.values()) {
      const controller = record.controller;
      if (controller && typeof controller[method] === "function") {
        return controller[method](...args);
      }
    }
    if (method === "confirmLeave") return Promise.resolve("proceed");
    return method === "handleLeaveResult";
  }

  function confirmLeave(trigger) {
    return leaveController("confirmLeave", trigger);
  }

  function handleLeaveResult(result) {
    return leaveController("handleLeaveResult", result);
  }

  async function requestReturnHome() {
    const result = await confirmLeave(byId("settingsBack"));
    if (!handleLeaveResult(result)) return;
    history.replaceState(null, "");
    showHome();
  }

  function mountModule(kind, capability, adapter, controller = {}) {
    providers.set(kind, { capability, adapter, controller });
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
    if (activeSection) resetDetail(true);
    else {
      const record = providers.get(kind);
      if (record && record.controller && typeof record.controller.unmount === "function") {
        record.controller.unmount();
      }
    }
    providers.delete(kind);
    sections = mergeSectionCapabilities();
  }

  function reloadActive() {
    if (activeSection) return loadDetail(activeSection);
    return Promise.resolve();
  }

  byId("settingsBack").addEventListener("click", requestReturnHome);
  byId("pendingResultCheck").addEventListener("click", () => {
    if (pendingCheckHandler) pendingCheckHandler();
  });
  window.addEventListener("beforeunload", (event) => {
    if (!leaveController("hasUnsaved")) return;
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
    reloadActive,
    unmountModule,
  });
})();
