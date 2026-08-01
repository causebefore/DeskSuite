# `content_refresh_app`

> 按产品计划执行网络会话、SNTP 校时、设备状态上传、显示集合刷新与错误退避。

## 1. 定位与职责

- 层级：Application
- 状态所有者：刷新时机、连续失败次数、下一轮期限和内容网络会话
- 执行资源：一个 FreeRTOS Task、停止握手信号量和 Task Notification

负责启动后立即刷新、按需取得 Network Manager 会话、校时、上传状态、同步照片集合、停止网络，
以及 1/5/15 分钟短期退避和长期失败 RTC 整点计划。不负责固件 OTA、按键语义、照片导航或
墨水屏刷新。

## 2. 主要流程

```text
启动、确认键手动请求或计划到期
    → 注册网络状态快速通知
    → 接管 ONLINE 会话或启动并等待 ONLINE
    → 启动或保持 remote_log 上传 Task
    → system_clock_sync_from_sntp（成功后回写设备 RTC）
    → device_status_upload_app 单次上传（失败不阻断照片同步）
    → display_collection_service_sync
    → remote_log_stop
    → network_manager_stop
    → 成功按 next_refresh_at 等待；失败前三次按 1/5/15 分钟退避
    → 继续失败时按可信 RTC 等待到下一本地整点，时间不可信则退回相对 60 分钟
```

手动刷新和停止使用 Notification 位，不创建命令队列。多次手动请求自动合并；完整刷新执行中
收到的请求并入当前轮次，调度等待中收到的请求提前开始新一轮。正常内容刷新不检查固件，也不
为了 OTA 延长在线会话。

每轮在 `network_manager_stop()` 成功或明确失败后，通过借用回调发布轮次结果、当前
`next_refresh_at` 和活动集合 `generation`，供电源协调 App 等待显示收敛。

## 3. 生命周期、停止与恢复

- `init → start → stop → deinit`，成功停止后允许再次启动。
- Task 默认优先级 3、栈 6144 字节、不绑核。
- Manifest 请求最多阻塞配置的 HTTP 超时；帧下载在数据回调边界检查停止标记。
- `stop()` 最长等待 HTTP 超时与两次 SNTP 样本超时中的较大值，再加 10 秒清理余量。
- 关闭 Network Manager 前以 10 秒上限同步停止远端日志，避免日志 HTTP 与 Wi-Fi Driver
  释放并发执行。
- Network Manager 未完成清理时进入 `CLEANUP_FAILED`，拒绝重启和反初始化。
- 普通网络、协议、SD 错误保留旧集合并进入退避，不停止 Task；长期失败时运行态等待也会
  对齐可信 RTC 的下一本地整点。

## 4. 配置与所有权

`init()` 将完整 `protocol_backend_context_t` 按值复制到 Runtime；状态上传、显示 Manifest 和
帧下载都借用这同一份 URL、Token、产品、固件目标与稳定设备身份。
刷新目标只来自 Manifest v3 必填的 `next_refresh_at` UTC Unix 时间戳；缺失或非法响应按协议
错误处理。SNTP 服务器和单样本超时由项目 Kconfig 配置，底层单次网络取样由共享
`time_sync` 执行；候选可信度、二次确认和 RTC 回写仍由 `system_clock` 决定。

## 5. 依赖与验证

调用 `network_manager`、`remote_log`、`system_clock`、`device_status_upload_app` 和
`display_collection_service`。应验证首次会话复用、SNTP 超时、RTC 回写、手动完整刷新、
1/5/15 分钟短期退避、长期失败 RTC 整点计划及时间不可信兜底、下载取消、轮次完成回调位于
状态锁外，以及网络清理失败。公共 API 保持 C ABI。
