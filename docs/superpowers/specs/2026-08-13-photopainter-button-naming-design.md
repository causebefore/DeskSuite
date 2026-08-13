# PhotoPainter 按键命名整理设计

> 状态：已确认，创建于 2026-08-13。对应实现改动仅限 `devices/photopainter/`。

## 1. 背景与现状

PhotoPainter 固件有 3 个物理按键，功能与逻辑名已确认正确，无需变更：

| 逻辑 ID | GPIO | 功能 |
| --- | --- | --- |
| `DEVICE_BUTTON_LEFT` | GPIO5 | 上一张照片 |
| `DEVICE_BUTTON_RIGHT` | GPIO4 | 下一张照片 |
| `DEVICE_BUTTON_CONFIRM` | GPIO3 | 确认 / 完整内容刷新 |

按键链路在代码中跨越三层：

```text
Main / Composition Root
        ↓
Application / Service
        ↓
Device  （device_button_id_t / device_button_event_t）
        ↓
BSP     （bsp_button_id_t / bsp_button_event_t）
        ↓
Driver  （button_driver_event_t，消抖状态机生产者）
        ↓
Boards  （BOARD_BTN_*_GPIO 等静态引脚宏）
```

## 2. 问题界定

### 2.1 真正需要清理的「污染」

1. **缩写不一致**：`board.h` 使用 `BOARD_BTN_*` 的 `BTN`，而 `BTN` 不在
   [受控术语表](../../standards/c_cpp_terminology.md) 的标准缩写或受控短拼写清单内；
   同文件已存在全拼 `BOARD_BUTTON_GPIO_MASK`，同一语义两种拼写并存。
2. **过时注释**：`board.h` 中「使用原中按键硬件」「使用原右按键硬件」描述的是历史
   板型位置，与实际按键语义无关，已造成误读，应予删除。

### 2.2 明确不是污染、必须保留的三层类型

`bsp_button_id_t` / `device_button_id_t` 与 `bsp_button_event_t` / `device_button_event_t` /
`button_driver_event_t` 看起来同值重复，但它们是分层边界的必要契约，删除会破坏
[项目分层规范](../../../devices/photopainter/docs/architecture/layering.md) 的单向依赖：

- `button_driver` 在 BSP 的 `CMakeLists.txt` 中是 `PRIV_REQUIRES`，`bsp.h` 公共头文件
  不得包含 Driver 头文件（`bsp.h` 头部注释亦明确此约束）。
- `layering.md` 第 90 行：Device「不得包含具体芯片 Driver、Board 头文件」，因此
  Device 不能直接复用 `button_driver_event_t`，必须保留自身事件契约。
- `layering.md` 第 86/126 行：硬件访问单向依赖 `Device → BSP → Drivers / Boards`，
  BSP 不得反向依赖 Device，因此 BSP 不能复用 `device_button_*` 类型。

因此三层 ID/事件类型各自属于对应分层边界，值相同是「简单按键语义在相邻层间同形」的
结果，不是可删除的重复命名。

## 3. 决策

1. `board.h`：`BOARD_BTN_*` 统一改为 `BOARD_BUTTON_*`，与既有
   `BOARD_BUTTON_GPIO_MASK` 拼写一致。
2. `board.h`：删除「使用原中按键硬件」「使用原右按键硬件」两处过时注释。
3. 同步更新 `bsp_buttons.c`、`bsp_power.c` 中对上述宏的引用。
4. 保留 `bsp_button_id_t`、`device_button_id_t`、`bsp_button_event_t`、
   `device_button_event_t`、`button_driver_event_t` 三层类型及 ID 映射的
   `_Static_assert`，不改动 GPIO 与按键功能语义。

## 4. 影响面

| 文件 | 改动 |
| --- | --- |
| `devices/photopainter/components/boards/reTerminal_E1001/board.h` | 宏改名 + 注释删除 |
| `devices/photopainter/components/bsp/src/bsp_buttons.c` | 宏引用改名 |
| `devices/photopainter/components/bsp/src/bsp_power.c` | 宏引用改名 |

无 GPIO、无功能行为、无持久化语义变化；不新增术语、不创建别名或转发包装。
