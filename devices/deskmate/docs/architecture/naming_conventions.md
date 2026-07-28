# DeskMate 术语与命名规范

> 状态：审阅草案，创建于 2026-07-28。
>
> 本文先统一“一个概念使用哪个词”，再规定如何把词组成 C/C++ 标识符。用户确认前，本文不覆盖
> 同目录中已经确认的架构规范，也不作为批量修改公共符号的授权。

## 1. 目标与适用范围

本文用于解决以下问题：

- 同一概念在不同文件中使用不同英文单词，例如 `callback`、`cb` 和 `listener`。
- 同一个词承担多种含义，例如 `sync` 同时表示阻塞等待、Dashboard 拉取和并发同步。
- 函数名只描述调用动作，没有描述完成语义，例如用 `handle` 隐藏“提交、更新或发布”。
- 为迁移旧 API 增加无语义包装函数，导致代码量增加且调用链变长。

适用范围：

- `main/`、`components/` 和项目自有测试、脚本中的 C/C++ 标识符及对应中文文档。
- DeskMate 自有公共 C API、组件私有函数、类型、枚举、Task、回调和状态字段。

不适用范围：

- ESP-IDF、FreeRTOS、LVGL、第三方组件、生成文件和网页依赖的既有名称。
- `shared/` 中已经对其他设备发布的公共 API；调用方保留提供者名称。需要修改共享 API 时，
  必须作为跨设备任务单独评审。
- HTTP、JSON、OTA 等协议或标准已经定义的字段名。

## 2. 采用的外部基线

项目不完整照搬某一份通用 C++ 风格，而采用最接近当前 ESP32 C/C++ 混合固件的组合：

1. 以 [ESP-IDF Style Guide][esp-idf-style] 为标识符形式的主基线：公共符号使用组件前缀，
   文件内符号使用 `static`，文件静态变量使用 `s_`，避免不必要缩写；C 类型使用
   `snake_case_t`，C 枚举值使用带命名空间的 `UPPER_SNAKE_CASE`。
2. 采用 [C++ Core Guidelines NL.7/NL.8][cpp-core-naming] 的“名称长度与作用域成比例”和
   “项目选择唯一 house style，外部库保留原风格”。公共名称完整，短作用域局部变量可以简短。
3. 采用 [Google C++ Style Guide 的 Naming][google-naming] 对描述性、作用域和缩写的要求，
   但不采用其 `PascalCase` 函数命名；DeskMate 公共 API 继续服从 ESP-IDF 的
   `snake_case`。
4. 采用 [LLVM Coding Standards 的命名原则][llvm-naming]：名称必须匹配实体的语义与角色，
   只保留公认缩写，并在整个调用链使用同一拼写。

当外部规范冲突时，优先级为：

```text
已确认的 DeskMate 架构契约
    → 本文确认后的术语规则
    → ESP-IDF 命名形式
    → C++ Core Guidelines / Google / LLVM 的通用可读性建议
    → 被调用第三方库的原始名称
```

例如，C++ Core Guidelines 建议只让宏使用全大写；但 DeskMate 的公共 C 枚举继续采用
ESP-IDF 的带组件前缀全大写形式，以保持 C API 一致。

## 3. 标识符形式

### 3.1 公共 C API

公共函数必须使用：

```text
<component>_<verb>_<object>[_<qualifier>][_<ownership>]()
```

其中：

- `<component>` 与 ESP-IDF 组件或稳定模块前缀一致，例如 `device_rtc`、
  `environment_service`、`app_network`。
- `<verb>` 必须来自本文的动作词表。
- `<object>` 表示实际被读取、修改或提交的对象，不使用 `data`、`item`、`thing`、
  `work` 等无法区分职责的泛词。
- `<qualifier>` 只补充来源、目标、模式或单位，例如 `from_dashboard`、`at_utc`、
  `timeout_ms`。
- `<ownership>` 只使用已确认的 `copy`、`borrow`、`take`。

允许省略没有信息增量的部分：

```c
esp_err_t web_file_service_init(void);
esp_err_t device_environment_sample(void);
esp_err_t app_network_request_dashboard_refresh(void);
```

公共类型、枚举和宏使用：

```text
类型：<component>_<noun>_t
枚举值：<COMPONENT>_<NOUN>_<VALUE>
宏：<COMPONENT>_<PURPOSE>
```

公共 C ABI 类型继续使用 `snake_case_t`，不因实现迁移到 C++ 而改成 C++ 类名。

