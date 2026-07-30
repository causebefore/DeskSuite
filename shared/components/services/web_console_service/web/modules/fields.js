(() => {
  "use strict";

  const INTEGER_LIMITS = {
    int32: { min: -2147483648, max: 2147483647 },
    uint32: { min: 0, max: 4294967295 },
  };
  const hasOwn = (object, key) =>
    object !== null && object !== undefined &&
    Object.prototype.hasOwnProperty.call(object, key);

  function valueEntries(values) {
    const result = new Map();
    if (!Array.isArray(values)) return result;
    for (const entry of values) {
      if (entry && typeof entry.id === "string" && !result.has(entry.id)) {
        result.set(entry.id, entry);
      }
    }
    return result;
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

  function displayValue(field, entry) {
    if (!hasOwn(entry, "value")) return "—";
    const value = entry.value;
    if (field.type === "bool") return value === true ? "已启用" : "未启用";
    if (field.type === "enum" && Array.isArray(field.options)) {
      const match = field.options.map(optionParts).find(
        (option) => option.value === String(value),
      );
      if (match) return match.label;
    }
    return value === null || value === undefined ? "—" : String(value);
  }

  function appendFieldHeading(row, field) {
    const heading = document.createElement("div");
    heading.className = "field-heading";
    const label = document.createElement("span");
    label.className = "field-label";
    label.textContent = typeof field.label === "string" ? field.label : field.id;
    heading.append(label);
    if (typeof field.description === "string" && field.description) {
      const description = document.createElement("p");
      description.className = "field-description";
      description.textContent = field.description;
      heading.append(description);
    }
    if (typeof field.effect === "string" && field.effect) {
      const effect = document.createElement("span");
      effect.className = "field-effect";
      effect.textContent = `生效方式：${field.effect}`;
      heading.append(effect);
    }
    row.append(heading);
  }

  function createInput(field, entry) {
    let input;
    if (field.type === "bool") {
      input = document.createElement("input");
      input.type = "checkbox";
      input.checked = hasOwn(entry, "value") ? entry.value === true : false;
      return input;
    }
    if (field.type === "enum") {
      input = document.createElement("select");
      for (const rawOption of Array.isArray(field.options) ? field.options : []) {
        const parts = optionParts(rawOption);
        const option = document.createElement("option");
        option.value = parts.value;
        option.textContent = parts.label;
        input.append(option);
      }
      if (hasOwn(entry, "value")) input.value = String(entry.value);
      return input;
    }
    input = document.createElement("input");
    input.type = "text";
    if (field.type === "int32") input.inputMode = "numeric";
    if (field.type === "uint32") input.inputMode = "numeric";
    if (field.type === "string" && Number.isInteger(field.maxBytes)) {
      input.maxLength = Math.max(0, field.maxBytes);
    }
    if (hasOwn(entry, "value")) input.value = String(entry.value);
    return input;
  }

  function rawInputValue(field, input) {
    return field.type === "bool" ? input.checked : input.value;
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
        .map(optionParts)
        .find((option) => option.value === raw);
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

  function renderFields(container, fields, values, options = {}) {
    const editable = options.editable === true;
    const entries = valueEntries(values);
    const controls = new Map();
    const seen = new Set();
    container.replaceChildren();

    for (const field of Array.isArray(fields) ? fields : []) {
      if (!field || typeof field.id !== "string" || seen.has(field.id)) {
        throw new Error("字段能力描述无效。");
      }
      seen.add(field.id);
      const entry = entries.get(field.id);
      const row = document.createElement("section");
      row.className = "field-row";
      row.dataset.fieldId = field.id;
      appendFieldHeading(row, field);
      const control = document.createElement("div");
      control.className = "field-control";
      const error = document.createElement("p");
      error.className = "field-error";
      error.setAttribute("role", "alert");

      if (field.secret === true) {
        const badge = document.createElement("span");
        badge.className = "field-badge";
        badge.textContent = entry && entry.configured === true ? "已配置" : "未配置";
        control.append(badge);
      } else if (editable && field.writable === true) {
        const input = createInput(field, entry);
        input.setAttribute("aria-label", typeof field.label === "string" ? field.label : field.id);
        control.append(input);
        const state = {
          field,
          input,
          error,
          touched: false,
          hasInitialValue: hasOwn(entry, "value"),
          initialRaw: rawInputValue(field, input),
        };
        const markTouched = () => {
          state.touched =
            !state.hasInitialValue || rawInputValue(field, input) !== state.initialRaw;
          error.textContent = "";
          if (typeof options.onDirtyChange === "function") options.onDirtyChange();
        };
        input.addEventListener(field.type === "bool" || field.type === "enum" ? "change" : "input", markTouched);
        controls.set(field.id, state);
      } else {
        const output = document.createElement("output");
        output.textContent =
          field.readable === false ? "不可读取" : displayValue(field, entry);
        control.append(output);
      }
      row.append(control);
      row.append(error);
      container.append(row);
    }

    return {
      hasChanges() {
        return [...controls.values()].some((state) => state.touched);
      },
      readChanges() {
        const changes = [];
        let firstError = null;
        for (const [id, state] of controls) {
          state.error.textContent = "";
          if (!state.touched) continue;
          try {
            changes.push({ id, value: validateInput(state.field, state.input) });
          } catch (error) {
            state.error.textContent = error.message;
            if (!firstError) firstError = error;
          }
        }
        if (firstError) throw firstError;
        return changes;
      },
      setErrors(errors) {
        for (const state of controls.values()) state.error.textContent = "";
        for (const item of Array.isArray(errors) ? errors : []) {
          const state = item && controls.get(item.id);
          if (state && typeof item.message === "string") {
            state.error.textContent = item.message;
          }
        }
      },
      setDisabled(disabled) {
        for (const state of controls.values()) state.input.disabled = disabled;
      },
    };
  }

  window.webConsole.fields = { render: renderFields };
})();
