# Presentation

> Presentation 层把不可变产品事实转换为 UI 可直接消费的 View Model 和呈现事件。

## 1. 定位

- 上游：Application 传入的呈现状态，以及 Service、Communication、Data、System 发布的事实。
- 下游：UI Runtime 和各 LVGL 页面。
- 当前构建：本目录仍属于 `main` ESP-IDF 组件。

## 2. 职责边界

负责：

- 订阅事实事件并通过所有者 API 复制有界快照。
- 执行字段裁剪、枚举映射、单位转换和显示文本格式化。
- 维护页面级只读 View Model，并提供 `*_get_view_copy()`。
- 定义共享页面契约和 `PRESENTATION_EVENT` 类型化呈现事件。

不负责：

- 不解释按键或触摸的产品语义。
- 不启动 Portal、OTA、语音、网络会话或设备操作。
- 不决定重试、降级、重启、生命周期和资源释放。
- 不创建 Task、Queue 或 Timer，不包含 Application 或 UI 头文件。

## 3. 主要流程

```text
Application 或下层不可变事实
    → <feature>_presenter 更新 View Model
    → presentation_dispatch 发布轻量事件
    → UI Runtime 接收事件
    → UI 调用 <feature>_presenter_get_view_copy()
    → LVGL 渲染
```

大型页面数据不随事件复制；事件只表达页面切换、“哪类视图需要刷新”或一条按值设置动作。

## 4. 依赖关系

| 方向 | 层级 | 用途 |
| --- | --- | --- |
| 调用 | Service / Communication / Data / System | 读取稳定事实事件与快照 |
| 被调用 | Application | 更新 Presenter 状态或发布呈现事件 |
| 被调用 | UI | 读取 View Model 和页面呈现契约 |

依赖必须保持 `Application → Presentation ← UI`。Presentation 不反向包含这两个层级的头文件，
从而避免 Application、Presentation、UI 形成编译环。

## 5. 命名与文件

`pomodoro_presenter` 接收 Application 按值推送的完整番茄钟事实，生成阶段、倒计时、今日计数、
预计结束、轮次和按键提示文本，并同步更新状态栏角标事实。它不读取 Application、NVS 或系统
时钟；运行/暂停/完成三态只映射到本地静态图标。`completion_latched` 与非零
`completion_generation` 保留在 View Model 中，供 UI Runtime 恢复时补画一次全局完成提示。
独立的 `settings_version` 也按值保留，供设置页创建带版本草稿；Presenter 不自行生成或推进
领域版本。最新设置请求的有效位、请求 ID、状态、结果版本和错误也作为一组完整事实复制，
设置页只把与本机提交 ID 匹配的 `SUCCEEDED/FAILED` 解释为终态。

- Presenter：`<feature>_presenter.[ch]`
- View Model 类型：`<feature>_view_model_t`
- 公共 View Model：[`presentation_view_model.h`](presentation_view_model.h)
- 页面契约：[`presentation_page.h`](presentation_page.h)
- 呈现事件：[`presentation_dispatch.h`](presentation_dispatch.h)

当前 Presenter 包括首页、天气、日历、邮件、限额、设置、系统、语音、状态栏、OTA 和网页
文件管理。
`settings_presenter` 聚合网络阶段、SSID、IP、RSSI、Portal 与当前版本；`system_presenter`
只提供版本、构建时间、运行时长、SRAM、PSRAM 和 CPU 频率，不再复制网络字段。
`ota_presenter` 保存带锁快照，并以检查中、无更新、有更新、下载中、检查失败和安装失败表达
设置子页状态。旧的全局 OTA 弹层状态和百分比载荷已删除。

