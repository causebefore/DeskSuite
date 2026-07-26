(() => {
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const weather = data.weather || {};
  const mailData = data.mail || {};
  const quotaData = data.quota || {};
  const deviceStatus = data.device_status || {};
  const availability = data.availability || {};
  const weatherIcons = data.qweather_icons || {};
  const weatherAvailable = availability.weather !== false;
  const calendarAvailable = availability.calendar !== false;
  const mailAvailable = availability.mail !== false;
  const quotaAvailable = availability.quota !== false && quotaData.available !== false;

  const byId = (id) => document.getElementById(id);
  const setText = (id, value, fallback = "--") => {
    const node = byId(id);
    if (node) node.textContent = value ?? fallback;
  };
  const add = (parent, className, text = "") => {
    const node = document.createElement("div");
    node.className = className;
    node.textContent = text ?? "";
    parent.appendChild(node);
    return node;
  };
  const showEmpty = (parent, text) => add(parent, "empty-state content content--small font--bold pp-cn", text);
  const setWeatherIcon = (node, code, fallback) => {
    const svg = weatherIcons[String(code || "")];
    if (svg) {
      // SVG 来自服务端固定版本的本地和风天气资源，不接收用户 HTML。
      node.innerHTML = svg;
      node.setAttribute("aria-label", fallback || "天气");
      return;
    }
    node.textContent = fallback || "--";
  };
  const shortDate = (value) => {
    const match = String(value || "").match(/^\d{4}-(\d{2})-(\d{2})$/);
    return match ? `${match[1]}/${match[2]}` : (value || "--");
  };

  setText("date", data.date);
  setText("weekday", data.weekday);
  setText("time", data.time);
  setText("city", weather.city);
  setText("weather-summary", weatherAvailable ? weather.text : "不可用");
  setText("temp-large", weatherAvailable ? weather.temp_c : null);
  setText("feels", weatherAvailable ? weather.feels_like_c : null);
  setText("humidity", weatherAvailable ? weather.humidity_percent : null);
  setText("air", weatherAvailable ? weather.air : null);

  const statusNumber = (value) => {
    if (value === null || value === undefined || value === "") return null;
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  };
  const indoorTemperature = statusNumber(deviceStatus.temperature_c);
  const indoorHumidity = statusNumber(deviceStatus.humidity_percent);
  const batteryPercent = statusNumber(deviceStatus.battery_percent);
  setText(
    "device-temperature",
    indoorTemperature !== null ? `${indoorTemperature.toFixed(indoorTemperature % 1 ? 1 : 0)}°` : "--",
  );
  setText("device-humidity", indoorHumidity !== null ? `${Math.round(indoorHumidity)}%` : "--");
  setText("device-battery", batteryPercent !== null ? `${Math.round(batteryPercent)}%` : "--");
  setText("battery-label", "电量");

  if (weatherAvailable) {
    setWeatherIcon(byId("weather-main-icon"), weather.icon, weather.text);
  } else {
    setText("weather-main-icon", "不可用");
  }

  const forecast = byId("forecast");
  const daily = weatherAvailable ? (weather.daily || []).slice(0, 3) : [];
  daily.forEach((item) => {
    const box = add(forecast, "forecast-item flex flex--col flex--center");
    add(box, "forecast-date description description--base font--bold", shortDate(item.date));
    const icon = add(box, "forecast-icon icon");
    setWeatherIcon(icon, item.icon, item.text);
    add(box, "forecast-text content content--small font--bold pp-cn", item.text);
    add(box, "forecast-range content content--small font--bold", `${item.min ?? "--"}° / ${item.max ?? "--"}°`);
  });
  if (!daily.length) showEmpty(forecast, weatherAvailable ? "暂无预报" : "天气数据不可用");

  const calendar = byId("calendar");
  const events = calendarAvailable ? (data.calendar || []).slice(0, 3) : [];
  setText("calendar-count", calendarAvailable ? `${events.length} 项` : "不可用");
  events.forEach((item) => {
    const row = add(calendar, "agenda-item flex flex--row flex--center-y");
    add(row, "agenda-time content content--small font--bold pp-cn", item.relative);
    add(row, "agenda-name title title--small font--bold pp-cn", item.title);
    add(row, "agenda-place content content--small font--bold pp-cn", item.location || "未指定地点");
  });
  if (!events.length) showEmpty(calendar, calendarAvailable ? "近期没有日程" : "日程数据不可用");

  setText("mail-count", mailAvailable ? `${mailData.unread_count ?? 0} 未读` : "不可用");
  const mail = byId("mail");
  const messages = mailAvailable ? (mailData.messages || []).slice(0, 2) : [];
  messages.forEach((item) => {
    const row = add(mail, "mail-item flex flex--row gap--small");
    const meta = add(row, "mail-meta content content--small font--bold pp-cn");
    add(meta, "mail-from", item.from);
    add(meta, "mail-date", item.date);
    add(row, "mail-subject content content--small font--bold pp-cn", item.subject);
  });
  if (!messages.length) showEmpty(mail, mailAvailable ? "收件箱已清空" : "邮件数据不可用");

  setText("quota-level", quotaAvailable ? quotaData.level : "不可用");
  const quota = byId("quota");
  const quotaItems = quotaAvailable ? (quotaData.items || []).slice(0, 3) : [];
  quotaItems.forEach((item) => {
    const remaining = Math.max(0, Math.min(100, Number(item.remaining) || 0));
    const row = add(quota, "quota-item flex flex--col gap--xsmall");
    const label = add(row, "quota-label flex flex--row flex--between content content--small font--bold pp-cn");
    add(label, "quota-name", item.label);
    add(label, "quota-value value--tnums", `${remaining}%`);
    const progress = add(row, "progress-bar progress-bar--small");
    const track = add(progress, "track quota-track");
    const fill = add(track, "fill quota-fill");
    fill.style.width = `${remaining}%`;
  });
  if (!quotaItems.length) showEmpty(quota, quotaAvailable ? "暂无额度数据" : "额度数据不可用");

  window.__PHOTO_PAINTER_READY__ = true;
})();
