# 嵌入式 C/C++ 受控术语表

> 状态：审阅草案，创建于 2026-07-28。适用于 DeskSuite 全仓及其他采用本术语表的嵌入式项目。
>
> 本文是[嵌入式 C/C++ 术语与命名规范](c_cpp_naming_conventions.md)的唯一术语来源。主要
> 管理公共 API、跨文件接口、生命周期、并发、异步、所有权和数据形态中的英文词。

## 1. 使用原则

- 一个语义只保留一个首选英文词。
- 一个英文词只承担本文声明的含义。
- 先选择语义，再按命名规范组成标识符。
- 禁止通过缩写、时态变化或近义词绕过术语表。
- 外部协议、SDK 和芯片手册已经定义的名称保持原样，不反向改变项目术语。

术语表没有列出所有领域对象。只在单一模块内使用的 `temperature`、`frame`、`alarm` 等具体
对象可以直接使用；一旦某个新词成为跨模块契约、公共 API 动作或可复用架构概念，就必须加入
本文。

## 2. 新增术语流程

当现有术语不能准确表达新语义时，必须按以下顺序处理：

1. 搜索本文的首选词、禁用近义词和中文固定译法。
2. 判断能否通过“现有动作词 + 更具体对象”表达，不能只因名称较长就创造缩写。
3. 确认现有词确实无法覆盖后，在使用该词的同一任务中先修改本文。
4. 新条目必须包含英文词、类别、中文固定译法、唯一含义、适用形式和边界。
5. 如果新词与现有词接近，必须说明为何不能复用现有词。
6. 在本次任务的用户可见更新和最终回复中告知新增理由，不能静默加入。

告知格式：

```text
新增术语：<term>
理由：<现有术语为什么不能准确表达>
边界：<允许用于什么，不允许替代什么>
示例：<一个推荐标识符>
```

不需要新增术语的情况：

- 只引用未修改的外部 API、协议字段或寄存器名。
- 只在短函数内部使用且不会形成公共语义的局部变量。
- 使用本文已有词构成更具体的对象名。

如果新增术语会改变多个公共 API 的含义或替代已经广泛使用的词，必须先作为术语决策提交用户
审阅，再执行代码迁移。

## 3. 生命周期与资源动作

| 首选词 | 中文固定译法 | 唯一含义 | 禁止混用 |
| --- | --- | --- | --- |
| `init` | 初始化 | 创建模块固定资源并建立可调用状态，不隐含开始持续工作 | `create`、`start` |
| `deinit` | 反初始化 | 释放由 `init` 创建的固定资源 | `destroy`、`stop` |
| `start` | 启动 | 从已初始化状态进入运行态；成功返回时已经运行 | `init`、`request_start` |
| `stop` | 停止 | 停止运行并等待声明的终态；成功返回时已经停止 | `request_stop` |
| `create` | 创建对象 | 创建独立对象、句柄或 Task 并返回所有权 | `init`、`open` |
| `destroy` | 销毁对象 | 销毁由 `create` 产生的独占对象 | `deinit`、`close` |
| `open` | 打开 | 打开文件、流、设备会话或协议会话 | `create`、`start` |
| `close` | 关闭 | 关闭由 `open` 建立的文件、流或会话 | `destroy`、`stop` |
| `reset` | 复位内容 | 保留对象和生命周期资源，把可变内容恢复到初始值 | `deinit`、`clear` |
| `clear` | 清除 | 清除标志、缓存项、告警或已保存内容 | `reset`、`delete` |
| `recover` | 恢复 | 从已知失败状态修复资源或事务 | `retry`、`reset` |
| `reconcile` | 收敛 | 根据外部事实重新计算，使本地状态与事实一致 | `refresh`、`recover` |
| `retry` | 重试 | 在契约允许时再次执行同一失败操作 | `recover` |

规则：

- `stop()` 是同步完成操作；非阻塞版本必须使用 `request_stop()`。
- `init/deinit` 面向模块固定生命周期，`create/destroy` 面向独立对象。
- `open/close` 面向可关闭会话，不用于普通内存对象。

## 4. 数据动作

| 首选词 | 中文固定译法 | 唯一含义 | 禁止混用 |
| --- | --- | --- | --- |
| `get` | 获取缓存值 | 从内存或轻量状态读取，不主动执行设备、文件或总线 I/O | `read`、`query` |
| `read` | 读取 I/O | 从设备、寄存器、文件、流或总线执行输入 I/O | `get` |
| `write` | 写入 I/O | 向设备、寄存器、文件、流或总线执行输出 I/O | `set`、`update` |
| `set` | 设置属性 | 设置一个明确属性或配置值 | `update`、`write` |
| `update` | 更新已有数据 | 使用调用方提供的数据更新所有者状态，不重新读取来源 | `refresh` |
| `refresh` | 刷新来源 | 重新读取权威来源并替换或推进本地快照 | `sync`、`update` |
| `synchronize` | 双向同步 | 对齐两个都可能变化的数据源，解决双方差异 | `refresh` |
| `apply` | 应用模型 | 把完整输入模型映射到现有对象 | `update` |
| `load` | 载入 | 从持久化介质载入结构 | `get`、`read` |
| `save` | 保存 | 把结构写入持久化介质 | `set`、`write` |
| `copy` | 复制 | 创建与来源无共享生命周期的值副本 | `borrow` |
| `format` | 格式化 | 把已有值转换为文本或指定表示，不改变来源 | `serialize` |
| `parse` | 解析 | 把外部表示解析为结构化值 | `load` |
| `encode` | 编码 | 按协议或格式把结构化值编码为字节或文本 | `format` |
| `decode` | 解码 | 按协议或格式把字节或文本解码为结构化值 | `parse` |
| `validate` | 校验 | 检查输入是否满足明确约束，不修改输入 | `check` |
| `measure` | 测量 | 从硬件或算法获得物理量或尺寸 | `get` |
| `sample` | 采样 | 取得一个时点的传感器或信号样本 | `read`、`measure` |

