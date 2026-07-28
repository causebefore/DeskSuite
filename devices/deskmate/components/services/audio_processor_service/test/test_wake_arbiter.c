/* host 单元测试：wake_arbiter 纯逻辑。用 gcc 直接编译运行，不依赖 ESP-IDF。 */
#include <stdio.h>
#include "wake_arbiter_logic.h"

static int failures = 0;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        if (!(cond))                                         \
        {                                                    \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            failures++;                                      \
        }                                                    \
    }                                                        \
    while (0)

int main(void)
{
    wake_arbiter_t a;
    wake_arbiter_init(&a, 2000); /* 冷却 2000ms */

    /* 1. busy 时一律不通过 */
    CHECK(wake_arbiter_consume_detection(&a, 1000, true) == false);

    /* 2. 空闲、首次、过冷却 → 通过 */
    CHECK(wake_arbiter_consume_detection(&a, 1000, false) == true); /* last_trigger=1000 */

    /* 3. 冷却窗口内（now - last < 2000）→ 抑制，即使空闲 */
    CHECK(wake_arbiter_consume_detection(&a, 2999, false) == false); /* 2999-1000=1999 < 2000 */
    CHECK(wake_arbiter_consume_detection(&a, 1000, false) == false); /* 回拨时间也不通过（< last） */

    /* 4. 边界：now - last == 2000 → 通过 */
    CHECK(wake_arbiter_consume_detection(&a, 3000, false) == true); /* 3000-1000=2000 == 2000，last=3000 */

    /* 5. 窗口外 → 通过 */
    CHECK(wake_arbiter_consume_detection(&a, 9999, false) == true); /* last=9999 */

    /* 6. busy 优先于冷却判断（即使过了冷却，busy 仍抑制；且 busy 不更新 last） */
    CHECK(wake_arbiter_consume_detection(&a, 99999, true) == false);
    CHECK(wake_arbiter_consume_detection(&a, 99999, false)
          == true); /* last 未被上一次 busy 改动，仍 9999，过冷却 → 通过 */

    if (failures == 0)
    {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}
