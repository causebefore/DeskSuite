# `web_console_network_provider`

> `web_console_network_provider` 是无状态的共享叶子适配组件，把 Network Manager 的一次完整
> 诊断快照映射为 `web_console_service` 可装配的只读 Status Provider。

## 1. 定位

- 层级：Service 边界上的可选 Provider Adapter。
- 触发方：由产品 Application 或 Composition Root 在初始化网页控制台时显式装配。
- 主要输出：稳定的 `network` Status 分区描述符，以及每次 HTTP 读取时形成的网络运行摘要。

它单独成组件的原因是隔离可选依赖：Console Core 不需要知道 Communication，Communication
也不需要知道 Console；只有选择网络状态分区的产品才链接本组件。

## 2. 职责边界

负责：

- 提供固定、进程期有效的网络 Status Provider、字段和 Manager 状态枚举元数据。
- 每次状态读取只调用一次 `network_manager_get_diagnostics_copy()`。
- 映射 Manager 状态/错误、链路查询错误、关联/BSSID/主信道/RSSI、IPv4/网关/主 DNS、
  已保存配置和 Portal 活动事实。
- BSSID 固定使用小写冒号格式 `aa:bb:cc:dd:ee:ff`；未关联 AP 时输出空字符串。

不负责：

- 不启动、停止或重连 Network Manager，不申请产品网络租约，不切换 Portal。
- 不直接调用 `connect`，不读取或写入网络配置、NVS、密码、Token 或服务地址。
- 不注册 Network Manager 通知回调，不拥有缓存、版本、Task、Queue、Timer 或生命周期。
- 不公开 SSID。底层 SSID 是有界原始字节，首版不假定其一定是合法 UTF-8。

## 3. 主要流程

```text
产品显式取得静态 Provider
    → web_console_service 在 init_borrow() 期间复制描述符和字段元数据
    → 已认证 GET /api/status?section=network
    → Provider 一次复制 Network Manager 诊断快照
    → 固定字段映射
    → Console 校验并编码 JSON
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 公开依赖 | `web_console_service` | 使用公共 `web_console_status_provider_t` 契约 |
| 私有调用 | `network_manager` | 一次复制完整网络诊断快照 |
| 被调用 | 目标 Application / Composition Root | 显式选择并装配 Provider |

依赖只沿 `web_console_network_provider → web_console_service / network_manager` 方向存在。
`network_manager` 所在 Communication 目录不包含、链接或条件调用本组件。

## 5. 公共接口

公共头文件：
[`include/web_console_network_provider.h`](include/web_console_network_provider.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `web_console_network_provider_get_status_borrow()` | 同步 | 返回进程期有效的静态 Provider 描述符 |

返回指针不转移所有权。Console 初始化只深复制描述符和字段元数据；Provider `context` 为空，
不存在额外借用对象。

## 6. 状态、生命周期与并发

- 生命周期：无；不提供空的 `init/start/stop/deinit`。
- 状态所有者：本组件没有可变状态；Network Manager 继续拥有全部网络事实。
- Task：不创建 Task、Queue 或 Timer。
- 回调：状态读取运行于 Console HTTPD 普通 Task、Console Core 锁外；只执行一次有界诊断
  复制与局部格式化，不回调 Console。
- 版本：分区 `version` 固定为 `0`，表示底层没有适合所有链路字段的统一单调版本来源。

## 7. 故障与恢复

- 参数或 Provider 输出缓冲区不合法时返回 `ESP_ERR_INVALID_ARG`。
- Network Manager 顶层诊断读取失败时原样返回错误。
- 底层实时链路查询失败不让整个分区失效；错误保存在 `link_snapshot_error`，其余 Manager
  事实和查询失败前已经取得的 best-effort 链路字段仍然输出。未取得的后续链路字段保持
  零值或空字符串，该错误码表示本次链路快照可能不完整。
- 重试、降级、隐藏入口或设备重启等产品决策仍由 Application 负责。

## 8. 配置与文件

- 构建配置：复用 `CONFIG_WEB_CONSOLE_STATUS`。组件依赖由产品显式声明；关闭 Status 后
  不编译实现源码，其他产品也可以完全不发现本组件。
- [`web_console_network_provider.c`](web_console_network_provider.c)：固定元数据和无状态映射。
- 不持久化任何数据，不包含凭据字段。

## 9. 验证

- 静态契约：
  [`tests/check_web_console_network_provider.ps1`](tests/check_web_console_network_provider.ps1)。
- DeskMate 完整构建必须编译并链接本组件实现；关闭 Status 的裁剪构建不得编译实现源码
  或产生 Provider 实现符号。
- PhotoPainter 的 Communication-only 构建不得发现或链接本组件与 `web_console_service`。
- 编译验证不等同于浏览器、断网重连或真实设备验收。
