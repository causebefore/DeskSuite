# PhotoPainter 显示工作流

本目录实现确定性的 PhotoPainter 显示编排。它不会调用大模型，也不会向设备暴露
天气、邮件、日历或记忆等原始业务 JSON；设备只获得集合 Manifest、PNG 预览和
最终 PPF2 帧。

## 文件职责

- `workflow.py`：按设备加锁，计算页面所需数据源并协调一次集合刷新；同时计算
  下一次绝对刷新时间。
- `context.py`：并行调用现有数据服务，将结果转换为网页模板上下文。
- `pages.py`：登记页面字段和数据源依赖，并裁剪每页可见上下文。
- `renderer.py`：内联本地样式和字体，执行 Chromium 截图、四灰阶量化、PPF2
  编码以及集合 Manifest 原子发布。

## 入口与数据流

`app/api/display.py` 提供 `/render`、`/manifest`、帧下载和预览接口。一次刷新按
以下顺序执行：

```text
请求页面集合
  → 校验页面和默认页
  → 合并页面数据源依赖
  → 获取设备独享锁
  → 并行取数并构建公共上下文
  → 按页面裁剪上下文
  → 渲染 HTML 并量化为四灰阶
  → 写入 PPF2、PNG 和原子 Manifest
```

当前登记页面包括综合简报 `demo`、今日日程 `calendar`、整月概览
`month-calendar`、天气 `weather`、月相 `moon` 和订阅阅读 `rss`。模板仍位于
`web/pages/<page-id>/`，不放入工作流目录。

## 状态与失败处理

- 刷新由设备请求驱动，不创建后台定时任务；服务端只发布下一次建议刷新时间。
- 进程内锁只负责防止同一设备并发生成互相覆盖，不作为跨进程锁。
- 渲染状态和产物存放在配置指定的运行目录，不提交到仓库。
- 新集合完整生成后才替换 Manifest；API 刷新失败时可以继续提供上一完整集合。
- 单个外部数据源的缓存和降级由对应 `app/services/` 实现，工作流不复制策略。

## 本地验证

显示测试使用临时目录和假数据源，不调用天气、邮箱或其他外部 API：

```powershell
uv run pytest -q tests/test_display.py tests/test_display_refresh_service.py
```
