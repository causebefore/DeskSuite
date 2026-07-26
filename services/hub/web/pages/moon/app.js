(() => {
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const moon = data.moon || {};
  const availability = data.availability || {};
  const available = availability.moon !== false;
  const current = available ? (moon.current || {}) : {};
  const icons = data.qweather_icons || {};

  const byId = (id) => document.getElementById(id);
  const setText = (id, value, fallback = "--") => {
    const node = byId(id);
    if (node) node.textContent = value ?? fallback;
  };
  const setIcon = (node, code, fallback) => {
    const svg = icons[String(code || "")];
    if (svg) {
      // SVG 来自服务端固定版本的本地 QWeather Icons，不接收外部 HTML。
      node.innerHTML = svg;
      node.setAttribute("aria-label", fallback || "月相");
      return;
    }
    node.textContent = fallback || "--";
  };
  const numeric = (value) => {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  };

  setText("moon-location", available ? moon.city : "月相数据不可用");
  setText(
    "moon-region",
    available ? [moon.adm1, moon.adm2].filter(Boolean).join(" · ") : "请检查和风天气配置",
  );
  setText("moon-phase-name", available ? current.name : "暂无月相");
  setText("moon-phase-time", available ? current.time : null);

  const phaseValue = available ? numeric(current.value) : null;
  setText("moon-phase-value", phaseValue === null ? null : phaseValue.toFixed(2));
  setIcon(byId("moon-hero-icon"), available ? current.icon : "", available ? current.name : "月相不可用");

  const illumination = available ? numeric(current.illumination) : null;
  const safeIllumination = illumination === null
    ? 0
    : Math.max(0, Math.min(100, Math.round(illumination)));
  setText("moon-illumination", illumination === null ? null : safeIllumination);
  setText("illumination-caption", illumination === null ? "暂无数据" : `${safeIllumination}% 可见`);
  byId("moon-progress-fill").style.width = `${safeIllumination}%`;

  setText("moonrise", available ? moon.moonrise : null);
  setText("moonset", available ? moon.moonset : null);

  const timeline = byId("moon-timeline");
  const points = available && Array.isArray(moon.timeline)
    ? moon.timeline.slice(0, 6)
    : [];
  while (points.length < 6) {
    points.push({ time: "--", illumination: null, icon: "", name: "月相不可用" });
  }
  points.forEach((point) => {
    const item = document.createElement("div");
    item.className = "moon-point";
    const time = document.createElement("span");
    time.className = "moon-point-time value value--xsmall value--tnums font--bold";
    time.textContent = point.time || "--";
    const icon = document.createElement("div");
    icon.className = "moon-point-icon icon";
    setIcon(icon, point.icon, point.name || "月相");
    const light = document.createElement("span");
    light.className = "moon-point-light value value--xsmall value--tnums font--bold";
    const pointIllumination = numeric(point.illumination);
    light.textContent = pointIllumination === null ? "--" : `${Math.round(pointIllumination)}%`;
    item.append(time, icon, light);
    timeline.appendChild(item);
  });

  window.__PHOTO_PAINTER_READY__ = true;
})();
