# API 与所有权规范

## 1. 公共 API 命名

公共符号必须使用组件前缀：

```text
函数：<module>_<verb>[_<object>]()
类型：<module>_<name>_t
宏和枚举值：MODULE_NAME_*
```

- 同步操作使用普通动词，例如 `device_environment_read()`，返回最终执行结果。
- 异步提交使用 `request`，例如 `firmware_ota_request_check()`；返回值只表示是否提交成功。
- 多字段状态读取使用 `get_status_copy` 并复制完整、有界的元数据快照；快照不得包含帧缓冲或
  其他大型载荷。
- 时间、容量和长度参数必须携带单位，例如 `timeout_ms`、`interval_s`、`size_bytes`。
- 禁止没有具体语义的 `do_work()`、`process()`、`handle()`。`manager` 只有在组件确实独占
  状态机和资源协调时才可使用。
- Presentation 的页面适配器使用 `<feature>_presenter_*`，只读模型使用
  `<feature>_view_model_t`，整结构读取使用 `<feature>_presenter_get_view_copy()`。
- Presentation 事件和载荷由 `presentation_dispatch` 定义；UI 私有 `ui_msg_t` 不泄漏给
  Application 或 Presenter。

## 2. 错误语义

项目公共执行 API 继续使用 `esp_err_t`，不额外包装统一项目错误体系：

- `ESP_OK` 表示本次操作完成或命令成功提交，具体含义由同步/异步契约决定。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 生命周期不合法返回 `ESP_ERR_INVALID_STATE`。
- 没有形成新业务语义时，保留原始的超时、校验、内存或 I/O 错误码。
- 网络状态、设备状态等正常多状态结果使用组件自己的枚举或结果结构，不伪装成错误码。
- 组件只报告错误事实；是否致命、降级或重启由 Application 判断。
- 输出参数默认仅在 `ESP_OK` 时有效；允许部分结果时必须在头文件中明确有效字段和判定方式。

异步 API 的返回错误描述“命令是否成功提交”，完成事件中的 `esp_err_t` 才描述实际执行结果。

### 2.1 ESP-IDF 错误处理宏

项目使用 [`esp_check.h`](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0.1/esp32/api-reference/system/esp_err.html)
提供的宏统一记录错误位置并传播原始 `esp_err_t`，但宏的选择必须服从实际恢复语义：

- 当前函数可以立即结束且不需要清理已取得资源时，使用 `ESP_RETURN_ON_ERROR` 传播下层错误，
  使用 `ESP_RETURN_ON_FALSE` 把参数、生命周期或资源条件转换为明确错误码。
- `void` 函数可以立即结束且不需要清理时，使用 `ESP_RETURN_VOID_ON_ERROR` 或
  `ESP_RETURN_VOID_ON_FALSE`。正常的“无需处理”条件不属于错误，不为消除普通 `return` 而
  套用宏。
- 已经取得资源、失败后必须逆序清理时，使用 `ESP_GOTO_ON_ERROR` 或
  `ESP_GOTO_ON_FALSE`。函数必须声明 `esp_err_t ret = ESP_OK`，统一跳转到清理标签并最终
  返回 `ret`；清理失败是否覆盖主错误必须按该组件契约明确决定。
- `_ISR` 版本只允许在真实 ISR 上下文使用。普通 FreeRTOS Task、ESP-IDF 普通事件回调和组件
  回调仍使用非 ISR 版本。
- `ESP_ERROR_CHECK` 会在失败时终止程序，只能用于已经确认无法恢复、无法传播且继续运行会破坏
  系统不变量的致命错误。Application、Service 和 Communication 默认不得用它代替
  错误返回。
- `ESP_ERROR_CHECK_WITHOUT_ABORT` 只用于明确允许继续运行的最佳努力操作；若失败会改变状态、
  恢复路径或上层决策，必须保留显式分支并传播或记录结果。

错误宏负责“记录并返回/跳转”，不负责产品策略。重试与退避、异步完成结果、状态机迁移、降级
继续、多个清理错误的优先级，以及需要先恢复状态再返回的失败都必须保留显式分支。同一层已经
由宏记录并直接传播时，不再追加内容重复的错误日志；宏中的说明文本必须使用中文并指出本层
操作语义。

当前工具链为 ESP-IDF v6.0.1。该版本 `esp_check.h` 的 C++20 以上分支中
`ESP_RETURN_VOID_ON_FALSE` 形参与 C 分支不一致。升级并验证 ESP-IDF 前，C++ `void` 函数
对此场景保留显式 `if` / `return`，C 文件仍按标准接口使用；不得为此增加项目私有宏包装。

## 3. 所有权后缀

只有跨组件传递对象或缓冲区且存在复制、长期借用或所有权转移语义时，函数名才采用：

```text
<module>_<verb>_<object>_<ownership>()
```

