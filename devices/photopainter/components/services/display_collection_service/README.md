# `display_collection_service`

> 管理多页面 PPF2 集合的同步事务、SD 持久化和活动集合原子切换，不拥有 Task。

## 1. 定位

- 层级：Service
- 调用方：`content_refresh_app` 执行同步，`photo_playback_app` 查询活动页面
- 状态所有者：本 Service 独占活动/上一集合、A/B 状态代数与本地页面索引

## 2. 职责边界

负责 Manifest 获取、绝对刷新时间元数据传递、页面复用与下载、PPF2 校验、版本化文件落盘、
A/B 状态提交、启动恢复和旧集合清理。不负责网络启动/停止、唤醒策略、照片选择、按键或
墨水屏刷新。

## 3. 数据流

```text
content_refresh_app Task
    → display_protocol Manifest
    → 逐页复用或下载到单个 96032 字节 PSRAM 缓冲区
    → display_frame_protocol 校验
    → SD 临时文件改名
    → 集合 Manifest + 非活动 A/B 状态槽
    → 内存快照切换 → 锁外提交回调
```

任一步骤失败都不修改当前内存活动集合；未完成文件留作后续复用或尽力清理。

## 4. 生命周期与并发

- `init → sync* → deinit`，同步事务执行时拒绝第二个同步和反初始化。
- 本组件不创建 Task；同步网络和存储操作运行在调用方 Task。
- 快照和页面查询通过内部互斥锁复制，不长期借用内部对象。
- 提交回调在内部锁之外同步执行，借用持续到替换、清除或反初始化。
- SD 不可用或状态损坏不阻止初始化，快照记录错误，下一次同步重新尝试恢复。

## 5. 持久化与内存

- 活动状态使用 `display/state_a.json` 与 `display/state_b.json`，按有效校验和和最大 generation 恢复。
- 旧版黑白状态或损坏的本地元数据不会阻塞启动；Service 以空快照等待下一次四灰阶同步。
- 集合 Manifest 与页面文件按版本不可变命名，只保留活动集合和上一集合。
- 本地存储格式为 schema v3，新集合 Manifest 必须保存 `next_refresh_at` UTC Unix 时间戳；
  旧 schema 状态会被忽略，并在下一轮联网时重新同步完整集合。
- 页面缓冲区固定为一个 PPF2 文件大小，活动页面索引和同步事务工作区也使用固定容量；
  两者都只从 PSRAM 分配，不回退内部 RAM，避免 16 页元数据占用调用方 Task 栈。

## 6. 依赖

调用 `protocols`、`device_sd`、`utils` 和 FreeRTOS 同步原语；不调用其他 Service 或 Application。

## 7. 验证

应覆盖 10/16 页、304、缓存复用、中途下载失败、A/B 单槽损坏、SD 缺失与重启恢复。公共头文件
保持 C ABI，可被 C 和 C++ 调用。
