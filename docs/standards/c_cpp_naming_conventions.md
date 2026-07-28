# 嵌入式 C/C++ 术语与命名规范

> 状态：审阅草案，创建于 2026-07-28。适用于 DeskSuite 全仓及其他采用本规范的嵌入式项目。
>
> 本规范以嵌入式 C 公共 API 为主要对象，并规定 C++ 私有实现与 C ABI 的一致命名方式。
> 术语的唯一含义由[嵌入式 C/C++ 受控术语表](c_cpp_terminology.md)定义；本文只规定如何选择
> 术语并组成标识符。

## 1. 目标

本规范用于达到以下结果：

- 相同语义在不同模块中使用相同英文词。
- 函数名能够表达操作对象、同步或异步完成语义以及所有权。
- 公共 C API 在 C 与 C++ 混合实现中保持稳定、一致且可搜索。
- 私有实现名称与作用域相称，不通过未登记缩写、泛词或包装层隐藏真实行为。
- AI 或开发者需要引入新术语时，先扩展受控术语表并向用户说明理由。

本规范不试图统一第三方库、芯片厂商 SDK、RTOS、协议字段或生成代码的既有名称。

## 2. 适用范围与优先级

适用于：

- 裸机、RTOS 和嵌入式 Linux 用户态组件中的项目自有 C/C++ 代码。
- 公共头文件、私有实现、驱动、HAL/BSP、Service、Application、Task 和回调。
- C ABI、C/C++ 交界头文件及不向 C 暴露的 C++ 私有类型。

不适用于：

- 未修改的第三方源码和供应商 SDK。
- 标准、协议、芯片手册和外部 API 已经固定的名称。
- 自动生成文件。

发生冲突时按以下顺序处理：

```text
项目已经确认的架构与协议契约
    → 本规范与受控术语表
    → 目标平台的官方风格
    → 被调用外部库的原始名称
```

项目需要偏离本规范时，必须在项目自己的规范中记录例外、原因和适用边界，不能让相同概念在
不同文件中临时选择不同拼写。

## 3. 外部基线

本规范组合使用以下公开规则：

1. [ESP-IDF Style Guide][esp-idf-style]：公共符号使用组件前缀，文件私有符号使用
   `static`，文件静态变量使用 `s_`，避免不必要或未登记缩写；C 类型使用 `snake_case_t`。
2. [C++ Core Guidelines NL.7/NL.8][cpp-core-naming]：名称长度与作用域成比例，为项目选择
   唯一 house style，外部库保留原有风格。
3. [Google C++ Style Guide 的 Naming][google-naming]：名称应具有描述性，避免删除单词
   字母形成非标准缩写。
4. [LLVM Coding Standards 的命名原则][llvm-naming]：名称必须匹配实体的语义和角色，并
   保持一致拼写。

本文不照搬 Google 的函数大小写规则。嵌入式公共 C API 和 C++ 成员函数统一使用
`snake_case`；C++ 私有类使用 `PascalCase`。

## 4. 受控术语使用规则

公共 API、跨文件私有接口、类型、状态机、事件和所有权名称必须优先从
[受控术语表](c_cpp_terminology.md)选择动作词与语义名词。

以下名称不要求逐个加入术语表：

- 只在短函数内使用的局部变量。
- 芯片、寄存器、协议和外部 SDK 已经固定的名称。
- 单一模块内部且不会形成可复用概念的具体领域对象。

以下名称不存在于术语表时，必须先新增术语：

- 新的公共 API 动作词。
- 新的生命周期、并发、异步、所有权或数据形态名词。
- 会在两个及以上模块中复用的领域术语。
- 用于替代现有术语、可能形成近义词的名称。

新增术语必须遵守术语表中的告知流程。禁止先在代码中使用，再把文档留给后续补充。

## 5. 公共 C API

### 5.1 函数

公共函数使用：

```text
<module>_<verb>_<object>[_<qualifier>][_<ownership>]()
```

- `<module>`：稳定的组件、模块或能力前缀。
- `<verb>`：受控术语表中的动作词。
- `<object>`：实际被操作的对象。
- `<qualifier>`：来源、目标、模式或单位。
- `<ownership>`：仅使用受控所有权后缀。

示例：

```c
sensor_init();
sensor_read_sample();
display_request_flush();
display_wait_flush_done(timeout_ms);
network_get_status_copy(out_status);
storage_read_info(out_info);
```

允许省略没有信息增量的对象：

```c
module_init();
module_start();
module_stop(timeout_ms);
module_deinit();
```

禁止使用不能说明行为的公共名称：

```text
do_work(), process(), handle(), execute(), perform(), run_action()
```

协议或框架要求的入口使用名词后缀 `_handler`，例如 `http_upload_handler()`；业务动作不能
借用 `handler` 隐藏。

### 5.2 类型、枚举和宏

```text
C 类型：<module>_<noun>_t
C 枚举值：<MODULE>_<NOUN>_<VALUE>
宏：<MODULE>_<PURPOSE>
```

要求：

