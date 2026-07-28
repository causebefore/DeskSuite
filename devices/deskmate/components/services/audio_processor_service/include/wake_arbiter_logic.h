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

    /** @brief 唤醒检测认领所使用的冷却窗口运行数据 */
    typedef struct
    {
        uint32_t cooldown_ms;     /**< 命中后的抑制窗口，单位毫秒 */
        uint32_t last_trigger_ms; /**< 上次通过的时间戳；0 表示从未通过 */
    } wake_arbiter_t;

    /**
     * @brief 初始化唤醒仲裁器
     *
     * @param[out] arbiter 仲裁器运行数据
     * @param[in] cooldown_ms 命中后的抑制窗口，单位毫秒
     */
    void wake_arbiter_init(wake_arbiter_t *arbiter, uint32_t cooldown_ms);

    /**
     * @brief 认领并解释一次原始唤醒检测事实
     *
     * busy 为 true 或尚在冷却窗口内时不认领，也不更新通过时间；其余场景记录本次通过。
     *
     * @param[in,out] arbiter 仲裁器运行数据
     * @param[in] now_ms 调用方提供的当前单调时间，单位毫秒
     * @param[in] busy 当前是否已有录音、思考或播放事务
     * @return true 应触发一次对话；false 本次检测被抑制
     */
    bool wake_arbiter_consume_detection(wake_arbiter_t *arbiter, uint32_t now_ms, bool busy);

#ifdef __cplusplus
}
#endif
