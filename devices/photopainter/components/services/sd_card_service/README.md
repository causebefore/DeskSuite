# `sd_card_service`

> 这是一个 Service 组件，负责把 SD 卡槽硬件事件串行收敛为自动挂载或卸载，Task 不下沉到 Device。

## 1. 定位

- 层级：Service
- 触发方：Main / Composition Root 在 `device_sd` 初始化后启动
- 主要输出：`device_sd` 中可查询的物理插卡与 FATFS 挂载状态

创建本 Service 的充分理由是：GPIO15 提供持续硬件事件源，ISR 后需要去抖，并要串行执行可能
阻塞数秒的 FATFS 挂载或卸载。该流程需要独立 Task，按架构规则由 Service 拥有。

## 2. 职责边界

负责：

- 注册并借用 `device_sd` 的 GPIO ISR 回调。
- 用 Task Notification 唤醒专用 Task，完成 50 ms 去抖。
- 根据物理插卡事实自动调用 `device_sd_mount()` 或 `device_sd_unmount()`。
- 同步停止 Task，并在停止阶段卸载文件系统。

不负责：

- SD GPIO、SPI2、FATFS 句柄和文件 I/O 实现，这些由 Device/BSP 提供。
- 运行期 I/O 故障后的重试、复位或产品降级策略。
- 决定何时启动整个产品、是否因 SD 缺失而终止启动。

## 3. 主要流程

```text
GPIO15 电平变化
    ↓ ISR 仅投递 Task Notification
Service Task 阻塞唤醒并去抖 50 ms
    ↓
读取 device_sd 状态
    ├─ 已插卡且未挂载 → 同步挂载 FATFS
    └─ 已拔卡且已挂载 → 同步卸载 FATFS
```

## 4. 依赖关系

| 方向 | 组件 | 用途 |
| --- | --- | --- |
| 调用 | `device_sd` | 读取卡状态、注册 ISR 回调、挂载和卸载 |
| 被调用 | Main / Application | 装配并控制 Service 生命周期 |

`device_sd` 与 FreeRTOS 是私有实现依赖；公共头文件只暴露 `esp_err_t` 生命周期接口，
不泄漏 Task 或信号量句柄。

## 5. 公共接口

公共头文件：[`include/sd_card_service.h`](include/sd_card_service.h)

| API | 同步性 | 作用与完成语义 |
| --- | --- | --- |
| `sd_card_service_start()` | 同步 | 收敛当前卡状态并启动插拔监测 Task |
| `sd_card_service_stop()` | 同步 | 注销 ISR 回调、等待 Task 停止并卸载 FATFS |

## 6. 状态、生命周期与并发

- 生命周期：`STOPPED → RUNNING → STOPPED`，停止失败时进入 `CLEANUP_FAILED`。
- 状态所有者：Service 控制接口写入生命周期状态；专用 Task 串行执行运行期卡状态收敛。
- Task：`sd_card_service_task.c` 创建一个阻塞等待 Task，不做周期轮询。
- 回调：GPIO ISR 回调借用持续到 `stop()`；ISR 只调用 `xTaskNotifyFromISR()`。
- 停止：先注销 ISR 回调，再通知 Task 退出并等待最多 500 ms，最后由停止方删除 Task。

## 7. 故障与恢复

- 启动时无卡属于正常状态，Service 继续监测。
- 已插卡但挂载失败时记录错误并保持未挂载，Service 本身仍可运行。
- 停止超时或卸载失败时保留清理失败状态并拒绝重新启动，可再次调用 `stop()` 收敛。
- 运行期 I/O 故障后的重试、卸载和降级策略仍是架构未决边界，本组件不自行定义。

## 8. 配置与文件

- 构建配置：无 Kconfig。
- `src/sd_card_service.c`：生命周期与挂载状态收敛。
- `src/sd_card_service_task.c`：Task、ISR 通知和同步停止。
- 持久化格式：不定义业务格式；底层只挂载现有 FAT 文件系统，不自动格式化。

## 9. 验证

- 构建：在仓库根目录执行统一脚本 `.\build_tools\dm.ps1 build`。
- 实机：启动时插卡应挂载；运行期插拔应分别记录挂载和卸载；Main 读写测试内容应一致。
- 已知缺口：当前没有自动化实机插拔测试，也未定义运行期 I/O 故障重试策略。