`web_console_presenter` 只接收 Application 主动推送的 `web_console_presenter_input_t` 纯展示事实，
把独立 Presenter 状态映射为固定中文标题，并把字节容量格式化为 B/KiB/MiB/GiB。它只在
`RUNNING` 时把本地 URL 和六位访问码保存到有界 View Model，不读取或持有浏览器 Bearer
token，也不读取 Application、Service、Network 或文件系统。是否允许退出由 Application
根据 Service/租约所有权计算后按值传入，Presenter 不重复产品判断。每份输入还携带
Application 分配的严格单调 64 位展示版本；Presenter 只接受晚于当前已应用版本的输入，
相同或更旧的并发到达快照不会覆盖新 View Model。首次初始化把已应用版本设为 0，重复初始化
保留当前 View Model 和版本；版本 0 非法，`UINT64_MAX` 后禁止回绕。唯一写入方
`app_web_console` 还用私有静态互斥量串行化版本仲裁和事件入队，避免已接受版本在派发前被更高
版本覆盖。
Application 只会在 Service `RUNNING` 快照包含合法六位访问码后一次性发布完整运行事实；
后续 Network Manager 断线、重连或 IPv4 变化只替换 URL 并保留访问码。Presenter 不补查或
猜测这些字段，UI 因此读取到的运行信息始终来自 Application 已收敛快照。
`app_network` 的链路变化借用回调只唤醒 Application 所有的 `app_web_console_task`，不会调用
Presenter；Presentation 看到的仍然只是 Application 重新读取并收敛后的不可变完整事实。

`web_console_view_model_t.title` 使用 32 字节：精确标题“网页控制台已开启”的 UTF-8 正文为
27 字节，加 NUL 需要 28 字节；原计划的 24 字节会截断多字节字符。

网页控制台呈现链路为：

```text
设备设置页选择“网页控制台”
  → app_web_console 检查 /sdcard
  → app_network 授予 APP_NETWORK_LEASE_WEB_CONSOLE
  → web_console_service 恢复事务并启动 HTTPD
  → 浏览器用 6 位访问码换取 Bearer token
  → handler 提供 Files、Settings 与 Status 模块
  → 设备返回时 Service 安全停止后释放网络租约
```

在这条产品流程中，Application 每次状态迁移或 URL 变化都映射并调用
`web_console_presenter_update_copy()`；Presenter 按展示版本原子替换完整 View Model，只有新版本
被接受后才发布轻量刷新事件，UI Runtime 随后调用 `web_console_presenter_get_view_copy()` 并在
LVGL 上渲染。Presentation 不复刻浏览器的 Files、Settings 或 Status 表格；这些 HTTP 交互由
Service 与产品 Provider 直接完成，不产生新的设备端 View Model 或事件。

## 6. 并发与所有权

- 每份 View Model 只有对应 Presenter 写入。
- `status_bar_presenter` 的页面、电池和服务可达事实可能来自不同 Task，使用短临界区保证 Getter
  复制到完整快照；查询下层快照和发布时间转换均在锁外执行。
- `ota_presenter` 使用短临界区原子替换完整 OTA 快照；事件只通知 UI 重新读取，不携带目标指针。
- `web_console_presenter` 只拥有当前有界 View Model 和已应用展示版本；访问码仅供本地页面读取，
  更新输入和 Getter 都按值复制，不保存输入指针。
- 事件载荷只在回调期间有效；UI 不保存事件指针。
- Presenter 回调只做有界内存转换和事件发布，不执行阻塞 I/O。
- UI 停止期间允许刷新通知合并；Presenter 保留最新 View Model，UI 恢复后重新拉取。

## 7. 验证

- 静态核对本目录不包含 `app_*.h` 或 `ui_*.h`。
- 静态核对 UI 只通过 `*_presenter_get_view_copy()` 读取页面数据。
- 本轮未运行测试、网页生成器、固件编译或视觉实机检查；后续编译需通过统一命令
  `& .\ds.ps1 build deskmate`，视觉验收需在目标硬件完成。

相关规范：

- [`../../docs/architecture/layering.md`](../../docs/architecture/layering.md)
- [`../../docs/architecture/data_flow.md`](../../docs/architecture/data_flow.md)
