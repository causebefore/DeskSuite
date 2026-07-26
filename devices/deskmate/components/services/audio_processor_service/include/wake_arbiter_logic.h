/*
 * 文件职责：唤醒仲裁纯逻辑（busy 抑制 + 冷却窗口）。不依赖 ESP-IDF，可 host 编译测试。
 * 调用方：app_voice 收到 raw 唤醒事件后，用本模块决定是否真正触发对话。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint32_t cooldown_ms;     /* 命中后抑制窗口（ms） */
        uint32_t last_trigger_ms; /* 上次「通过」的时间戳；0 表示从未通过 */
    } wake_arbiter_t;

    /* 初始化仲裁器，设置冷却窗口。last_trigger_ms 置 0（视为远古，首次必过冷却）。 */
    void wake_arbiter_init(wake_arbiter_t *a, uint32_t cooldown_ms);

    /*
 * 处理一次 raw 唤醒检测，返回是否应真正触发对话。
 * - busy 为真（录音/思考/播放中）→ 返回 false，且不更新 last_trigger。
 * - 距上次通过不足 cooldown_ms（含时间回拨）→ 返回 false。
 * - 否则更新 last_trigger_ms 并返回 true。
 * now_ms 由调用方传入（不在内部读时钟，便于 host 测试）。
 */
    bool wake_arbiter_handle(wake_arbiter_t *a, uint32_t now_ms, bool busy);

#ifdef __cplusplus
}
#endif
