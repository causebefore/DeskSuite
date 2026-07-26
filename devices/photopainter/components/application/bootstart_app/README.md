# `bootstart_app`

> 启动阶段 Application：为 `app_main()` 提供分阶段的产品启动用例，封装各阶段的具体初始化、
> 回调装配、局部回滚和降级细节，但不拥有整轮顶层启动顺序。

## 1. 定位

- 层级：Application
- 触发方：`main/app_main()`
- 主要输出：已初始化或已启动的系统、设备、Service、Communication 与其他 Application
  能力，以及供顶层编排判断的 `esp_err_t` 和 `refresh_ready`

## 2. 职责边界

负责：

- 读取启动唤醒事实，初始化系统存储、系统时钟、RTC、环境、SD、显示、网络、远端日志和 OTA
  基础能力。
- 执行配网、照片播放链路、内容刷新配置、电源管理和首轮内容刷新等启动阶段子流程。
- 在每个子流程内部完成固定的回调装配、部分初始化回滚、可选能力降级和诊断记录。
- 将 `system_storage` 的网络配置读、写、清除能力适配为 Network Manager 的持久化回调并注入。
- 维护 OTA 状态画面的一次性启动恢复适配，在首次正常页面物理刷新成功后清除标记。

不负责：

- 不决定整轮启动阶段的先后顺序；顺序和关键失败分支明确保留在 `app_main()`。
- 不创建自己的 Task、Queue、Timer 或长期 Runtime，也不接管下层组件的状态所有权。
- 不替代被调用 Application、Service、Communication 或 Device 的公共生命周期与状态机。
- 不负责运行期深睡、内容刷新、照片导航或 OTA 下载事务；这些仍由对应组件拥有。

## 3. 主要流程

```text
app_main 选择下一启动阶段
    ↓
bootstart_app 执行该阶段的固定调用、回调装配和局部回滚
    ↓
同步返回结果事实或 refresh_ready
    ↓
app_main 决定继续、忽略可选故障或终止本轮启动
```

顶层顺序当前由 `app_main()` 表达为：唤醒上下文 → 系统基础 → RTC 与绝对时间门禁 → 可选环境和
SD → 关键显示/网络/日志/OTA → 配网并启动日志上传 → 照片与内容链路 → 反馈设备 → 电源管理 →
首轮内容刷新。

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 被调用 | `main` | 保留顶层顺序并调用各启动阶段 API |
| 调用 | `provisioning_app`、`photo_playback_app`、`content_refresh_app`、`power_management_app` | 执行启动期产品用例并连接回调 |
| 调用 | 显示、集合、按键、环境与 SD Service | 装配持续执行或事务能力 |
| 调用 | `firmware_ota`、`network_manager`、`remote_log` | 初始化通信、远端诊断和 OTA 事务能力 |
| 调用 | System 与 Device 公共 API | 初始化持久化、时间和硬件能力 |

所有依赖均为单向调用；本组件不允许任何被调用 Application 反向依赖 `bootstart_app`。
公共头文件只暴露 C ABI 的启动阶段结果与唤醒上下文，不泄漏下层句柄。

## 5. 公共接口

公共头文件：[`include/bootstart_app.h`](include/bootstart_app.h)

| API 组 | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `bootstart_app_get_wakeup_context_copy()` | 同步 | 复制本轮唤醒事实，底层读取错误保存在输出结构中 |
| `bootstart_app_init_*()` / `bootstart_app_start_*()` | 同步 | 返回时相应启动阶段已经完成或已经回滚其局部资源 |
| `bootstart_app_run_provisioning()` | 同步阻塞 | 返回 `ESP_OK` 时网络已经在线 |
| `bootstart_app_reject_pending_image_on_fatal_error()` | 同步请求 | 待验证镜像遇到致命启动错误时请求 OTA 回滚重启 |

精确参数、错误码、可选降级和输出有效性以公共头文件 Doxygen 为准。

## 6. 状态、生命周期与并发

- 生命周期：本组件没有独立 `init/start/stop/deinit`；每个公共函数代表一个由 `app_main()`
  单次调用的启动阶段。
- 状态所有者：不持有长期可变状态；唤醒上下文由值语义结构在 `app_main()` 栈上传递。
- Task：本组件不创建 Task；被调用组件按各自契约创建和拥有 Task。
- 回调：OTA 强制重启和照片首次成功呈现回调均为文件内静态函数，由对应下层组件按借用契约保存。
- 网络配置持久化回调为文件内静态函数，描述符使用空上下文；Network Manager 复制描述符后长期
  保存回调函数指针，回调由 `network_manager_task` 串行调用。

## 7. 故障与恢复

- 系统基础或关键通信失败由函数返回给 `app_main()`，顶层决定终止，并可请求待验证镜像回滚。
- 环境、SD、RTC、反馈设备和电源管理当前属于顶层允许忽略返回值的可选阶段；忽略动作明确写在
  `app_main()`，本组件只执行阶段内部的固定回滚。
- 内容刷新初始化失败时保留本地照片播放；首轮启动失败时回滚内容刷新并向电源管理提交退避
  休眠事实。
- 远端日志初始化、配置或启动失败只记录告警并保留串口日志，不阻断设备主流程。
- OTA 状态画面恢复标记由运行期电源管理流程设置；启动编排只读取该事实，并在正常页面物理
  刷新成功后清除，失败则保留到后续刷新或下次启动。

## 8. 配置与文件

- 构建配置：无独立 Kconfig；HTTP、OTA 和清醒窗口参数当前为组件内产品常量。
- 远端日志在 Network Manager 初始化后启用 Log V2 捕获；配网成功后读取已生效的
  `service_url`，使用默认设备 ID `default` 启动上传。
- `bootstart_app.c`：全部启动阶段实现和私有回调适配。
- `include/bootstart_app.h`：供 `main` 使用的分阶段 C ABI。
- 持久化：网络配置通过本组件注入的适配回调映射到 `system_storage`；Network Manager 不直接
  依赖 System。一次性画面恢复标记仍直接使用 `system_storage`；旧设备遗留的 `ota_dev_mode`
  键不再读取或迁移。

## 9. 验证

- 静态检查 `main.c` 只包含 `app_main()`，且顶层顺序与迁移前一致。
- 检查每个公共声明都有唯一实现，`main` 只直接依赖 `bootstart_app`。
- 检查 `bootstart_app` 不创建 Task，不包含 BSP、Driver 或 Board 头文件，依赖图保持无环。
- 检查 `network_manager` 不包含 `system_storage`/`sys.h`，网络配置只经注入回调访问持久化实现。
- 实机应覆盖冷启动、按键/定时器唤醒、无网络配置配网、OTA 待验证镜像、无 SD、内容刷新失败
  和 OTA 提示画面恢复。
- 当前没有该组件的自动化测试；按仓库约束，本次迁移不主动执行项目构建。
