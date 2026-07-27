# time_sync

`time_sync` 是共享通信层的 SNTP 网络取样组件。它串行拥有 ESP-NETIF 全局 SNTP 客户端，
每次调用完成初始化、等待一个样本、复制 UTC 秒与耗时、清理客户端。

边界如下：

- 只提供网络样本事实，不判断候选时间是否可信；
- 不决定是否需要第二个样本，不实现重试、Timer 或 Task；
- 不读取或写入设备 RTC；
- 不拥有时区、可信锚点或产品校时周期；
- ESP-IDF 收到 SNTP 样本时会先更新 POSIX 墙上时钟，因此调用方应以自身单调可信锚点校验
  候选，不能把临时墙上时钟直接视为可信时间。

PhotoPainter 与 DeskMate 的 `system_clock` 共同使用
`time_sync_sample_sntp_once_copy()`，但继续分别拥有候选范围、大跳变二次确认、RTC 回写及
可信来源发布。
