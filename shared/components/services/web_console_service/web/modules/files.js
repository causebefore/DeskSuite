/*
 * Adapted from Crosslink src/network/html/FilesPage.html.
 * Copyright (c) 2025 Dave Allie, MIT License.
 * Modified for authenticated embedded HTTP file management.
 */
(() => {
  "use strict";

  const API = {
    files: "/api/files",
    file: "/api/file",
    directory: "/api/directory",
  };
  const consoleApi = window.webConsole;
  const byId = (id) => document.getElementById(id);
  let currentPath = "/";
  let currentEntries = [];
  let operationActive = false;
  let operationKind = "";
  let activeUploadRequest = null;
  let failedUploadFile = null;
  let moduleActive = false;
  let moduleGeneration = 0;
  const selectedPaths = new Set();

  const fileInput = byId("fileInput");
  const uploadButton = byId("uploadButton");
  const cancelUploadButton = byId("cancelUploadButton");
  const retryUploadButton = byId("retryUploadButton");
  const progressBar = byId("progressBar");
  const progressText = byId("progressText");
  const fileMessage = byId("fileMessage");
  const filterInput = byId("filterInput");
  const sortSelect = byId("sortSelect");
  const selectAll = byId("selectAll");
  const newFolderButton = byId("newFolderButton");
  const renameButton = byId("renameButton");
  const moveButton = byId("moveButton");
  const deleteButton = byId("deleteButton");
  const editors = ["folderEditor", "renameEditor", "moveEditor"].map(byId);

  function setMessage(message, state = "error") {
    consoleApi.setNotice(fileMessage, message, state);
  }

  function pathFor(name) {
    return currentPath === "/" ? `/${name}` : `${currentPath}/${name}`;
  }

  function parentPath(path) {
    const index = path.lastIndexOf("/");
    return index <= 0 ? "/" : path.slice(0, index);
  }

  function baseName(path) {
    return path.slice(path.lastIndexOf("/") + 1);
  }

  function joinPath(parent, name) {
    return parent === "/" ? `/${name}` : `${parent}/${name}`;
  }

  function normalizeDirectoryPath(value) {
    let path = value.trim();
    if (!path) throw new Error("请输入目标目录路径。");
    if (path.includes("\\")) throw new Error("目录路径不能包含反斜杠。");
    if (!path.startsWith("/")) path = `/${path}`;
    if (path.length > 1 && path.endsWith("/")) path = path.slice(0, -1);
    if (path.split("/").some((part) => part === "." || part === "..")) {
      throw new Error("目录路径不能包含 . 或 ..。");
    }
    return path;
  }

  function validateItemName(value, label) {
    const name = value.trim();
    if (!name) throw new Error(`请输入${label}。`);
    if (name === "." || name === ".." || /[\u0000-\u001f\u007f/\\]/.test(name)) {
      throw new Error(`${label}包含无效字符。`);
    }
    if (new TextEncoder().encode(name).length > 255) {
      throw new Error(`${label}不能超过 255 字节。`);
    }
    return name;
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return "—";
    const units = ["B", "KB", "MB", "GB"];
    let value = Math.max(0, bytes);
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return `${value.toFixed(unit ? 1 : 0)} ${units[unit]}`;
  }

  function entryIsDirectory(entry) {
    return entry.type === "directory" || entry.isDirectory === true;
  }

  function entryIsRegularFile(entry) {
    return entry.type === "file" || entry.isDirectory === false;
  }

  async function browseDirectory(path) {
    const response = await consoleApi.apiFetch(
      `${API.files}?path=${encodeURIComponent(path)}`,
    );
    if (!response.ok) {
      throw new Error(await consoleApi.readApiError(response, "无法读取目录"));
    }
    const payload = await response.json();
    const resolvedPath = Array.isArray(payload) ? path : ((payload && payload.path) || path);
    const entries = Array.isArray(payload) ? payload : ((payload && payload.entries) || []);
    if (typeof resolvedPath !== "string" || !resolvedPath.startsWith("/") ||
        !Array.isArray(entries)) {
      throw new Error("设备返回的目录内容无效。");
    }
    return Array.isArray(payload)
      ? { path: resolvedPath, entries }
      : { ...payload, path: resolvedPath, entries };
  }

  function entryPath(entry) {
    return pathFor(entry.name);
  }

  function updateUploadControls() {
    uploadButton.disabled = operationActive || fileInput.files.length === 0;
    cancelUploadButton.disabled =
      !(operationActive && operationKind === "upload" && activeUploadRequest);
    retryUploadButton.classList.toggle("hidden", failedUploadFile === null);
    retryUploadButton.disabled = operationActive || failedUploadFile === null;
  }

  function selectedEntries() {
    return currentEntries.filter((entry) => selectedPaths.has(entryPath(entry)));
  }

  function visibleEntries() {
    const needle = filterInput.value.trim().toLocaleLowerCase();
    const entries = currentEntries.filter(
      (entry) => !needle || entry.name.toLocaleLowerCase().includes(needle),
    );
    const mode = sortSelect.value;
    return entries.slice().sort((left, right) => {
      const leftDirectory = entryIsDirectory(left);
      const rightDirectory = entryIsDirectory(right);
      if (leftDirectory !== rightDirectory) return leftDirectory ? -1 : 1;
      const nameOrder = left.name.localeCompare(
        right.name,
        "zh-CN",
        { numeric: true, sensitivity: "base" },
      );
      const leftSize = Number(left.sizeBytes ?? left.size ?? 0);
      const rightSize = Number(right.sizeBytes ?? right.size ?? 0);
      if (mode === "name-desc") return -nameOrder;
      if (mode === "size-desc" && leftSize !== rightSize) return rightSize - leftSize;
      if (mode === "size-asc" && leftSize !== rightSize) return leftSize - rightSize;
      if (mode === "type") {
        const leftType =
          leftDirectory ? "" : (left.name.includes(".") ? left.name.split(".").pop() : "");
        const rightType =
          rightDirectory ? "" : (right.name.includes(".") ? right.name.split(".").pop() : "");
        const typeOrder = leftType.localeCompare(rightType, "zh-CN", { sensitivity: "base" });
        if (typeOrder) return typeOrder;
      }
      return nameOrder;
    });
  }

  function updateSelectionControls() {
    const selected = selectedEntries();
    byId("selectionText").textContent =
      selected.length === 0 ? "未选择项目" : `已选择 ${selected.length} 项`;
    renameButton.disabled =
      operationActive || selected.length !== 1 || entryIsDirectory(selected[0]);
    moveButton.disabled =
      operationActive || selected.length === 0 || selected.some(entryIsDirectory);
    deleteButton.disabled = operationActive || selected.length === 0;

    const visiblePaths = visibleEntries().map(entryPath);
    const visibleSelected = visiblePaths.filter((path) => selectedPaths.has(path)).length;
    selectAll.checked = visiblePaths.length > 0 && visibleSelected === visiblePaths.length;
    selectAll.indeterminate = visibleSelected > 0 && visibleSelected < visiblePaths.length;
    selectAll.disabled = operationActive || visiblePaths.length === 0;
  }

  function setOperationActive(active, kind = "") {
    operationActive = active;
    operationKind = active ? kind : "";
    fileInput.disabled = active;
    filterInput.disabled = active;
    sortSelect.disabled = active;
    newFolderButton.disabled = active;
    document.querySelectorAll(
      "#filesView .name-button, #filesView .breadcrumbs button, " +
      "#filesView .row-select, #filesView .action-panel input, #filesView .action-panel button",
    ).forEach((item) => { item.disabled = active; });
    updateUploadControls();
    updateSelectionControls();
  }

  function renderBreadcrumbs() {
    const container = byId("breadcrumbs");
    container.replaceChildren();
    const parts = currentPath.split("/").filter(Boolean);
    let assembled = "";
    const entries = [
      { label: "根目录", path: "/" },
      ...parts.map((part) => {
        assembled += `/${part}`;
        return { label: part, path: assembled };
      }),
    ];
    entries.forEach((item, index) => {
      if (index) container.append(" / ");
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = item.label;
      button.disabled = operationActive;
      button.addEventListener("click", () => loadDirectory(item.path));
      container.append(button);
    });
  }

  function renderRows() {
    const rows = byId("fileRows");
    rows.replaceChildren();
    const entries = visibleEntries();
    if (entries.length === 0) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 4;
      cell.className = "empty-row";
      cell.textContent =
        currentEntries.length === 0 ? "当前目录为空" : "没有符合筛选条件的项目";
      row.append(cell);
      rows.append(row);
      updateSelectionControls();
      return;
    }

    for (const entry of entries) {
      const isDirectory = entryIsDirectory(entry);
      const path = entryPath(entry);
      const row = document.createElement("tr");
      row.classList.toggle("selected", selectedPaths.has(path));

      const checkCell = document.createElement("td");
      checkCell.className = "check-cell";
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.className = "row-select";
      checkbox.checked = selectedPaths.has(path);
      checkbox.disabled = operationActive;
      checkbox.setAttribute("aria-label", `选择 ${entry.name}`);
      checkbox.addEventListener("change", () => {
        if (checkbox.checked) selectedPaths.add(path);
        else selectedPaths.delete(path);
        row.classList.toggle("selected", checkbox.checked);
        updateSelectionControls();
      });
      checkCell.append(checkbox);
      row.append(checkCell);

      const nameCell = document.createElement("td");
      const nameButton = document.createElement("button");
      nameButton.type = "button";
      nameButton.className = "name-button";
      nameButton.textContent = entry.name;
      nameButton.disabled = operationActive;
      nameButton.addEventListener(
        "click",
        () => isDirectory ? loadDirectory(pathFor(entry.name)) : downloadFile(entry.name),
      );
      nameCell.append(nameButton);
      row.append(nameCell);

      const typeCell = document.createElement("td");
      typeCell.textContent = isDirectory ? "目录" : "文件";
      row.append(typeCell);
      const sizeCell = document.createElement("td");
      sizeCell.textContent =
        isDirectory ? "—" : formatBytes(Number(entry.sizeBytes ?? entry.size));
      row.append(sizeCell);
      rows.append(row);
    }
    updateSelectionControls();
  }

  function renderStorage(payload) {
    const total = Number(payload && payload.totalBytes);
    const free = Number(payload && payload.freeBytes);
    const storageBar = byId("storageBar");
    const storageText = byId("storageText");
    if (!Number.isFinite(total) || total <= 0 || !Number.isFinite(free)) {
      storageBar.style.width = "0%";
      storageText.textContent = "容量信息不可用";
      return;
    }
    const safeFree = Math.min(total, Math.max(0, free));
    const used = total - safeFree;
    const percent = Math.min(100, Math.max(0, used / total * 100));
    storageBar.style.width = `${percent.toFixed(1)}%`;
    storageText.textContent =
      `已用 ${formatBytes(used)} / ${formatBytes(total)} · 可用 ${formatBytes(safeFree)}`;
  }

  function renderMoveDestinations() {
    const paths = new Set(["/", currentPath, parentPath(currentPath)]);
    currentEntries.filter(entryIsDirectory).forEach((entry) => paths.add(entryPath(entry)));
    const list = byId("moveDestinations");
    list.replaceChildren();
    [...paths].sort(
      (left, right) => left.localeCompare(right, "zh-CN", { numeric: true }),
    ).forEach((path) => {
      const option = document.createElement("option");
      option.value = path;
      list.append(option);
    });
  }

  function hideEditors() {
    editors.forEach((editor) => editor.classList.add("hidden"));
  }

  async function requestDirectory(path, requestGeneration) {
    setMessage("", "info");
    try {
      const payload = await browseDirectory(path);
      if (!moduleActive || requestGeneration !== moduleGeneration) return false;
      currentPath = payload.path;
      currentEntries = payload.entries;
      selectedPaths.clear();
      hideEditors();
      renderBreadcrumbs();
      renderStorage(payload);
      renderMoveDestinations();
      renderRows();
      return true;
    } catch (error) {
      if (consoleApi.getToken() && moduleActive && requestGeneration === moduleGeneration) {
        setMessage(error.message);
      }
      return false;
    }
  }

  async function loadDirectory(path = currentPath) {
    if (operationActive || !moduleActive) return;
    const requestGeneration = moduleGeneration;
    setOperationActive(true, "browse");
    try {
      await requestDirectory(path, requestGeneration);
    } finally {
      if (consoleApi.getToken() && moduleActive && requestGeneration === moduleGeneration) {
        setOperationActive(false);
      }
    }
  }

  function createFileFieldPicker(field, entry) {
    const suffix = String(field.fileSuffix || "").toLocaleLowerCase();
    const maxBytes = Number.isInteger(field.maxBytes) ? field.maxBytes : 0;
    const root = document.createElement("div");
    root.className = "file-field";

    const valueRow = document.createElement("div");
    valueRow.className = "file-field-value";
    const input = document.createElement("input");
    input.type = "text";
    input.readOnly = true;
    if (maxBytes > 0) input.maxLength = maxBytes;
    if (entry && Object.prototype.hasOwnProperty.call(entry, "value")) {
      input.value = String(entry.value);
    }
    const chooseButton = document.createElement("button");
    chooseButton.type = "button";
    chooseButton.className = "secondary";
    chooseButton.textContent = "选择文件";
    chooseButton.setAttribute("aria-expanded", "false");
    valueRow.append(input, chooseButton);

    const status = document.createElement("p");
    status.className = "file-field-status";
    status.setAttribute("role", "status");
    status.setAttribute("aria-live", "polite");

    const panel = document.createElement("div");
    panel.className = "file-picker hidden";
    panel.id = `filePicker-${field.id}`;
    chooseButton.setAttribute("aria-controls", panel.id);
    const toolbar = document.createElement("div");
    toolbar.className = "file-picker-toolbar";
    const upButton = document.createElement("button");
    upButton.type = "button";
    upButton.className = "secondary";
    upButton.textContent = "上一级";
    const directoryLabel = document.createElement("span");
    directoryLabel.className = "file-picker-path";
    const refreshButton = document.createElement("button");
    refreshButton.type = "button";
    refreshButton.className = "secondary";
    refreshButton.textContent = "刷新";
    const closeButton = document.createElement("button");
    closeButton.type = "button";
    closeButton.className = "secondary";
    closeButton.textContent = "关闭";
    toolbar.append(upButton, directoryLabel, refreshButton, closeButton);

    const pickerMessage = document.createElement("p");
    pickerMessage.className = "file-picker-message";
    pickerMessage.setAttribute("role", "status");
    const list = document.createElement("div");
    list.className = "file-picker-list";
    panel.append(toolbar, pickerMessage, list);
    root.append(valueRow, status, panel);

    let disabled = false;
    let loading = false;
    let pickerPath = "/";
    let cachedDirectory = null;

    function pathIsUsable(path) {
      return typeof path === "string" && path.startsWith("/") &&
        path.length > 1 && !path.includes("\\") && !path.includes("//");
    }

    function setStatus(text, state = "unknown") {
      status.textContent = text;
      status.dataset.state = state;
    }

    function setPickerMessage(text, state = "info") {
      pickerMessage.textContent = text;
      pickerMessage.dataset.state = state;
    }

    function updateDisabledState() {
      input.disabled = disabled;
      chooseButton.disabled = disabled || loading;
      upButton.disabled = disabled || loading || pickerPath === "/";
      refreshButton.disabled = disabled || loading;
      closeButton.disabled = disabled || loading;
      list.querySelectorAll("button").forEach((button) => {
        button.disabled = disabled || loading || button.dataset.pathTooLong === "true";
      });
    }

    function setLoading(nextLoading) {
      loading = nextLoading;
      updateDisabledState();
    }

    function pickerEntries(payload) {
      return payload.entries.filter((item) => item && typeof item.name === "string" &&
        item.name && !/[\/\\]/.test(item.name) &&
        (entryIsDirectory(item) ||
          (entryIsRegularFile(item) && item.name.toLocaleLowerCase().endsWith(suffix))));
    }

    function renderPickerDirectory(payload) {
      pickerPath = payload.path;
      directoryLabel.textContent = `目录：${pickerPath}`;
      directoryLabel.title = pickerPath;
      list.replaceChildren();
      const entries = pickerEntries(payload).sort((left, right) => {
        const typeOrder = Number(entryIsDirectory(right)) - Number(entryIsDirectory(left));
        return typeOrder || left.name.localeCompare(right.name, "zh-CN", { numeric: true });
      });
      if (entries.length === 0) {
        const empty = document.createElement("p");
        empty.className = "file-picker-empty";
        empty.textContent = `当前目录没有子目录或 ${suffix} 文件。`;
        list.append(empty);
      }
      for (const item of entries) {
        const itemPath = joinPath(pickerPath, item.name);
        const directory = entryIsDirectory(item);
        const pathTooLong = !directory && maxBytes > 0 &&
          new TextEncoder().encode(itemPath).length > maxBytes;
        const button = document.createElement("button");
        button.type = "button";
        button.className = "file-picker-entry";
        button.dataset.pathTooLong = pathTooLong ? "true" : "false";
        const name = document.createElement("span");
        name.textContent = item.name;
        const detail = document.createElement("span");
        detail.textContent = directory
          ? "目录"
          : (pathTooLong ? "路径过长" : formatBytes(Number(item.sizeBytes)));
        button.append(name, detail);
        button.addEventListener("click", () => {
          if (directory) {
            loadPickerDirectory(itemPath, true);
            return;
          }
          input.value = itemPath;
          input.dispatchEvent(new Event("input", { bubbles: true }));
          setStatus("当前文件存在于 SD 卡。", "available");
          panel.classList.add("hidden");
          chooseButton.setAttribute("aria-expanded", "false");
        });
        list.append(button);
      }
      setPickerMessage(`仅显示 ${suffix} 文件。`, "info");
      updateDisabledState();
    }

    async function loadPickerDirectory(path, force = false) {
      if (disabled || loading) return;
      setLoading(true);
      setPickerMessage("正在读取目录…", "info");
      try {
        const payload = !force && cachedDirectory && cachedDirectory.path === path
          ? cachedDirectory
          : await browseDirectory(path);
        cachedDirectory = payload;
        renderPickerDirectory(payload);
      } catch (error) {
        setPickerMessage(error.message, "error");
      } finally {
        setLoading(false);
      }
    }

    async function verifySelectedFile() {
      if (!pathIsUsable(input.value)) {
        setStatus("当前路径无效，请重新选择文件。", "missing");
        return;
      }
      const directory = parentPath(input.value);
      const selectedName = baseName(input.value);
      setLoading(true);
      setStatus("正在确认当前文件…", "unknown");
      try {
        const payload = await browseDirectory(directory);
        cachedDirectory = payload;
        const exists = payload.entries.some((item) => item && item.name === selectedName &&
          entryIsRegularFile(item));
        setStatus(
          exists ? "当前文件存在于 SD 卡。" : "当前文件在 SD 卡中不存在，请重新选择。",
          exists ? "available" : "missing",
        );
      } catch (error) {
        setStatus(`暂时无法确认当前文件：${error.message}`, "unknown");
      } finally {
        setLoading(false);
      }
    }

    chooseButton.addEventListener("click", () => {
      const opening = panel.classList.contains("hidden");
      panel.classList.toggle("hidden", !opening);
      chooseButton.setAttribute("aria-expanded", opening ? "true" : "false");
      if (opening) {
        const initialDirectory = pathIsUsable(input.value) ? parentPath(input.value) : "/";
        loadPickerDirectory(initialDirectory);
      }
    });
    upButton.addEventListener("click", () => loadPickerDirectory(parentPath(pickerPath), true));
    refreshButton.addEventListener("click", () => loadPickerDirectory(pickerPath, true));
    closeButton.addEventListener("click", () => {
      panel.classList.add("hidden");
      chooseButton.setAttribute("aria-expanded", "false");
    });

    verifySelectedFile();
    return {
      root,
      input,
      setDisabled(nextDisabled) {
        disabled = nextDisabled;
        updateDisabledState();
      },
    };
  }

  async function downloadFile(name) {
    if (operationActive || !moduleActive) return;
    setOperationActive(true, "download");
    progressBar.style.width = "0%";
    progressText.textContent = `正在下载 ${name}…`;
    try {
      const response = await consoleApi.apiFetch(
        `${API.file}?path=${encodeURIComponent(pathFor(name))}`,
      );
      if (!response.ok) {
        throw new Error(await consoleApi.readApiError(response, "下载失败"));
      }
      const blob = await response.blob();
      const link = document.createElement("a");
      link.href = URL.createObjectURL(blob);
      link.download = name;
      link.click();
      URL.revokeObjectURL(link.href);
      progressBar.style.width = "100%";
      progressText.textContent = `下载完成：${name}`;
    } catch (error) {
      if (consoleApi.getToken() && moduleActive) {
        setMessage(error.message);
        progressText.textContent = "";
      }
    } finally {
      if (consoleApi.getToken() && moduleActive) setOperationActive(false);
    }
  }

  function uploadFile(file, overwrite = false) {
    return new Promise((resolve, reject) => {
      const request = new XMLHttpRequest();
      const startedAt = performance.now();
      activeUploadRequest = request;
      updateUploadControls();
      request.open("PUT", `${API.file}?path=${encodeURIComponent(pathFor(file.name))}`);
      request.setRequestHeader("Authorization", `Bearer ${consoleApi.getToken()}`);
      if (overwrite) request.setRequestHeader("X-Web-Console-Overwrite", "confirm");
      request.upload.onprogress = (event) => {
        if (!event.lengthComputable || !moduleActive) return;
        const percent = Math.round(event.loaded / event.total * 100);
        const seconds = Math.max((performance.now() - startedAt) / 1000, .001);
        progressBar.style.width = `${percent}%`;
        progressText.textContent =
          `正在上传 ${formatBytes(event.loaded)} / ${formatBytes(event.total)}` +
          `（${percent}% · ${formatBytes(event.loaded / seconds)}/秒）`;
      };
      const finish = (callback) => {
        if (activeUploadRequest === request) activeUploadRequest = null;
        updateUploadControls();
        callback();
      };
      request.onload = () => finish(() => resolve(request));
      request.onerror = () => finish(() => reject(new Error("上传网络错误")));
      request.onabort = () => finish(() => {
        const error = new Error("上传已取消");
        error.cancelled = true;
        reject(error);
      });
      request.send(file);
    });
  }

  function uploadErrorCode(request) {
    try {
      return JSON.parse(request.responseText).error || "";
    } catch (_) {
      return "";
    }
  }

  async function startUpload(retryFile = null) {
    const file = retryFile || fileInput.files[0];
    if (!file || operationActive || !moduleActive) return;
    failedUploadFile = null;
    setOperationActive(true, "upload");
    setMessage("", "info");
    progressBar.style.width = "0%";
    let uploadSucceeded = false;
    try {
      let response = await uploadFile(file);
      if (response.status === 401) {
        consoleApi.expireSession("登录已失效，请重新输入访问码。");
        return;
      }
      if (response.status === 409 && uploadErrorCode(response) === "overwrite_required") {
        if (confirm("同名文件已存在，是否覆盖？")) {
          response = await uploadFile(file, true);
        } else {
          const error = new Error("已取消覆盖现有文件。");
          error.cancelled = true;
          throw error;
        }
      }
      if (response.status === 401) {
        consoleApi.expireSession("登录已失效，请重新输入访问码。");
        return;
      }
      if (response.status < 200 || response.status >= 300) {
        let message = `上传失败（${response.status}）`;
        try {
          message = JSON.parse(response.responseText).message || message;
        } catch (_) {
          // 非 JSON 响应保留状态码文案。
        }
        throw new Error(message);
      }
      progressBar.style.width = "100%";
      progressText.textContent = `上传完成：${file.name}`;
      fileInput.value = "";
      uploadSucceeded = true;
    } catch (error) {
      if (!moduleActive) return;
      progressText.textContent = "";
      if (error.cancelled) {
        const message =
          error.message === "上传已取消"
            ? "上传已取消，未完成的临时文件将被清理。"
            : error.message;
        setMessage(message, "info");
      } else {
        failedUploadFile = file;
        setMessage(`${error.message}，可以重试该文件。`);
      }
    } finally {
      if (consoleApi.getToken() && moduleActive) setOperationActive(false);
      updateUploadControls();
    }
    if (uploadSucceeded && consoleApi.getToken() && moduleActive) {
      await loadDirectory(currentPath);
    }
  }

  function cancelUpload() {
    if (!activeUploadRequest) return;
    cancelUploadButton.disabled = true;
    progressText.textContent = "正在取消上传…";
    activeUploadRequest.abort();
  }

  function showEditor(editor) {
    hideEditors();
    editor.classList.remove("hidden");
    const input = editor.querySelector("input");
    if (input) {
      input.focus();
      input.select();
    }
  }

  async function mutationRequest(url, method) {
    const response = await consoleApi.apiFetch(url, { method });
    if (!response.ok) {
      throw new Error(await consoleApi.readApiError(response, "文件操作失败"));
    }
  }

  async function performMutations(tasks, successMessage) {
    if (operationActive || tasks.length === 0 || !moduleActive) return;
    setOperationActive(true, "mutation");
    setMessage("", "info");
    const failures = [];
    for (const task of tasks) {
      try {
        await task.run();
      } catch (error) {
        failures.push(`${task.name}：${error.message}`);
        if (!consoleApi.getToken()) break;
      }
    }
    if (consoleApi.getToken() && moduleActive) setOperationActive(false);
    if (!consoleApi.getToken() || !moduleActive) return;
    await loadDirectory(currentPath);
    if (failures.length === 0) setMessage(successMessage, "success");
    else setMessage(failures.join("；"));
  }

  byId("folderForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const name = validateItemName(byId("folderName").value, "目录名称");
      const target = pathFor(name);
      await performMutations([{
        name,
        run: () => mutationRequest(
          `${API.directory}?path=${encodeURIComponent(target)}`,
          "PUT",
        ),
      }], `目录“${name}”已创建。`);
    } catch (error) {
      setMessage(error.message);
    }
  });

  byId("renameForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const selected = selectedEntries();
    if (selected.length !== 1 || entryIsDirectory(selected[0])) return;
    try {
      const name = validateItemName(byId("renameName").value, "文件名");
      const source = entryPath(selected[0]);
      const destination = joinPath(parentPath(source), name);
      await performMutations([{
        name: selected[0].name,
        run: () => mutationRequest(
          `${API.file}?path=${encodeURIComponent(source)}` +
          `&destination=${encodeURIComponent(destination)}`,
          "PATCH",
        ),
      }], `文件已重命名为“${name}”。`);
    } catch (error) {
      setMessage(error.message);
    }
  });

  byId("moveForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const selected = selectedEntries();
    if (selected.length === 0 || selected.some(entryIsDirectory)) return;
    try {
      const destinationDirectory = normalizeDirectoryPath(byId("moveDestination").value);
      const tasks = selected.map((entry) => {
        const source = entryPath(entry);
        const destination = joinPath(destinationDirectory, entry.name);
        return {
          name: entry.name,
          run: () => mutationRequest(
            `${API.file}?path=${encodeURIComponent(source)}` +
            `&destination=${encodeURIComponent(destination)}`,
            "PATCH",
          ),
        };
      });
      await performMutations(tasks, `${selected.length} 个文件已移动。`);
    } catch (error) {
      setMessage(error.message);
    }
  });

  uploadButton.addEventListener("click", () => startUpload());
  cancelUploadButton.addEventListener("click", cancelUpload);
  retryUploadButton.addEventListener("click", () => startUpload(failedUploadFile));
  fileInput.addEventListener("change", updateUploadControls);
  filterInput.addEventListener("input", renderRows);
  sortSelect.addEventListener("change", renderRows);
  selectAll.addEventListener("change", () => {
    for (const entry of visibleEntries()) {
      const path = entryPath(entry);
      if (selectAll.checked) selectedPaths.add(path);
      else selectedPaths.delete(path);
    }
    renderRows();
  });
  newFolderButton.addEventListener("click", () => {
    byId("folderName").value = "";
    showEditor(byId("folderEditor"));
  });
  renameButton.addEventListener("click", () => {
    const selected = selectedEntries();
    if (selected.length !== 1 || entryIsDirectory(selected[0])) return;
    byId("renameName").value = selected[0].name;
    showEditor(byId("renameEditor"));
  });
  moveButton.addEventListener("click", () => {
    byId("moveDestination").value = currentPath;
    showEditor(byId("moveEditor"));
  });
  deleteButton.addEventListener("click", async () => {
    const selected = selectedEntries();
    if (selected.length === 0) return;
    const names = selected.map((entry) => entry.name).join("、");
    if (!confirm(`确定删除以下 ${selected.length} 项吗？目录必须为空。\n${names}`)) return;
    const tasks = selected.map((entry) => ({
      name: entry.name,
      run: () => mutationRequest(
        `${API.file}?path=${encodeURIComponent(entryPath(entry))}`,
        "DELETE",
      ),
    }));
    await performMutations(tasks, `${selected.length} 个项目已删除。`);
  });
  document.querySelectorAll("#filesView .editorCancel").forEach(
    (button) => button.addEventListener("click", hideEditors),
  );

  updateUploadControls();
  consoleApi.files = Object.freeze({
    createFieldPicker: createFileFieldPicker,
  });
  consoleApi.registerModule({
    id: "files",
    rootId: "filesView",
    async mount() {
      moduleActive = true;
      moduleGeneration += 1;
      await loadDirectory(currentPath);
    },
    unmount() {
      moduleActive = false;
      moduleGeneration += 1;
      if (activeUploadRequest) activeUploadRequest.abort();
      activeUploadRequest = null;
      setOperationActive(false);
      hideEditors();
    },
  });
})();