### 3.2 C++ 私有实现

- 私有类和结构体使用 `PascalCase`，成员函数和成员变量使用 `snake_case`。
- C++ 类只用于表达资源所有权、不变量或实例边界，不为每个 C 函数机械创建同名方法。
- C++ 私有 Runtime 的名称描述其所有权，例如 `WebFileRuntime`；避免只有
  `Manager`、`Helper`、`Context` 的泛名。
- 跨 C/C++ 边界的 thunk 使用 `<source>_<event>_callback` 或
  `<protocol>_<operation>_handler`，只做适配和快速投递。

### 3.3 文件、变量和参数

- 文件名使用小写 `snake_case`，同组件源码带组件前缀。
- 定义 FreeRTOS Task 入口的文件使用 `_task.c` 或 `_task.cpp`，入口函数以 `_task` 结尾。
- 文件静态变量使用 `s_`；文件私有函数声明为 `static`。
- 参数和局部变量使用 `snake_case`，不编码类型信息，不使用 `p_`、`str_`、`u32_` 等匈牙利
  前缀。
- 布尔查询函数以 `is_`、`has_`、`can_` 或 `should_` 开头；布尔字段使用正向形容词，
  例如 `active`、`enabled`、`pending`，避免双重否定。
- 数量使用 `count`，容器上限使用 `capacity`，字节数使用 `size_bytes`，字符长度使用
  `length` 或带单位的具体名称。
- 时间参数必须带单位，例如 `timeout_ms`、`interval_s`、`timestamp_us`；绝对时间说明
  时间基准，例如 `at_utc`，相对时长不得使用 `time`。
- 输出参数优先使用 `out_<noun>`，输入输出参数使用 `inout_<noun>`；回调上下文统一写
  `context`，不再混用 `ctx`。

## 4. 动作词表

### 4.1 生命周期与资源

| 词 | 唯一含义 | 返回 `ESP_OK` 时的保证 |
| --- | --- | --- |
| `init` | 创建组件固定资源并建立可调用状态 | 初始化完成；不隐含开始持续工作 |
| `deinit` | 释放由 `init` 创建的固定资源 | 组件回到未初始化状态 |
| `start` | 从已初始化状态开始运行 | 已达到公共契约声明的运行态 |
| `stop` | 停止运行并等待终态 | 已完全停止；可失败清理不得遗留成“已停止” |
| `request_start` / `request_stop` | 非阻塞提交启停命令 | 命令已接受，不表示启停完成 |
| `create` / `destroy` | 创建或销毁一个独立对象、句柄或 Task | 对象所有权已经取得或释放 |
| `open` / `close` | 打开或关闭文件、会话、流等可关闭资源 | 会话或流已经打开或关闭 |
| `reset` | 保留对象和生命周期资源，把可变内容恢复到初始值 | 对象仍然存在且内容已复位 |
| `clear` | 清除标志、缓存项、告警或已保存内容 | 指定内容已不存在或标志已复原 |
| `recover` | 从已知失败状态修复资源或事务 | 达到函数声明的可继续状态 |
| `reconcile` | 根据外部事实重新计算并收敛本地状态 | 本地状态已与所给事实一致 |

`stop()` 默认表示“同步等待停止完成”，不添加 `_sync`。如果返回时只记录意图，必须命名为
`request_stop()`。禁止使用“同步停止请求”这一组合；应分别写“同步停止操作”或
“异步停止请求”。

### 4.2 数据读取、写入与刷新

| 词 | 唯一含义 | 不应替代为 |
| --- | --- | --- |
| `get` | 从内存或轻量状态读取，不触发外部 I/O | `read`、`query` |
| `read` | 从设备、文件、流或总线执行输入 I/O | `get` |
| `write` | 向设备、文件、流或总线执行输出 I/O | `set`、`update` |
| `set` | 设置一个明确属性或配置值 | `update` |
| `update` | 使用调用方已经提供的数据更新所有者状态，不主动重新读取来源 | `refresh` |
| `refresh` | 重新读取权威来源并替换或推进本地快照 | `sync`、`update` |
| `apply` | 把一个完整输入模型应用到现有对象，强调映射结果而非数据来源 | `update` |
| `load` | 从持久化介质载入结构 | `get` |
| `save` | 把结构写入持久化介质 | `set` |
| `copy` | 复制值或有界复合对象，返回后无共享生命周期 | `get` 单独表达 |

Dashboard 是服务端到设备的单向拉取，因此统一使用 `refresh`：

