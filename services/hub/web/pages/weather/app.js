(() => {
  const SVG_NS = "http://www.w3.org/2000/svg";
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const weather = data.weather || {};
  const availability = data.availability || {};
  const weatherAvailable = availability.weather !== false;
  const weatherIcons = data.qweather_icons || {};
  const daily = weatherAvailable && Array.isArray(weather.daily) ? weather.daily.slice(0, 7) : [];

  const byId = (id) => document.getElementById(id);
  const valueOr = (value, fallback = "--") => (
    value === null || value === undefined || value === "" ? fallback : value
  );
  const numberOrNull = (value) => {
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  };
  const setText = (id, value, fallback = "--") => {
    const node = byId(id);
    if (node) node.textContent = valueOr(value, fallback);
  };
  const setWeatherIcon = (node, code, fallback) => {
    if (!node) return;
    const svg = weatherIcons[String(code || "")];
    if (svg) {
      // SVG 来自服务端固定版本的本地和风天气资源，不接收用户 HTML。
      node.innerHTML = svg;
      node.setAttribute("aria-label", fallback || "天气");
      return;
    }
    node.textContent = fallback || "--";
  };
  const formatClock = (value) => {
    const match = String(value || "").match(/T(\d{2}):(\d{2})/);
    return match ? `${match[1]}:${match[2]} 更新` : "更新时间 --";
  };
  const formatDate = (value) => {
    const match = String(value || "").match(/^(\d{4})-(\d{2})-(\d{2})$/);
    if (!match) return { label: valueOr(value), weekday: "--" };
    const date = new Date(Number(match[1]), Number(match[2]) - 1, Number(match[3]));
    const weekdays = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"];
    return { label: `${Number(match[2])}/${Number(match[3])}`, weekday: weekdays[date.getDay()] };
  };
  const svgNode = (name, attrs = {}, text = "") => {
    const node = document.createElementNS(SVG_NS, name);
    Object.entries(attrs).forEach(([key, value]) => node.setAttribute(key, String(value)));
    if (text) node.textContent = text;
    return node;
  };

  setText("city", weatherAvailable ? weather.city : "天气不可用");
  const district = [weather.adm1, weather.adm2]
    .filter((item, index, values) => item && item !== weather.city && values.indexOf(item) === index)
    .join(" · ");
  setText("district", weatherAvailable ? district : "", "");
  setText("date", data.date);
  setText("weekday", data.weekday);
  setText("observed-at", weatherAvailable ? formatClock(weather.observed_at) : "数据不可用");
  setText("weather-text", weatherAvailable ? weather.text : "数据不可用");
  setText("current-temp", weatherAvailable ? weather.temp_c : null);
  setText("feels-like", weatherAvailable && numberOrNull(weather.feels_like_c) !== null ? `${weather.feels_like_c}°` : null);
  setText("humidity", weatherAvailable && numberOrNull(weather.humidity_percent) !== null ? `${weather.humidity_percent}%` : null);
  const windText = weather.wind_dir || weather.wind_scale
    ? `${weather.wind_dir || ""}${weather.wind_scale ? ` ${weather.wind_scale}级` : ""}`.trim()
    : null;
  setText("wind", weatherAvailable ? windText : null);
  setText("precip", weatherAvailable && numberOrNull(weather.precip_mm) !== null ? `${weather.precip_mm}mm` : null);
  setText("attribution", weather.attribution || "QWeather");
  setWeatherIcon(byId("current-icon"), weatherAvailable ? weather.icon : "", weatherAvailable ? weather.text : "不可用");

  const today = daily[0] || {};
  setText(
    "today-range",
    daily.length ? `今日 ${valueOr(today.min)}° / ${valueOr(today.max)}°` : "今日 --° / --°",
  );

  const alerts = weatherAvailable && Array.isArray(weather.alerts) ? weather.alerts : [];
  const headline = byId("headline-strip");
  if (alerts.length) {
    headline.classList.add("is-alert");
    setText("headline-label", alerts[0].severity ? `${alerts[0].severity}预警` : "天气预警");
    setText("headline-text", alerts[0].title || alerts[0].type || "预警生效中");
  } else {
    setText("headline-label", weatherAvailable ? "天气提示" : "数据状态");
    setText(
      "headline-text",
      weatherAvailable ? weather.minutely?.summary : "和风天气数据暂不可用",
      weatherAvailable ? "未来两小时暂无降水" : "天气数据暂不可用",
    );
  }

  const drawTemperatureChart = () => {
    const svg = byId("temperature-chart");
    svg.replaceChildren();
    const points = daily
      .map((item, index) => ({ index, high: numberOrNull(item.max), low: numberOrNull(item.min) }))
      .filter((item) => item.high !== null && item.low !== null);
    if (!points.length) {
      svg.appendChild(svgNode("text", { x: 260, y: 68, class: "chart-empty" }, "暂无温度趋势数据"));
      return;
    }

    const temperatures = points.flatMap((item) => [item.high, item.low]);
    const rawMin = Math.min(...temperatures);
    const rawMax = Math.max(...temperatures);
    const span = Math.max(8, rawMax - rawMin + 4);
    const middle = (rawMax + rawMin) / 2;
    const scaleMin = middle - span / 2;
    const scaleMax = middle + span / 2;
    const left = 28;
    const right = 492;
    const top = 20;
    const bottom = 102;
    const xFor = (index) => Math.round(daily.length === 1
      ? (left + right) / 2
      : left + (right - left) * index / (daily.length - 1));
    const yFor = (temperature) => Math.round(
      bottom - (temperature - scaleMin) / (scaleMax - scaleMin) * (bottom - top),
    );

    [38, 82].forEach((y) => svg.appendChild(svgNode("line", { x1: 10, x2: 510, y1: y, y2: y, class: "chart-grid" })));
    if (daily.length > 1) {
      svg.appendChild(svgNode("line", { x1: xFor(0), x2: xFor(0), y1: 10, y2: 112, class: "chart-today" }));
    }

    const linePath = (key) => points
      .map((point, index) => `${index === 0 ? "M" : "L"} ${xFor(point.index)} ${yFor(point[key])}`)
      .join(" ");
    svg.appendChild(svgNode("path", { d: linePath("high"), class: "chart-high-line" }));
    svg.appendChild(svgNode("path", { d: linePath("low"), class: "chart-low-line" }));

    points.forEach((point) => {
      const x = xFor(point.index);
      const highY = yFor(point.high);
      const lowY = yFor(point.low);
      svg.appendChild(svgNode("circle", { cx: x, cy: highY, r: 4, class: "chart-high-dot" }));
      svg.appendChild(svgNode("rect", { x: x - 4, y: lowY - 4, width: 8, height: 8, class: "chart-low-dot" }));
      svg.appendChild(svgNode("text", { x, y: Math.max(11, highY - 9), class: "chart-label" }, `${point.high}°`));
      svg.appendChild(svgNode("text", { x, y: Math.min(119, lowY + 16), class: "chart-label" }, `${point.low}°`));
    });
  };

  setText("forecast-title", daily.length ? `未来 ${daily.length} 天温度` : "未来天气");
  drawTemperatureChart();

  const daysNode = byId("forecast-days");
  daysNode.style.gridTemplateColumns = `repeat(${Math.max(1, daily.length)}, minmax(0, 1fr))`;
  daily.forEach((item, index) => {
    const day = document.createElement("div");
    day.className = `forecast-day${index === 0 ? " is-today" : ""}`;
    const date = formatDate(item.date);
    const label = document.createElement("div");
    label.className = "forecast-day-label label label--small pp-cn-min";
    label.textContent = index === 0 ? "今天" : date.weekday;
    const icon = document.createElement("div");
    icon.className = "forecast-day-icon icon";
    setWeatherIcon(icon, item.icon, item.text);
    const text = document.createElement("div");
    text.className = "forecast-day-text content content--small font--bold pp-cn";
    text.textContent = item.text || date.label;
    day.append(label, icon, text);
    daysNode.appendChild(day);
  });
  if (!daily.length) {
    const empty = document.createElement("div");
    empty.className = "forecast-day content content--small font--bold pp-cn";
    empty.textContent = weatherAvailable ? "暂无预报" : "天气数据不可用";
    daysNode.appendChild(empty);
  }

  const minutelySummary = String(weather.minutely?.summary || "").trim();
  const minutelyPoints = weatherAvailable && Array.isArray(weather.minutely?.points)
    ? weather.minutely.points.slice(0, 24)
    : [];
  const minutelyUnavailable = !weatherAvailable || (
    !minutelyPoints.length
    && (!minutelySummary || /不可用|无法获取|缺少经纬度/.test(minutelySummary))
  );
  const rainValues = [];
  for (let index = 0; index < 12; index += 1) {
    const pair = minutelyPoints.slice(index * 2, index * 2 + 2);
    rainValues.push(Math.max(0, ...pair.map((item) => numberOrNull(item.precip_mm) || 0)));
  }
  const maxRain = Math.max(0, ...rainValues);
  const raining = !minutelyUnavailable && maxRain > 0;
  const rainCard = byId("rain-card");
  rainCard.classList.toggle("is-raining", raining);
  rainCard.classList.toggle("is-dry", !minutelyUnavailable && !raining);
  rainCard.classList.toggle("is-unavailable", minutelyUnavailable);
  const rainSummary = minutelyUnavailable
    ? (minutelySummary || "分钟降水暂不可用")
    : (minutelySummary || (raining ? "未来两小时有降水" : "未来两小时无降水"));
  setText("rain-summary", rainSummary);
  const bars = byId("rain-bars");
  if (raining) {
    rainValues.forEach((value) => {
      const bar = document.createElement("span");
      bar.className = "rain-bar";
      bar.style.height = `${Math.max(4, Math.round(value / maxRain * 38))}px`;
      bars.appendChild(bar);
    });
  }

  setText("aqi", weatherAvailable ? weather.aqi : null);
  setText("air-category", weatherAvailable ? weather.air : null);
  const pollutantLabels = {
    pm2p5: "PM2.5",
    pm10: "PM10",
    o3: "O₃",
    no2: "NO₂",
    so2: "SO₂",
    co: "CO",
  };
  setText(
    "air-primary",
    weatherAvailable ? (pollutantLabels[weather.air_primary] || weather.air_primary) : null,
  );
  const uv = numberOrNull(today.uv_index);
  const uvLevel = uv === null ? "" : uv <= 2 ? "低" : uv <= 5 ? "中等" : uv <= 7 ? "较强" : uv <= 10 ? "很强" : "极强";
  setText("uv-index", uv === null ? null : `${uv} · ${uvLevel}`);
  setText("sunrise", weatherAvailable ? today.sunrise : null, "--:--");
  setText("sunset", weatherAvailable ? today.sunset : null, "--:--");
  setText("visibility", weatherAvailable && numberOrNull(weather.vis_km) !== null ? `${weather.vis_km} km` : null);
  setText("pressure", weatherAvailable && numberOrNull(weather.pressure_hpa) !== null ? `${weather.pressure_hpa} hPa` : null);

  window.__PHOTO_PAINTER_READY__ = true;
})();