`sync` 只允许作为已经被平台或领域广泛固定的缩写，例如 `clock_sync`；新公共 API 优先拼写
`synchronize`。不能使用 `_sync` 表示阻塞版本。

## 5. 异步、并发与调用边界

| 首选词 | 中文固定译法 | 唯一含义 | 禁止混用 |
| --- | --- | --- | --- |
| `intent` | 用户意图 | 用户或 UI 表达的期望动作，尚未表示所有者接受 | `command`、`event` |
| `request` | 请求 | 向异步所有者提交操作；返回只表示是否接受 | `command`、`event` |
| `command` | 命令 | 已接受、等待唯一所有者执行的内部消息 | `request`、`event` |
| `event` | 事件 / 事实 | 已经发生且不可变的事实 | `intent`、`command` |
| `notification` | 唤醒通知 | 允许合并的轻量提示；消费者必须重读事实 | `event` |
| `callback` | 回调 | 被调用方反向调用调用方的函数机制 | `event`、`listener` |
| `dispatch` | 分发 | 路由已经构造好的类型化消息或意图 | `publish`、`handle` |
| `publish` | 发布事实 | 由事实所有者对外发布不可变事件 | `dispatch`、`emit` |
| `wait` | 等待完成 | 有界阻塞直到已提交操作完成或超时 | `poll` |
| `cancel` | 取消请求 | 请求取消尚未完成的操作 | `stop`、`clear` |
| `register` | 注册 | 向集合或框架加入回调、处理器或对象 | `set` |
| `unregister` | 注销注册 | 移除由 `register` 加入的对象 | `clear` |
| `handler` | 框架处理器 | 满足协议、HTTP、ISR 桥接或 SDK 入口签名的适配函数 | 业务 `handle()` |
| `task` | RTOS Task | RTOS 定义的长期执行上下文 | `worker`、`thread` |
| `timer` | 定时器 | 平台或 RTOS 定时触发资源 | `task` |
| `queue` | 队列 | 具有明确容量和满策略的消息队列 | `buffer` |
| `mutex` | 互斥量 | 保护共享状态互斥访问的同步原语 | `semaphore` |
| `semaphore` | 信号量 | 表达计数、完成或资源可用性的同步原语 | `mutex` |

补充：

- 函数指针类型使用 `_callback_t`，不使用 `_cb_t`、`_listener_t`。
- 平台正式使用 Thread 而非 Task 时保留 `thread`；同一项目不混用。
- `emit`、`notify`、`send` 不能作为 `publish/dispatch/request` 的随意近义词。
- `process`、`execute`、`perform`、`do_work` 不进入公共 API；应选择实际动作。

## 6. 状态与数据形态

### 6.1 强制判定顺序

1. 表达一次调用的完成输出或错误：`result`。
2. 表达所有者内部长期可变资源和工作数据：`runtime`。
3. 只表达当前唯一离散阶段：`state`。
4. 回答组件能否工作、正在做什么或为何失败：`status`。
5. 回答某一时点领域值或硬件事实是什么：`snapshot`。
6. 表达初始化后基本稳定的能力属性：`info`。

### 6.2 术语表

| 首选词 | 中文固定译法 | 允许形态 | 唯一含义 |
| --- | --- | --- | --- |
| `state` | 状态 / 阶段 | 单一 `enum` | 生命周期或状态机当前唯一离散阶段 |
| `phase` | 子阶段 | 单一 `enum` | 更大状态机内部的业务阶段 |
| `status` | 运行摘要 | 有界只读 `struct` | 组件是否可用、正在做什么及最近失败 |
| `snapshot` | 数据快照 | 有界只读 `struct` | 某一时点的领域、设备或寄存器数据副本 |
| `result` | 操作结果 | 值、`enum` 或有界 `struct` | 一次操作的最终输出或完成原因 |
| `runtime` | 运行时 | 私有 `struct/class` | 同一生命周期内资源和可变工作数据的所有者 |
| `info` | 能力信息 | 有界只读 `struct` | 初始化后基本稳定的能力或静态属性 |
| `config` | 初始化配置 | 只读输入 `struct` | 创建或初始化资源所需的技术参数 |
| `settings` | 用户设置 | 可持久化值 `struct` | 用户可修改的产品偏好 |

