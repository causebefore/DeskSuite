# `display_present_service`

> 串行执行本地 PPF2 页面或单行/多行 ASCII 状态页的墨水屏全局刷新，不选择页面。

## 1. 定位与职责

- 层级：Service
- 调用方：`photo_playback_app`，以及组合根注入的 OTA 更新提示适配器
- 状态所有者：本 Service 独占单页 PSRAM 缓冲区、呈现互斥和最近结果

负责读取完整文件、调用 `display_frame_protocol` 校验、构造 GRAY2 图像视图并同步刷新。它不查询集合
Service，不处理按键、导航、重试或网络，也不初始化/深睡显示 Device。

状态文本入口不读取文件：居中入口清白并呈现单行文本；坐标式布局入口在同一个事务中清白、
逐行绘制最多四行文本并只同步全刷一次。该能力不负责决定状态页的产品内容和出现时机。

## 2. 数据流

```text
photo_playback_app
    → present_borrow(本地路径, Manifest 页面元数据)
    → device_sd 完整读取
    → display_frame_protocol 完整校验
    → device_display_blit_borrow
    → device_display_present
```

```text
OTA 更新提示适配器
    → present_ascii_layout_borrow(坐标式文本行数组)
    → device_display_clear
    → device_display_draw_ascii_borrow（逐行）
    → device_display_present（一次）
    → BSP 5x7 ASCII 帧合成
    → 墨水屏物理全刷
```

只有 `device_display_present()` 成功后，上层才能更新当前页面。

图片自动局刷实验已停用：照片和完整页面的变化范围通常较大，帧差异规划及多窗口刷新收益有限。
UC8179 Driver 与 BSP 的黑白多窗口局刷 API 继续保留，后续状态栏或小组件等固定小区域可直接复用；
本 Service 的默认页面路径不调用这些接口。

## 3. 生命周期与并发

- `init → present* → deinit`，不创建 Task。
- 初始化要求显示 Device 已处于 800×480 四灰阶模式。
- 呈现事务通过互斥锁串行化；普通页面在已有事务时立即返回 `ESP_ERR_INVALID_STATE`。
- 状态文本为高优先级提示，最多等待 20 秒让已开始的页面刷新结束，再取得同一呈现所有权。
- 状态查询只进入短临界区，不等待最长约 15 秒的物理刷新。
- 呈现期间拒绝反初始化。

## 4. 内存与失败

固定分配一个 96032 字节 PSRAM 缓冲区，不回退内部 RAM。读取、校验、blit 或物理刷新失败均
原样返回并写入状态；Service 不决定重试或页面回退。

## 5. 依赖与验证

调用 `device_sd`、`protocols`、`device_display` 和 FreeRTOS 同步原语。应验证有效页面、文件缺失、
PPF2 损坏、ASCII 参数、并发呈现和显示超时。公共头文件保持 C ABI。
