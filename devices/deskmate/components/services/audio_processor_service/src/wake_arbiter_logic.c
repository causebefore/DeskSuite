#include "wake_arbiter_logic.h"
#include <stddef.h>

void wake_arbiter_init(wake_arbiter_t *arbiter, uint32_t cooldown_ms)
{
    if (arbiter == NULL)
    {
        return;
    }
    arbiter->cooldown_ms     = cooldown_ms;
    arbiter->last_trigger_ms = 0;
}

bool wake_arbiter_consume_detection(wake_arbiter_t *arbiter, uint32_t now_ms, bool busy)
{
    if (arbiter == NULL)
    {
        return false;
    }
    if (busy)
    {
        return false;
    }
    /* last_trigger_ms == 0 视为从未触发，首次必过冷却。 */
    if (arbiter->last_trigger_ms == 0)
    {
        arbiter->last_trigger_ms = now_ms;
        return true;
    }
    /* 用 int32_t 差值容纳回拨（now < last 时差为负，< cooldown → 抑制）。 */
    int32_t elapsed = (int32_t) (now_ms - arbiter->last_trigger_ms);
    if (elapsed < (int32_t) arbiter->cooldown_ms)
    {
        return false;
    }
    arbiter->last_trigger_ms = now_ms;
    return true;
}