- 公共 C 类型使用 `snake_case_t`。
- C 枚举通过 `typedef enum` 定义，枚举值包含模块和类型语义前缀。
- 宏必须有模块前缀，不能使用无命名空间的 `MIN`、`MAX`、`DEBUG` 等名称。
- 非宏常量不因“像常量”而机械使用全大写；遵循所在语言和项目的常量规则。
- 公共不透明句柄使用 `<module>_handle_t`，不能暴露内部结构。

### 5.3 参数、字段和返回

- 输入参数默认使用具体名词；容易混淆方向时使用 `in_`、`out_`、`inout_`。
- 输出参数使用 `out_<noun>`，不能只写 `out`、`value` 或 `data`。
- 回调上下文使用 `context` 或受控短拼写 `ctx`；同一公共接口族必须服从同一缩写配置，
  不能混用。`arg`、`cookie` 只在外部 API 已固定时保留。
- 布尔查询函数以 `is_`、`has_`、`can_` 或 `should_` 开头。
- 布尔字段使用正向含义，例如 `active`、`enabled`、`pending`。
- 数量使用 `count` 或受控短拼写 `num`，容量使用 `capacity`，字节数使用 `size_bytes`；
  具体拼写服从所在作用域配置。
- 时间、频率和容量必须带单位，例如 `timeout_ms`、`interval_us`、`sample_rate_hz`。
- 绝对时间说明时间基准，例如 `deadline_monotonic_us`、`timestamp_utc_s`。

错误返回类型由项目统一决定。公共 API 必须说明成功代表“操作完成”还是“请求已接受”，不能
只依赖返回类型让调用方猜测。

## 6. 文件与内部符号

- C 源文件使用 `.c`，C++ 源文件使用 `.cpp`。
- 公共或跨语言头文件使用 `.h`；只供 C++ 私有实现使用的头文件可以使用 `.hpp`。
- 文件名使用小写 `snake_case`，同组件源码带稳定模块前缀。
- 文件内函数和变量声明为 `static`；文件静态变量使用 `s_`。
- 全局可变变量默认禁止。确需跨文件暴露时，必须由单一所有者封装访问 API。
- 局部变量名称长度与作用域成比例；循环索引可以使用 `i`，跨函数字段不能使用不明缩写。
- Task 入口文件使用 `<owner>_<purpose>_task.c/.cpp`，入口函数以 `_task` 结尾。
- 不使用 `worker`、`thread`、`loop` 作为 RTOS Task 的同义后缀；如果平台正式概念就是
  Thread，则保持该平台术语。

### 6.1 受控短缩写配置

以下常用短拼写在嵌入式 C 中合法：

| 完整拼写 | 短拼写 |
| --- | --- |
| `callback` | `cb` |
| `context` | `ctx` |
| `config` | `cfg` |
| `message` | `msg` |
| `count` | `num` |
| `length` | `len` |
| `buffer` | `buf` |

使用规则：

- 函数内局部变量和严格私有的短接口可以直接使用受控短拼写。
- 项目、组件或公共 API 族可以针对表中每一项选择完整或紧凑拼写，也可以在本地规范中强制
  选择结果。
- 一旦某个作用域对某项选择或强制一种拼写，同一语义不得再混用另一种。例如选择
  `callback=cb` 后，类型和参数统一使用 `_cb_t`、`cb`，不再同时出现
  `_callback_t`、`callback`。
