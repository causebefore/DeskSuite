#include "wake_arbiter_logic.h"
#include <stddef.h>

void wake_arbiter_init(wake_arbiter_t *a, uint32_t cooldown_ms)
{
    if (a == NULL)
    {
        return;
    }
    a->cooldown_ms     = cooldown_ms;
    a->last_trigger_ms = 0;
}

bool wake_arbiter_handle(wake_arbiter_t *a, uint32_t now_ms, bool busy)
{
    if (a == NULL)
    {
        return false;
    }
    if (busy)
    {
        return false;
    }
    /* last_trigger_ms == 0 视为从未触发，首次必过冷却。 */
    if (a->last_trigger_ms == 0)
    {
        a->last_trigger_ms = now_ms;
        return true;
    }
    /* 用 int32_t 差值容纳回拨（now < last 时差为负，< cooldown → 抑制）。 */
    int32_t elapsed = (int32_t) (now_ms - a->last_trigger_ms);
    if (elapsed < (int32_t) a->cooldown_ms)
    {
        return false;
    }
    a->last_trigger_ms = now_ms;
    return true;
}