```c
app_network_request_dashboard_refresh();
app_network_cancel_dashboard_refresh();
app_network_refresh_dashboard_for_power_save();
app_network_set_dashboard_auto_refresh_enabled();
app_network_get_next_dashboard_refresh_at_utc();
```

`sync` 不再作为“阻塞调用”的函数后缀。它只保留给领域中确实表示两侧对齐的术语，例如
`system_clock_sync_from_rtc()`；并发语境使用中文“同步机制”，不要据此给普通函数增加
`_sync`。如果未来出现双向数据合并，才使用 `synchronize` 或经评审确认的 `sync`。

### 4.3 异步、事件与调用边界

| 词 | 唯一含义 | 典型形式 |
| --- | --- | --- |
| `request` | 把命令复制或记录给异步所有者；返回值只表示是否接受 | `request_<operation>` |
| `command` | 已接受、等待唯一所有者执行的内部消息 | `<component>_command_t` |
| `event` | 已经发生的不可变事实，不能被消费者撤销 | `<component>_event_t` |
| `notification` | 可合并的轻量唤醒提示；消费者必须重读最新状态 | Task notification / pending 标志 |
| `callback` | 调用机制及函数指针类型，不等于事件本身 | `<purpose>_callback_t` |
| `dispatch` | 把已经构造好的类型化事件或意图路由到其传输或消费者 | `presentation_dispatch_*` |
| `publish` | 由事实所有者对外发布一个不可变事件 | `publish_<event>` |
| `wait` | 有界阻塞直到已提交操作完成或超时 | `wait_<operation>_done` |
| `cancel` | 请求取消尚未完成的操作 | 必须说明运行中是否可取消及最终结果 |
| `handler` | 满足 HTTP、SDK、协议或框架入口签名的适配函数 | `<protocol>_<operation>_handler` |

规则：

- 项目自有异步公共 API 使用 `request_<operation>`，不使用 `_async`。例如
  `device_display_request_flush()` 与配对的 `device_display_wait_flush_done()`。
- 函数指针类型统一使用 `_callback_t`，不再新增 `_cb_t`。只有真正维护多订阅者集合时，
  注册动作可以使用 `register/unregister`；可调用对象的类型仍叫 `callback`。
- `notify` 只用于无耐久载荷、允许合并的唤醒提示。传递业务事实时使用 `event`，提交可执行
  工作时使用 `request`。
- `handle` 不作为项目公共业务 API 的动作词。HTTP/SDK 适配器使用名词后缀 `_handler`；
  业务函数必须改成 `apply`、`route`、`dispatch`、`update` 或具体领域动词。
- `emit` 不作为新的公共动作词；事实所有者使用 `publish`，路由层使用 `dispatch`。
- `process`、`do_work`、`execute`、`perform` 不能说明对象变化或完成语义，不用于新的公共
  API；私有函数也应优先写出具体动作。

### 4.4 所有权

所有权后缀继续服从 [API 与所有权规范](api_conventions.md)：

| 后缀或动词 | 唯一含义 |
| --- | --- |
| `_copy` | 在返回前复制输入，或把所有者的有界复合状态复制给调用者 |
| `_borrow` | 明确的大缓冲区同步借用，或组件长期保存回调和 `context` 但不取得所有权 |
| `_take` | `ESP_OK` 时所有权转移，失败时仍归调用方 |
| `acquire` / `release` | 取得和释放严格配对的租约、引用或共享资源使用权 |
| `create` / `destroy` | 创建和销毁独占对象 |
| `open` / `close` | 打开和关闭会话、文件或流 |

不增加 `_owned`、`_move`、`_share`、`_peek`、`_raw` 等近义后缀。

## 5. 名词词表

