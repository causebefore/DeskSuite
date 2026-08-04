(() => {
  "use strict";

  const SESSION_ENDPOINT = "/api/session";
  const CAPABILITIES_ENDPOINT = "/api/capabilities";
  const TOKEN_KEY = "webConsoleSessionToken";
  const TOKEN_PATTERN = /^[0-9a-fA-F]{32}$/;
  const modules = new Map();
  let activeNavigation = null;

  const element = (id) => document.getElementById(id);

  function token() {
    const value = sessionStorage.getItem(TOKEN_KEY);
    return value && TOKEN_PATTERN.test(value) ? value : null;
  }

  function setNotice(target, message, state = "error") {
    target.textContent = message;
    target.classList.toggle("success", state === "success");
    target.classList.toggle("info", state === "info");
  }

  function registerModule(controller) {
    if (!controller || typeof controller.id !== "string" ||
        typeof controller.rootId !== "string" ||
        typeof controller.mount !== "function" || typeof controller.unmount !== "function") {
      throw new Error("网页控制台模块契约无效。");
    }
    if (controller.navigation !== undefined &&
        (!controller.navigation || typeof controller.navigation.id !== "string" ||
          typeof controller.navigation.label !== "string")) {
      throw new Error("网页控制台模块导航描述无效。");
    }
    if (modules.has(controller.id)) {
      throw new Error(`网页控制台模块重复注册：${controller.id}`);
    }
    modules.set(controller.id, controller);
  }

  function deactivateNavigation() {
    if (!activeNavigation) return;
    for (const module of activeNavigation.modules) {
      module.controller.unmount();
      module.root.classList.add("hidden");
    }
    activeNavigation = null;
  }

  function showLogin(message = "") {
    deactivateNavigation();
    sessionStorage.removeItem(TOKEN_KEY);
    element("consoleView").classList.add("hidden");
    element("loginView").classList.remove("hidden");
    element("moduleNavigation").replaceChildren();
    setNotice(element("consoleMessage"), "");
    setNotice(element("loginMessage"), message);
  }

  async function readApiError(response, fallback) {
    try {
      const payload = await response.json();
      if (payload && typeof payload.message === "string" && payload.message) {
        return payload.message;
      }
    } catch (_) {
      // 非 JSON 错误响应使用统一回退文案。
    }
    return `${fallback}（${response.status}）`;
  }

  async function apiFetch(url, options = {}) {
    const sessionToken = token();
    if (!sessionToken) {
      showLogin("登录已失效，请重新输入访问码。");
      throw new Error("登录已失效");
    }
    const headers = new Headers(options.headers || {});
    headers.set("Authorization", `Bearer ${sessionToken}`);
    const response = await fetch(url, { ...options, headers });
    if (response.status === 401) {
      showLogin("登录已失效，请重新输入访问码。");
      throw new Error("登录已失效");
    }
    return response;
  }

  function validateCapabilities(payload) {
    if (!payload || payload.schema !== 1 || !Array.isArray(payload.modules)) {
      throw new Error("设备返回的控制台能力描述无效。");
    }
    const seen = new Set();
    for (const capability of payload.modules) {
      if (!capability || typeof capability.id !== "string" ||
          typeof capability.label !== "string" || seen.has(capability.id)) {
        throw new Error("设备返回的控制台模块描述无效。");
      }
      seen.add(capability.id);
    }
    return payload.modules;
  }

  async function activateNavigation(navigation, button) {
    if (activeNavigation && activeNavigation.id === navigation.id) return;
    deactivateNavigation();
    document.querySelectorAll("#moduleNavigation button").forEach((item) => {
      item.classList.toggle("active", item === button);
      item.setAttribute("aria-current", item === button ? "page" : "false");
    });
    for (const module of navigation.modules) module.root.classList.remove("hidden");
    activeNavigation = navigation;
    try {
      await Promise.all(
        navigation.modules.map((module) => module.controller.mount(module.capability)),
      );
    } catch (error) {
      if (token()) setNotice(element("consoleMessage"), error.message);
    }
  }

  async function loadCapabilities() {
    setNotice(element("consoleMessage"), "正在读取设备能力…", "info");
    const response = await apiFetch(CAPABILITIES_ENDPOINT);
    if (!response.ok) throw new Error(await readApiError(response, "无法读取设备能力"));
    const capabilities = validateCapabilities(await response.json());
    const navigation = element("moduleNavigation");
    navigation.replaceChildren();
    const navigationEntries = new Map();

    for (const capability of capabilities) {
      const controller = modules.get(capability.id);
      if (!controller) continue;
      const root = element(controller.rootId);
      if (!root) throw new Error(`模块页面不存在：${controller.id}`);
      const descriptor = controller.navigation || {
        id: capability.id,
        label: capability.label,
      };
      let entry = navigationEntries.get(descriptor.id);
      if (!entry) {
        entry = { id: descriptor.id, label: descriptor.label, modules: [] };
        navigationEntries.set(descriptor.id, entry);
      } else if (entry.label !== descriptor.label) {
        throw new Error(`共享导航标签不一致：${descriptor.id}`);
      }
      entry.modules.push({ capability, controller, root });
    }

    const entries = [];
    for (const entry of navigationEntries.values()) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = entry.label;
      button.addEventListener("click", () => activateNavigation(entry, button));
      navigation.append(button);
      entries.push({ entry, button });
    }

    setNotice(element("consoleMessage"), "");
    element("emptyModuleView").classList.toggle("hidden", entries.length !== 0);
    if (entries.length !== 0) {
      await activateNavigation(entries[0].entry, entries[0].button);
    }
  }

  async function enterConsole() {
    element("loginView").classList.add("hidden");
    element("consoleView").classList.remove("hidden");
    element("emptyModuleView").classList.add("hidden");
    await loadCapabilities();
  }

  async function login(event) {
    event.preventDefault();
    const code = element("accessCode").value.trim();
    if (!/^\d{6}$/.test(code)) {
      setNotice(element("loginMessage"), "请输入 6 位数字访问码。");
      return;
    }
    const submit = element("loginView").querySelector("button");
    submit.disabled = true;
    setNotice(element("loginMessage"), "正在登录…", "info");
    try {
      const response = await fetch(SESSION_ENDPOINT, {
        method: "POST",
        headers: { "Content-Type": "text/plain; charset=utf-8" },
        body: code,
      });
      if (!response.ok) throw new Error(await readApiError(response, "访问码无效或登录失败"));
      const result = await response.json();
      if (!result || !TOKEN_PATTERN.test(result.token || "")) {
        throw new Error("服务器未返回有效的登录令牌。");
      }
      sessionStorage.setItem(TOKEN_KEY, result.token);
      setNotice(element("loginMessage"), "");
      await enterConsole();
    } catch (error) {
      if (token()) sessionStorage.removeItem(TOKEN_KEY);
      element("consoleView").classList.add("hidden");
      element("loginView").classList.remove("hidden");
      setNotice(element("loginMessage"), error.message);
    } finally {
      submit.disabled = false;
    }
  }

  async function logout() {
    const sessionToken = token();
    if (sessionToken) {
      try {
        await fetch(SESSION_ENDPOINT, {
          method: "DELETE",
          headers: { Authorization: `Bearer ${sessionToken}` },
        });
      } catch (_) {
        // 本地令牌仍必须立即清理。
      }
    }
    showLogin();
  }

  window.webConsole = {
    apiFetch,
    getToken: token,
    readApiError,
    registerModule,
    setNotice,
    expireSession: showLogin,
  };

  document.addEventListener("DOMContentLoaded", async () => {
    element("loginView").addEventListener("submit", login);
    element("logoutButton").addEventListener("click", logout);
    if (!token()) {
      showLogin();
      return;
    }
    try {
      await enterConsole();
    } catch (error) {
      if (token()) {
        showLogin(error.message);
      }
    }
  });
})();
