# Graphics 组件边界

`components/graphics` 是图形机制能力的分组目录，不注册聚合 ESP-IDF Component。当前只有叶子组件 `ui_platform`。

## 允许职责

- `ui_platform` 适配 LVGL port、显示 flush、按需/连续刷新机制和 LVGL lock。
- 管理字体分区、字体缓存、fallback 和静态图片 catalog。
- 通过 `device_display` 执行 framebuffer 写入和异步 flush，不直接依赖 BSP。
- 向 `main/ui` 提供无业务语义的 `ui_platform_*` 同步 API。

## 禁止边界

- 不依赖 Application、Presentation、页面实现、Service Core 或产品功耗策略。
- 不创建 DeskMate 产品 Task、Command Queue 或业务 Timer。
- 不解释页面 ID、网络状态、按键、天气、电池或语音业务。
- 不反向调用 `main/application`、`main/presentation` 或 `main/ui`。

## 三类显示执行上下文

| 上下文 | 所有者 | 当前配置 | 职责 |
|---|---|---|---|
| 产品 `ui_runtime_task` | `main/ui/core/ui_task.c` | 优先级 3，内部 SRAM 栈由 `CONFIG_DESKMATE_UI_TASK_STACK_SIZE` 配置 | 消费 Presentation 事件和 UI 私有消息、读取 View Model、串行修改控件并拥有控制握手 |
| LVGL port Task | `esp_lvgl_port` | 优先级 4，CPU affinity 0 | 执行 LVGL timer/handler、按需帧与动画帧渲染；由 `lvgl_port_init()` 创建，属于第三方框架 Task |
| `display_flush_task` | `bsp_display`（经 `device_display` 间接使用） | 优先级 5，栈 4096 bytes | 执行 RLCD framebuffer、DMA、TE 和 SPI flush；属于当前保留的硬件执行 Task |

后两类 Task 不是隐藏的产品业务 Task。产品 `ui_runtime_task` 只负责 UI Runtime 命令与生命周期，
不能替代 LVGL port 或 BSP flush worker。

## 生命周期所有权

`ui_runtime_task` 是完整 UI Runtime 的唯一产品所有者：

```text
start: ui_platform_lvgl_init -> ui_platform_font_init -> 持锁 ui_main_init -> READY
stop:  关闭 UI 入口 -> 停止 LVGL tick -> 等待显示 DMA -> GPIO hold -> STOPPED
start: 重同步控件树 -> 解除 GPIO hold -> 同步完整刷新 -> 恢复 LVGL tick -> RUNNING
deinit: 持锁 ui_main_deinit -> ui_platform_font_deinit -> ui_platform_lvgl_deinit -> UNINIT
```

`ui_platform` 只实现上述同步机制。其 render timer 收到刷新请求后立即在 `taskLVGL` 上提交脏区；
存在 `lv_anim` 时按 45ms 连续运行，动画结束后自动暂停。可逆 `stop()` 使用
`lvgl_port_stop()` 停止 tick timer，保留 port Task、显示对象和缓冲区；随后在仍持有 LVGL
锁时停止显示入口并等待 BSP DMA 静止。`start()` 在控件树重同步后先同步提交完整画面并等待
显示 DMA 完成，再恢复 tick timer；因此返回 `ESP_OK` 就代表维护画面已经实际刷新。

完整 `deinit()` 中，`esp_lvgl_port` 的 `lvgl_port_deinit()` 只发出异步停止请求，因此平台必须
在 port 同步对象仍有效时先唤醒 `taskLVGL`，再提交停止并有界等待它真正退出；停止请求发出后
不得再访问可能已被 Task 清理的 port 同步对象。LVGL port 启动后若 lock、display、draw buffer
或 render timer 创建失败，也进入同一回滚屏障，不能遗留框架 Task。UI 子模块若保存跨 Runtime
的 LVGL 句柄、Timer 或 Style，必须由 `ui_main_deinit()` 在控件树销毁时重置；平台组件不反向
感知这些业务对象。

字体运行时通过 `esp_partition_mmap()` / `spi_flash_munmap()` 管理 Flash 映射，这两个路径可能冻结
外部 Cache。由于初始化和反初始化均由 `ui_runtime_task` 同步执行，其 Task 栈必须位于内部
SRAM，禁止改回 `MALLOC_CAP_SPIRAM`。大容量 LVGL draw buffer 仍放在 PSRAM，字体内容仍直接
使用 Flash 映射，不因此复制到内部 SRAM。

## 目录与公开 API

- `ui_platform/include/ui_platform_lvgl.h`：LVGL 生命周期、锁和按需/连续刷新机制。
- `ui_platform/include/ui_platform_font.h`：字体初始化、状态和字体查询。
- `ui_platform/include/ui_platform_image.h`：静态图片 catalog 和查询。
- 组件名保持 `ui_platform`，调用方无需感知分组目录。