| 英文 | 中文固定译法 | 唯一含义 |
| --- | --- | --- |
| `intent` | 用户意图 | UI 或输入层表达的期望动作，尚未表示 Application 接受 |
| `request` | 请求 | 调用方提交一次操作的 API 行为 |
| `command` | 命令 | 已被异步入口接受、等待所有者执行的内部消息 |
| `event` | 事件 / 事实事件 | 已发生且不可变的事实 |
| `notification` | 通知 / 唤醒通知 | 可合并提示；不承诺保存事件历史或完整载荷 |
| `result` | 结果 | 一次操作的最终输出或错误 |
| `state` | 状态 | 生命周期或状态机当前离散阶段 |
| `status` | 状态摘要 | 面向调用方的有界运行摘要，可包含 `state`、标志和 `last_error` |
| `snapshot` | 快照 | 某一时点整结构复制的领域数据，不返回内部可变指针 |
| `info` | 能力信息 | 初始化后基本稳定的设备能力或静态属性 |
| `config` | 配置 | 初始化或操作所需的调用方输入参数 |
| `settings` | 产品设置 | 用户可持久化修改的产品偏好 |
| `context` | 上下文 | 一组具有共同生命周期的依赖或回调定位信息 |
| `runtime` | 运行时 | 同一生命周期内资源与可变状态的唯一所有者 |
| `lease` | 租约 | 带类型和代次、必须显式释放的临时独占使用权 |
| `generation` | 代次 | 用于拒绝旧持有者的身份编号 |
| `version` | 版本 | 用于判断快照或展示数据的新旧顺序 |
| `sequence` | 序号 | 一组请求或消息的严格顺序编号 |
| `view_model` | 视图模型 | Presenter 提供给 UI 的只读、已格式化呈现数据 |
| `page` | 页面 | 产品导航和 Presentation 契约中的页面身份 |
| `screen` | LVGL Screen | 具体 LVGL 根对象；不作为产品页面的同义词 |

补充规则：

- `state` 是单个状态机阶段，`status` 是对外摘要，`snapshot` 是某一时点的数据副本。不能仅因
  结构体字段多就任选其一。
- 返回复合 `status`、`snapshot` 或 `info` 副本的函数必须带 `_copy`。返回标量、枚举值或
  借用静态只读字符串时不机械添加 `_copy`。
- `manager` 只保留给确实独占技术状态机和资源协调的既有组件，例如共享
  `network_manager`；普通类或模块不得用它替代具体职责。
- `service` 表示已满足 Service 层进入条件的事务或持续资源所有者，不是任意工具函数集合。
- `Task` 是执行机制，不是业务层、服务或模块的同义词。

## 6. 缩写白名单

公共名称只允许项目成员无需查表即可识别的标准缩写：

```text
API, ABI, BSP, CPU, DMA, DNS, GPIO, HTTP, HTTPD, I2C, ID, IP, IPv4,
ISR, JSON, LVGL, NVS, OTA, PSRAM, RAM, RTC, SDK, SD, SNTP, SPI, UI,
URI, URL, UTC, Wi-Fi
```

标识符中的缩写按所在命名形式整体小写，例如 `http_handler`、`ota_event`、`url`、
`device_id`。枚举值和宏中整体大写。以下项目内短写不再新增：

- `cb` → `callback`
- `ctx` → `context`
- `msg` → `message`，但 `ui_msg_t` 这类作用域严格局限于 UI 私有队列的短类型可保留
- `cfg` → `config`
- `num` → 根据语义改为 `count`、`index`、`value` 或具体名词
- `ret` → 私有短错误传播变量可以保留；公共字段和参数使用 `result` 或 `error`

## 7. 当前代码迁移候选

以下清单来自 2026-07-28 对 `main/` 与 `components/` 项目自有头文件的静态盘点，只是后续
术语收敛重构的审阅范围，不表示本草案已经修改这些符号。

### 7.1 可直接机械收敛

| 当前名称 | 拟统一名称 | 原因 |
| --- | --- | --- |
| `button_service_event_cb_t` | `button_service_event_callback_t` | 回调类型统一使用 `callback` |
| `UI_RUNTIME_STATE_UNINIT` | `UI_RUNTIME_STATE_UNINITIALIZED` | 生命周期状态不使用局部缩写 |
| `calendar_get_snapshot()` | `calendar_get_snapshot_copy()` | 返回完整快照副本 |
| `mail_get_snapshot()` | `mail_get_snapshot_copy()` | 返回完整快照副本 |
| `quota_get_snapshot()` | `quota_get_snapshot_copy()` | 返回完整快照副本 |
| `weather_get_snapshot()` | `weather_get_snapshot_copy()` | 返回完整快照副本 |
| `dashboard_store_get_snapshot()` | `dashboard_store_get_snapshot_copy()` | 返回完整快照副本 |
| `app_network_get_lease_snapshot()` | `app_network_get_lease_snapshot_copy()` | 返回完整快照副本 |

`dashboard_store_get_weather/calendar/mail/quota()` 也会核对实现；若均为整结构复制，则分别增加
`_copy`，不为旧名保留转发包装。

### 7.2 需要按调用链同步修改

