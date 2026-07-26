(() => {
  const data = window.__PHOTO_PAINTER_DATA__ || {};
  const rss = data.rss || {};
  const available = (data.availability || {}).rss !== false;
  const articles = available && Array.isArray(rss.items) ? rss.items.slice(0, 4) : [];
  const sourceCount = Number(rss.source_count || 0);
  const updatedText = rss.updated_text || "--";

  document.getElementById("updated-text").textContent = updatedText;
  document.getElementById("latest-text").textContent = `最近文章 ${updatedText}`;
  document.getElementById("source-count").textContent = `${sourceCount} 个订阅源 · 每小时刷新`;
  document.getElementById("feed-status").textContent = !available
    ? "数据不可用"
    : (rss.stale ? "缓存数据" : "RSS");

  const storyList = document.getElementById("story-list");
  articles.forEach((story, index) => {
    const article = document.createElement("article");
    article.className = "story";
    const number = document.createElement("span");
    number.className = "story-index value value--small value--tnums font--bold";
    number.textContent = String(index + 1).padStart(2, "0");
    const copy = document.createElement("div");
    copy.className = "story-copy";
    const title = document.createElement("div");
    title.className = "story-title title title--small font--bold pp-cn";
    title.textContent = story.title || "无标题文章";
    const source = document.createElement("div");
    source.className = "story-source description description--small pp-cn-min";
    source.textContent = story.source || "未知来源";
    const time = document.createElement("time");
    time.className = "story-time content content--small font--bold pp-cn-min";
    time.textContent = story.published_text || "--";
    copy.append(title, source);
    article.append(number, copy, time);
    storyList.appendChild(article);
  });

  if (!articles.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state content content--small font--bold pp-cn";
    empty.textContent = available ? "订阅源暂时没有文章" : "RSS 数据暂时不可用";
    storyList.appendChild(empty);
  }

  window.__PHOTO_PAINTER_READY__ = true;
})();
