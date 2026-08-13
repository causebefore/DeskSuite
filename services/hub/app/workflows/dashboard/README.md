# DeskMate Dashboard 工作流

本目录把现有天气、近期日程、邮件和 GLM 额度服务确定性地投影为 DeskMate
Dashboard schema 3。它不经过大模型，不改变现有设备协议，也不自行创建外部数据
服务实例。

## 入口与数据流

`GET /api/v1/dashboard` 从应用上下文取得 `DashboardService`，传入已认证的
`device_id` 后执行：

```text
设备请求
  → 并发查询天气、近期日程、邮件和额度
  → 对每个数据源单独执行超时与异常降级
  → 裁剪 ESP32 固定缓冲区中的 UTF-8 字段
  → 计算生成时间和下一次建议刷新时间
  → 返回 Dashboard schema 3
```

`workflow.py` 是本工作流的唯一实现文件。它复用 `app/services/` 中的服务单例和
缓存，因此不会因为 Dashboard 编排而额外创建 IMAP、CalDAV 或天气客户端。

## 状态与失败处理

- 四个数据源并行执行，整体延迟由最慢的允许超时决定。
- 单源失败只把对应数据块标记为不可用，其他数据仍正常返回。
- 邮件保持只读并优先投影未读内容；工作流不发送、删除或标记邮件。
- 刷新时间复用 Display 工作流的统一时间表计算，不维护第二套调度状态。
- 所有缓存归各数据服务所有，Dashboard 工作流自身不保存会话状态。

## 本地验证

Dashboard 测试使用假服务，不调用邮箱或其他外部 API：

```powershell
uv run pytest -q tests/test_dashboard.py tests/test_pim_wiring.py
```