| 当前词组 | 拟统一词组 | 说明 |
| --- | --- | --- |
| Dashboard `sync` | Dashboard `refresh` | 单向拉取，与后端 `next_refresh_at_utc` 和 Data 层一致 |
| `*_flush_async()` | `*_request_flush()` | 明确返回只表示异步提交成功 |
| `_cb_t` / `_listener_t` | `_callback_t` | 调用对象统一；多订阅能力由 `register/unregister` 表达 |
| `emit_user_intent` | `dispatch_user_intent` | UI 把已构造意图同步路由给 Application，不是发布完成事实 |
| 公共 `handle_*` / `*_handle` | 具体动作词 | 依据实际行为改为 `apply`、`route`、`dispatch` 或具体领域动词 |
| 产品层 `screen` | `page` | `screen` 只保留给 LVGL 对象 |

### 7.3 需要先拆清语义再改名

- `UI_USER_INTENT_SCREEN_LOADED` 是 UI 已经完成加载的事实，不是用户意图。应把“UI 用户动作”
  与“UI 生命周期事件”拆成两个类型后再命名，不能只做字符串替换。
- `app_page_notify_screen_loaded()`、`app_page_publish_initial_ui()`、
  `app_page_dispatch_current()` 和 `app_key_dispatch_event()` 同时混合事实接收、状态迁移和
  Presentation 路由。后续应按实际完成语义选择 `on_*_event`、`request_*` 或
  `presentation_dispatch_*`，不增加转发层。
- `consume_input()` 的 `bool` 返回表示“是否已消费”时语义成立；如果函数还会异步提交命令，
  需要在 Doxygen 中分别说明“已消费”和“命令已接受”，必要时改为显式结果类型。
- `status`、`state`、`snapshot` 当前大部分符合规则，但 Data 层旧接口和部分私有 Task 类型仍
  混用。迁移时按所有权与对外可见性逐个判断，不全局替换文本。

## 8. 术语重构执行规则

用户确认本文后，术语收敛重构必须遵守：

1. 只改名称、注释、Doxygen、文件名和必要的调用点，不在同一提交改变业务行为、线程模型、
   队列策略或错误码。
2. 按“公共声明 → 实现 → 全部调用方 → README/架构描述 → 测试”的顺序修改一条完整调用链。
3. 项目已经进入功能稳定期，DeskMate 自有且无外部消费者的旧名不保留兼容包装、宏别名或
   deprecated 转发函数；Git 历史承担追溯职责。
4. 共享组件、第三方 API 和协议字段不为追求局部整齐而改名。
5. 每个提交只收敛一个可独立回滚的术语组，例如“快照复制后缀”或“Dashboard refresh”。
6. 修改后使用 `rg` 检查旧词残留；残留必须属于第三方名称、历史文档引用或有明确不同语义。
7. 公共函数更名后同步补全中文 Doxygen，尤其说明同步/异步、完成条件、阻塞上限、回调上下文
   和指针生命周期。
8. 编译通过只能证明符号和类型闭合；运行时语义仍由用户按实际设备链路手动测试。

## 9. AI 新增或修改名称时的检查清单

- [ ] 是否先在本文词表中找到现有词，而不是创造近义词？
- [ ] 名称是否描述“对象发生了什么”，而不是只写 `handle/process/do`？
- [ ] `request` 的返回是否真的只表示接受，最终结果是否有明确出口？
- [ ] `stop` 返回 `ESP_OK` 时是否真的已经停止？
- [ ] `status/state/snapshot/info` 是否符合各自定义？
- [ ] 复合对象复制是否带 `_copy`，长期回调借用是否带 `_borrow`？
- [ ] 回调、事件、通知和命令是否没有相互冒充？
- [ ] Dashboard 单向拉取是否使用 `refresh`，是否误用 `sync` 表示阻塞？
- [ ] 公共名是否带组件前缀，局部名长度是否与作用域相称？
- [ ] 是否引入了白名单外缩写或同一缩写的另一种大小写？
- [ ] 是否可以直接删除旧名，而不是增加包装函数？
- [ ] 中文注释是否使用本文固定译法并明确完成语义？

[esp-idf-style]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/style-guide.html
[cpp-core-naming]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#nl8-use-a-consistent-naming-style
[google-naming]: https://google.github.io/styleguide/cppguide.html#Naming
[llvm-naming]: https://llvm.org/docs/CodingStandards.html#name-types-functions-variables-and-enumerators-properly
