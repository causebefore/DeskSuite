(() => {
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const availability = data.availability || {};
  const calendarAvailable = availability.calendar !== false;
  const byId = (id) => document.getElementById(id);
  const setText = (id, value, fallback = "--") => {
    const node = byId(id);
    if (node) node.textContent = value ?? fallback;
  };

  const dateText = String(data.date || "");
  const dateParts = dateText.match(/^(?:\d{4}年)?(\d{1,2})月(\d{1,2})日$/);
  setText("weekday", data.weekday);
  setText("day", dateParts ? dateParts[2] : data.day);
  setText("month", dateParts ? `${dateParts[1]} 月` : dateText);
  setText("date", dateText);
  setText("time", data.time);

  const calendar = byId("calendar");
  const events = calendarAvailable ? (data.calendar || []).slice(0, 6) : [];
  setText("event-count", calendarAvailable ? `${events.length} 个日程` : "日程不可用");

  events.forEach((item) => {
    const row = document.createElement("article");
    row.className = `event flex flex--row flex--center-y${item.all_day ? " all-day" : ""}`;
    const time = document.createElement("div");
    time.className = "event-time content content--small font--bold pp-cn";
    time.textContent = item.all_day ? "全天" : (item.relative || "待定");
    const title = document.createElement("div");
    title.className = "event-title title title--small font--bold pp-cn";
    title.textContent = item.title || "未命名日程";
    const location = document.createElement("div");
    location.className = "event-location content content--small font--bold pp-cn";
    location.textContent = item.location || "无地点";
    row.append(time, title, location);
    calendar.appendChild(row);
  });

  if (!events.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state content content--small font--bold pp-cn";
    empty.textContent = calendarAvailable ? "今天没有安排" : "日程数据不可用";
    calendar.appendChild(empty);
  }

  window.__PHOTO_PAINTER_READY__ = true;
})();
