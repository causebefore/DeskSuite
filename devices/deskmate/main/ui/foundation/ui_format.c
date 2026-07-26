/*
 * 文件职责：实现纯格式化函数。
 * 主要依赖：Presentation View Model 类型、标准库。
 * 调用方：主页、系统页。
 */
#include "ui_format.h"

#include <stdio.h>
#include <stdlib.h>

void ui_format_time(const home_time_view_model_t *time, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    if (time == NULL || time->status != PRESENTATION_DATA_OK)
    {
        snprintf(out, out_len, "--:--");
        return;
    }
    snprintf(out, out_len, "%02u:%02u", (unsigned) time->hour, (unsigned) time->minute);
}

void ui_format_uptime(uint32_t sec, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    uint32_t hours   = sec / 3600;
    uint32_t minutes = (sec % 3600) / 60;
    if (hours > 0)
    {
        snprintf(out, out_len, "%luh %lum", (unsigned long) hours, (unsigned long) minutes);
    }
    else
    {
        snprintf(out, out_len, "%lum", (unsigned long) minutes);
    }
}

void ui_format_memory(uint32_t free_kb, uint32_t total_kb, uint8_t used_percent, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    if (total_kb == 0)
    {
        snprintf(out, out_len, "--");
        return;
    }
    /* used_percent 是"已使用"占比（system_info = used/total），
     * 故容量也按 used/total 显示并加"已使用"前缀，避免"剩余/总量 已使用 75%"这种
     * 数字与标注矛盾的读法。 */
    const uint32_t used_kb = total_kb - free_kb;
    if (total_kb >= 1024)
    {
        snprintf(out,
                 out_len,
                 "已使用 %lu.%luM/%lu.%luM %u%%",
                 (unsigned long) (used_kb / 1024),
                 (unsigned long) ((used_kb % 1024) * 10 / 1024),
                 (unsigned long) (total_kb / 1024),
                 (unsigned long) ((total_kb % 1024) * 10 / 1024),
                 (unsigned) used_percent);
    }
    else
    {
        snprintf(out,
                 out_len,
                 "已使用 %luK/%luK %u%%",
                 (unsigned long) used_kb,
                 (unsigned long) total_kb,
                 (unsigned) used_percent);
    }
}

void ui_format_rssi(int8_t dbm, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    snprintf(out, out_len, "%ddBm", (int) dbm);
}

const char *ui_format_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    /* Sakamoto 算法: 结果 0=周日 .. 6=周六; 月份表已含闰年修正。
     * 1/2 月按上一年 13/14 月算, 所以先把年份减一。 */
    static const char   *names[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const uint8_t t[12]    = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (year == 0 || month == 0 || month > 12 || day == 0 || day > 31)
    {
        return "";
    }
    uint16_t y = year;
    if (month < 3)
    {
        y -= 1;
    }
    uint32_t dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
    return names[dow];
}

void ui_format_home_date(const home_time_view_model_t *time, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    if (time == NULL || time->status != PRESENTATION_DATA_OK)
    {
        snprintf(out, out_len, "--");
        return;
    }
    snprintf(out,
             out_len,
             "%u月%u日 · %s",
             (unsigned) time->month,
             (unsigned) time->day,
             ui_format_weekday(time->year, time->month, time->day));
}
