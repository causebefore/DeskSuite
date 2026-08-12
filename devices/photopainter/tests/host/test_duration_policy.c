#include <assert.h>
#include <stdint.h>

#include "utils.h"

int main(void)
{
    const int64_t threshold = 3000000LL;
    assert(!utils_duration_reached_us(0LL, threshold, threshold));
    assert(!utils_duration_reached_us(10LL, 9LL, threshold));
    assert(!utils_duration_reached_us(10LL, 3000009LL, threshold));
    assert(utils_duration_reached_us(10LL, 3000010LL, threshold));
    assert(utils_duration_reached_us(10LL, 4000010LL, threshold));
    return 0;
}