结构规则：

```text
<module>_state_t       → enum
<module>_status_t      → struct + 可选 state/active/pending/last_error
<module>_snapshot_t    → struct + 领域值/时间戳/版本/质量字段
<module>_runtime_t     → 私有资源所有者，不整体暴露
<operation>_result_t   → 单次操作输出
```

`status` 在实现上也是某一时点副本，但它的首要语义是运行情况，因此使用
`get_status_copy()`，不创建 `status_snapshot`。

`snapshot` 可以包含 `valid`、`updated_at_ms` 和 `last_error`；这些字段描述数据质量，不会
使它变成 `status`。缓存快照使用 `get_<object>_snapshot_copy()`，直接 I/O 使用
`read_<object>_snapshot()`。

快速判断：

```text
“处于哪个阶段？”                         → state
“组件能否工作、为何失败？”                 → status
“当前数据或硬件事实是什么？”               → snapshot
“本次调用最终发生了什么？”                 → result
“谁持有句柄、锁、Task 和工作数据？”        → runtime
```

## 7. 所有权与生命周期

| 首选词 | 中文固定译法 | 唯一含义 | 禁止混用 |
| --- | --- | --- | --- |
| `copy` | 副本 | 返回前完成复制，之后无共享生命周期 | `clone`、`duplicate` |
| `borrow` | 借用 | 不转移所有权，对象只在声明的借用期内有效 | `reference`、`peek` |
| `take` | 接管 | 成功时所有权转移，失败时仍归调用方 | `move`、`owned` |
| `acquire` | 取得使用权 | 取得租约、引用或共享资源使用权 | `get` |
| `release` | 释放使用权 | 释放由 `acquire` 取得的使用权 | `free`、`destroy` |
| `owner` | 所有者 | 对资源释放和可变状态写入负最终责任的唯一主体 | `manager` |
| `handle` | 不透明句柄 | 标识由其他层拥有的对象，不暴露内部结构 | `context`、`pointer` |
| `context` | 上下文 | 回调定位信息或共同生命周期依赖集合 | `handle`、`runtime` |
| `lease` | 租约 | 带身份或代次、必须显式释放的临时使用权 | `handle` |
| `buffer` | 缓冲区 | 具有明确容量、元素类型和所有权的连续存储 | `queue`、`data` |

禁止新增 `_owned`、`_move`、`_share`、`_peek`、`_raw` 等所有权后缀。

## 8. 顺序、身份与版本

| 首选词 | 中文固定译法 | 唯一含义 |
| --- | --- | --- |
| `generation` | 代次 | 区分资源或租约的新旧持有者，防止旧句柄重新匹配 |
| `version` | 版本 | 判断快照、配置或模型的新旧版本 |
| `sequence` | 序号 | 表达请求、事件或消息的严格顺序 |
| `index` | 索引 | 容器中的位置 |
| `count` | 数量 | 当前有效元素数量 |
| `capacity` | 容量 | 容器最多可容纳的元素数量 |
| `size_bytes` | 字节大小 | 存储空间或载荷的字节数 |
| `length` | 长度 | 字符、样本或协议元素长度；需要时补充单位 |

`generation`、`version` 和 `sequence` 不能因底层类型相同而互换。

## 9. 缩写白名单

公共名称只使用项目成员无需额外查表即可识别的标准缩写：

```text
API, ABI, ADC, BSP, CPU, DMA, DNS, GPIO, HAL, HTTP, I2C, ID, IP, ISR,
JSON, NVS, OTA, PWM, RAM, RTC, SDK, SPI, TCP, TLS, UART, UI, URI, URL,
USB, UTC, Wi-Fi
```

标识符中按所在命名形式整体小写，例如 `http_handler`、`device_id`、`uart_config`。

禁用短写：

- `cb` → `callback`
- `ctx` → `context`
- `cfg` → `config`
- `msg` → 跨文件接口使用 `message`；极短私有队列类型可按项目例外
- `num` → `count`、`index`、`value` 或具体名词
- `len` → 公共接口使用 `length` 或带单位名称；短局部变量可保留
- `buf` → 公共接口使用 `buffer`；短局部变量可保留

## 10. 中文固定表达

| 中文表达 | 对应语义 |
| --- | --- |
| 同步操作 | 函数返回时操作已经达到最终结果 |
| 异步请求 | 函数返回只表示请求是否接受 |
| 停止操作 | `stop()`；成功返回时已经停止 |
| 停止请求 | `request_stop()`；返回时可能仍在运行 |
| 状态 | 单一离散 `state` |
| 运行摘要 | 组件 `status` |
| 数据快照 | 领域或硬件 `snapshot` |
| 操作结果 | 单次调用 `result` |
| 状态收敛 | `reconcile`，根据外部事实更新本地状态 |
| 数据刷新 | `refresh`，重新读取权威来源 |
| 双向同步 | `synchronize`，对齐两个可变来源 |

禁止使用“同步停止请求”。需要等待完成时写“同步停止操作”；只提交命令时写“异步停止请求”。
