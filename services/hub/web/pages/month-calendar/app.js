(() => {
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const availability = data.availability || {};
  const monthData = data.calendar_month || {};
  const calendarAvailable = availability.calendar_month !== false;
  const byId = (id) => document.getElementById(id);
  const setText = (id, value, fallback = "--") => {
    const node = byId(id);
    if (node) node.textContent = value ?? fallback;
  };
  const pad2 = (value) => String(value).padStart(2, "0");
  const isoDate = (year, month, day) => `${year}-${pad2(month)}-${pad2(day)}`;

  const dateMatch = String(data.date || "").match(/^(\d{4})年(\d{1,2})月(\d{1,2})日$/);
  const year = Number(monthData.year || dateMatch?.[1]);
  const month = Number(monthData.month || dateMatch?.[2]);
  const todayDay = Number(dateMatch?.[3]);
  const validMonth = Number.isInteger(year) && Number.isInteger(month) && month >= 1 && month <= 12;
  const todayIso = validMonth && Number.isInteger(todayDay)
    ? isoDate(year, month, todayDay)
    : "";
  const events = calendarAvailable && Array.isArray(monthData.events)
    ? monthData.events.filter((item) => /^\d{4}-\d{2}-\d{2}$/.test(String(item.date || "")))
    : [];
  const currentMonthPrefix = validMonth ? `${year}-${pad2(month)}-` : "";
  const monthEvents = events.filter((item) => item.date.startsWith(currentMonthPrefix));

  setText("month-title", validMonth ? `${year}年 ${month}月` : "月份不可用");
  setText("month-count", calendarAvailable ? `本月 ${monthEvents.length} 条` : "日程不可用");
  setText("today-day", Number.isInteger(todayDay) ? todayDay : null);
  setText("today-weekday", data.weekday);
  setText("today-month", validMonth ? `${month}月` : null);
  setText(
    "calendar-source",
    calendarAvailable ? (monthData.source === "icloud" ? "iCloud CalDAV" : "本地预览") : "数据不可用",
  );

  const eventsByDate = new Map();
  monthEvents.forEach((event) => {
    const items = eventsByDate.get(event.date) || [];
    items.push(event);
    eventsByDate.set(event.date, items);
  });

  const grid = byId("month-grid");
  if (validMonth) {
    const first = new Date(Date.UTC(year, month - 1, 1));
    const mondayOffset = (first.getUTCDay() + 6) % 7;
    const gridStart = new Date(Date.UTC(year, month - 1, 1 - mondayOffset));
    for (let index = 0; index < 42; index += 1) {
      const current = new Date(gridStart.getTime() + index * 86400000);
      const cellYear = current.getUTCFullYear();
      const cellMonth = current.getUTCMonth() + 1;
      const cellDay = current.getUTCDate();
      const date = isoDate(cellYear, cellMonth, cellDay);
      const dayEvents = eventsByDate.get(date) || [];
      const cell = document.createElement("div");
      cell.className = `calendar-day${cellMonth === month ? "" : " is-outside"}${date === todayIso ? " is-today" : ""}`;
      cell.setAttribute("aria-label", `${cellMonth}月${cellDay}日${dayEvents.length ? `，${dayEvents.length}个日程` : ""}`);

      const number = document.createElement("strong");
      number.className = "day-number value value--xxsmall value--tnums font--bold";
      number.textContent = String(cellDay);
      cell.appendChild(number);

      if (dayEvents.length) {
        const markers = document.createElement("div");
        markers.className = "event-markers flex flex--row flex--center-y gap--xsmall";
        dayEvents.slice(0, 3).forEach(() => {
          const marker = document.createElement("i");
          marker.className = "event-marker";
          markers.appendChild(marker);
        });
        if (dayEvents.length > 3) {
          const overflow = document.createElement("span");
          overflow.className = "event-overflow";
          overflow.textContent = `+${dayEvents.length - 3}`;
          markers.appendChild(overflow);
        }
        cell.appendChild(markers);
      }
      grid.appendChild(cell);
    }
  }

  const tomorrowIso = (() => {
    if (!todayIso) return "";
    const tomorrow = new Date(Date.UTC(year, month - 1, todayDay + 1));
    return isoDate(tomorrow.getUTCFullYear(), tomorrow.getUTCMonth() + 1, tomorrow.getUTCDate());
  })();
  const agendaEvents = events
    .filter((item) => !todayIso || item.date >= todayIso)
    .sort((left, right) => `${left.date}T${left.time || "00:00"}`.localeCompare(`${right.date}T${right.time || "00:00"}`))
    .slice(0, 4);
  setText("agenda-status", calendarAvailable ? `${agendaEvents.length} 项` : "不可用");

  const agenda = byId("agenda-list");
  agendaEvents.forEach((event) => {
    const row = document.createElement("article");
    row.className = "agenda-item flex flex--row flex--center-y";
    const when = document.createElement("div");
    when.className = "agenda-when";
    const dayLabel = document.createElement("span");
    dayLabel.className = "label label--small pp-cn-min";
    dayLabel.textContent = event.date === todayIso
      ? "今天"
      : event.date === tomorrowIso
        ? "明天"
        : `${Number(event.date.slice(5, 7))}/${Number(event.date.slice(8, 10))}`;
    const timeLabel = document.createElement("strong");
    timeLabel.className = `content content--small font--bold${event.all_day ? " pp-cn-min" : ""}`;
    timeLabel.textContent = event.all_day ? "全天" : (event.time || "--:--");
    when.append(dayLabel, timeLabel);

    const copy = document.createElement("div");
    copy.className = "agenda-copy";
    const title = document.createElement("div");
    title.className = "agenda-title title title--small font--bold pp-cn";
    title.textContent = event.title || "未命名日程";
    const location = document.createElement("div");
    location.className = "agenda-location description description--base pp-cn-min";
    location.textContent = event.location || "无地点";
    copy.append(title, location);
    row.append(when, copy);
    agenda.appendChild(row);
  });

  if (!agendaEvents.length) {
    const empty = document.createElement("div");
    empty.className = "agenda-empty content content--small font--bold pp-cn";
    empty.textContent = calendarAvailable ? "近期没有日程安排" : "日程数据暂不可用";
    agenda.appendChild(empty);
  }

  window.__PHOTO_PAINTER_READY__ = true;
})();