普通同步调用中的 `const` 输入默认只在调用期间借用，函数返回后不得保存、入队或转交，
不强制添加 `_borrow`。创建并返回句柄的 `create/open` 与配对的 `destroy/close` 已通过生命周期
动词表达资源归属，也不添加所有权后缀。

### `_copy`

表示函数返回后不存在共享生命周期。以下情况必须使用 `_copy`：

- 异步提交前把输入复制到组件内部。
- 把组件内部的多字段状态或有界复合对象复制给调用者。

```c
esp_err_t environment_service_get_snapshot_copy(
    environment_service_snapshot_t *out_snapshot);

esp_err_t network_manager_get_status_copy(
    network_manager_status_t *out_status);
```

异步命令使用 `_copy` 时，必须在函数返回前完成复制，禁止将调用者栈指针放入队列。

### `_borrow`

当 API 需要特别强调大型缓冲区不会被复制时，可以使用 `_borrow`；这类同步调用不得保存、
入队或转交该指针：

```c
esp_err_t audio_service_write_pcm_borrow(
    const int16_t *in_samples,
    size_t sample_count);
```

除回调注册例外外，显式 `_borrow` API 必须同步，输入通常使用 `const`。

长期保存回调或 ISR handler 的注册 API 必须以 `_borrow` 结尾。组件可以保存回调和 context，
但所有权不转移。借用期从注册成功开始，持续到被后续设置替换、显式取消、对应 remove 或组件
`stop()` 完成，以最先发生者为准。调用方必须保证回调函数和 context 在整个借用期内有效；
公共头必须写明执行上下文和借用终点。普通同步回调若仅在函数返回前执行，可以沿用默认借用
语义，但必须在公共头文件中说明执行上下文。

### `_take`

返回 `ESP_OK` 时所有权转移给被调用组件；返回错误时仍由调用者持有。使用二级指针，并在成功
时置空：

```c
esp_err_t voice_service_submit_frame_take(
    voice_frame_t **inout_frame);
```

禁止使用 `_move`、`_owned`、`_share`、`_peek`、`_raw` 等同义但规则不同的命名。由组件
分配并交给调用者的缓冲区或消息必须提供配对的 `_release()` 或对象销毁 API；未来确有零拷贝
共享需求时才允许增加严格配对的 `_acquire()` / `_release()`。

普通无复合指针的 API，例如 `network_manager_start()`、`audio_service_stop()`，不添加所有权
后缀。

## 4. 参数和资源所有权

- 公共 API 必须通过 `const`、参数名和 Doxygen 明确输入/输出方向；当同类型参数较多或方向
  容易混淆时，应使用 `in_*`、`out_*`、`inout_*`，不要求为所有简单参数机械添加前缀。
- 普通同步 `const` 输入默认只在调用期间借用；异步复制、状态快照、长期回调借用和所有权转移
  必须分别按 `_copy`、`_borrow`、`_take` 规则明确表达。
- 回调收到的 `const event_t *` 只在回调期间有效，不得保存原指针。
- 分配内存的组件负责释放；跨组件转移必须使用 `_take` 契约。不得为了使用该后缀而引入动态
  分配。
- Application、Service 和其他上层公共 API 不得暴露 `TaskHandle_t`、Queue 句柄、`FILE *`、
  HTTP Client 句柄或硬件驱动句柄。
- Drivers 与 BSP 之间可以直接使用作为底层契约的 ESP-IDF 句柄，不要求为隐藏句柄再增加
  包装层。
- 长期资源在 `init()` 创建，在 `deinit()` 按相反顺序释放；部分初始化失败也必须清理已成功
  创建的资源。
- 禁止在周期循环中进行大小不受限制的重复分配。
- 大型缓冲区不得放在 Task 栈；DMA、内部 RAM 或 PSRAM 要求必须在分配处明确。
- 所有分配失败必须返回可诊断错误。

## 5. 生命周期、头文件与注释

- 只有真正存在相应阶段时才提供 `init/start/stop/deinit`，不添加空接口。
- `stop/deinit` 只有在资源达到文档声明的终态后才能释放本地所有权；超时或清理失败时必须
  保留 `STOPPING/CLEANUP_FAILED` 等显式状态并拒绝下一次 `start`，不能只返回错误后把对象
  重置为未初始化。
- 公共 API 放在组件的 `include/` 目录。组件可以按功能拆分多个公共头文件，并可提供
  `include/<component>.h` 作为聚合入口。
- 私有类型和函数放在组件内部头文件，不得由公共头文件泄漏。
- 所有公共 API 必须提供中文 Doxygen；复杂算法、生命周期、并发或所有权不直观的 `static`
  私有函数也必须说明契约。
- 项目日志和错误提示使用中文，禁止输出密码、Token、Authorization 或完整凭据。
- API 必须说明同步/异步、阻塞时间、调用上下文、输出有效性和指针生命周期，不能依赖实现
  代码让调用者猜测。
- 公共符号前缀默认与 ESP-IDF 组件名一致；确有历史稳定简称时必须在组件公共头文件中保持
  唯一。