- `num` 表示数量时写成 `num_frames`、`num_items`；不能用它替代任意数值。
- `len` 必须能从对象或后缀看出单位，例如 `msg_len`、`buf_len_bytes`。
- `buf` 在存在多个缓冲区时补充方向或用途，例如 `rx_buf`、`tx_buf`。
- 未列入[受控术语表](c_cpp_terminology.md#受控缩写)的短拼写仍需先登记并向用户说明理由。

## 7. 同步、异步和完成语义

### 7.1 同步操作

普通动作词表示返回时已经达到函数声明的最终结果：

```c
device_read_sample();
service_start();
service_stop(timeout_ms);
```

`stop()` 返回成功时必须已经停止。不能把只设置停止标志的函数命名为 `stop()`。

### 7.2 异步提交

异步公共 API 使用 `request_<operation>()`：

```c
display_request_flush();
application_request_stop();
```

返回成功只表示请求已复制、入队或记录。最终结果必须通过事件、结果队列、回调或后续状态读取
获得。

不使用 `_async` 同时表达提交语义，不使用 `sync` 作为“阻塞版本”的后缀。确需同时提供两种
形式时，使用：

```text
request_<operation>()  → 异步提交
<operation>()          → 同步完成
wait_<operation>_done() → 等待已经提交的操作
```

### 7.3 事件、通知与回调

- `event` 是已经发生的不可变事实。
- `notification` 是允许合并的轻量唤醒提示，消费者收到后重新读取事实。
- `callback` 是调用机制，不是事件的同义词。
- `command` 是已接受、等待唯一所有者执行的内部消息。
- `dispatch` 用于路由已经构造好的类型化消息。
- `publish` 用于事实所有者发布不可变事件。

函数指针类型可以使用完整形式 `_callback_t` 或紧凑形式 `_cb_t`，由项目或组件缩写配置决定。
回调 API 必须说明执行上下文、是否可阻塞、是否允许重入以及参数的有效期。

## 8. 数据形态

`state`、`status`、`snapshot`、`result` 和 `runtime` 的详细语义由
[受控术语表](c_cpp_terminology.md#状态与数据形态)定义。标识符形态固定为：

| 类型 | 允许形态 | 典型读取 |
| --- | --- | --- |
| `<module>_state_t` | 单一离散阶段的 `enum` | `get_state()` |
| `<module>_status_t` | 有界运行摘要 `struct` | `get_status_copy()` |
| `<module>_snapshot_t` | 有界领域或硬件数据 `struct` | `get_snapshot_copy()` / `read_snapshot()` |
| `<module>_runtime_t` | 仅所有者私有的可变 `struct/class` | 不提供整结构公共 Getter |
| `<operation>_result_t` | 单次操作结果值、枚举或结构 | 由操作直接输出 |

禁止：

- 使用 `_state_t` 命名复合可变结构。
- 使用 `_status_t` 命名单一阶段枚举。
- 使用 `_snapshot_t` 命名所有者内部可变对象。
- 创建 `status_snapshot`、`state_info` 等叠词掩盖边界。

## 9. 所有权

公共 API 只使用以下所有权表达：

| 后缀或动词 | 语义 |
| --- | --- |
| `_copy` | 返回前完成复制，返回后不存在共享生命周期 |
| `_borrow` | 不转移所有权；显式大型缓冲区借用或长期保存回调 |
| `_take` | 成功时所有权转移，失败时仍归调用方 |
| `acquire` / `release` | 取得和释放严格配对的租约、引用或共享资源使用权 |
| `create` / `destroy` | 创建和销毁独占对象 |
| `open` / `close` | 打开和关闭文件、流或会话 |

普通同步 `const` 输入默认只在调用期间借用，不机械添加 `_borrow`。长期保存回调或
`context` 的注册 API 必须使用 `_borrow` 并写明借用终点。

不新增 `_owned`、`_move`、`_share`、`_peek`、`_raw` 等平行规则。

## 10. C++ 私有实现

- 私有类和结构体使用 `PascalCase`。
- 成员函数、成员变量和命名空间使用 `snake_case`。
- 类名描述资源或领域所有权，例如 `AudioRuntime`，不使用孤立的 `Manager`、`Helper`、
  `Object`。
- C++ Runtime 用于收拢同一生命周期的资源与不变量，不为每个 C 函数机械创建同名方法。
- 不为字段机械增加 getter/setter；只暴露维护不变量所需的操作。
- 公共 C ABI 不暴露类、模板、重载、引用、异常、智能指针或标准库容器。
- C++ 回调 thunk 使用 `static` 函数，通过 `void *context` 或 `void *ctx` 定位实例，只做
  参数校验和快速投递；拼写服从所在组件配置。

## 11. 注释与日志中的术语

- 公共 API 注释先说明调用完成语义，再说明实现限制。
- 注释写“为什么、完成了什么、所有权到哪里结束”，不复述函数名。
- 中文固定译法以术语表为准，同一英文术语不能在不同文件中随意更换译法。
- “同步停止操作”表示调用返回时已经停止；“异步停止请求”表示只提交命令。禁止使用含义
  自相矛盾的“同步停止请求”。
- 项目日志和错误消息遵循项目语言规范；协议、寄存器和第三方错误原文可以保留。

## 12. 迁移与评审

命名重构必须：

1. 先确认术语和完成语义，再修改声明、实现、调用方、测试和文档。
2. 单独提交命名变更，不混入业务行为、线程模型、错误码或持久化语义变化。
3. 项目自有且无外部消费者的旧名直接删除，不创建宏别名、deprecated 转发函数或包装层。
4. 外部 ABI 确需兼容时，明确兼容期限和删除条件；兼容入口不进入新代码。
5. 使用静态搜索检查旧术语残留，并逐个解释确需保留的外部名称。
6. 公共 API 更名后重新核对 Doxygen、同步/异步语义、回调上下文和所有权。

评审检查：

- [ ] 动作词和语义名词是否存在于受控术语表？
- [ ] 新术语是否已经记录并向用户说明理由？
- [ ] 公共符号是否有稳定模块前缀？
- [ ] 函数名是否说明对象和完成语义？
- [ ] `state/status/snapshot/result/runtime` 是否按结构和用途选择？
- [ ] 异步提交是否使用 `request`，最终结果是否有明确出口？
- [ ] 复合缓存副本是否带 `_copy`，长期借用是否带 `_borrow`？
- [ ] 使用的短拼写是否存在于受控缩写表，并与所在作用域配置一致？
- [ ] 是否可以删除旧名而不增加包装函数？

[esp-idf-style]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/style-guide.html
[cpp-core-naming]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#nl8-use-a-consistent-naming-style
[google-naming]: https://google.github.io/styleguide/cppguide.html#Naming
[llvm-naming]: https://llvm.org/docs/CodingStandards.html#name-types-functions-variables-and-enumerators-properly
