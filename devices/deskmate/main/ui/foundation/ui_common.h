/*
 * 文件职责：UI 层共享样式与 LVGL 控件原语，所有页面统一依赖。
 * 主要依赖：ui_platform 字体接口、LVGL。
 * 调用方：各 ui_xxx_page、ui_main（初始化）。
 *
 * 设计与参考项目 ui_service.c 的样式/原语一致，字体统一由 ui_platform 提供。
 * 必须在持 LVGL 锁的上下文中调用 ui_common_init() 一次。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "ui_types.h"

#include <stdint.h>

/** @brief UI 页面宽度（像素） */
#define UI_WIDTH       400

/** @brief UI 页面内容区高度（像素） */
#define UI_BODY_H      268

/** @brief 统一留白与规则线令牌，页面不再自行定义同类常量。 */
#define UI_SPACE_1     4
#define UI_SPACE_2     8
#define UI_SPACE_3     12
#define UI_SPACE_4     16
#define UI_RULE_THIN   1
#define UI_RULE_STRONG 2

/**
 * @brief 初始化共享样式（幂等，持锁调用）
 *
 * @return ESP_OK 样式已初始化或本次初始化成功（幂等）
 */
esp_err_t ui_common_init(void);

/**
 * @brief 在控件树清空后释放共享样式并恢复未初始化状态
 */
void ui_common_deinit(void);

/**
 * @brief 创建 16px 常规中文文本
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_text16_regular(lv_obj_t *parent);

/**
 * @brief 创建 16px 半粗中文文本，用于元数据强调和状态文本
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_text16_semibold(lv_obj_t *parent);

/**
 * @brief 创建 24px 常规中文文本
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_text24_regular(lv_obj_t *parent);

/**
 * @brief 创建 24px 半粗中文文本，用于页面内标题和未读主题
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_text24_semibold(lv_obj_t *parent);

/**
 * @brief 创建 32px 常规中文文本，用于少量英雄信息
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_text32_regular(lv_obj_t *parent);

/**
 * @brief 创建 48px 等宽数字文本
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_num48(lv_obj_t *parent);

/**
 * @brief 创建 16px 半粗反白文本，用于状态徽标和反白标签
 *
 * @param parent 父对象
 * @return lv_obj_t* 新建的标签对象
 */
lv_obj_t *ui_common_new_inverse_text16_semibold(lv_obj_t *parent);

/**
 * @brief [旧 API，待删除] 创建 16px 常规文本标签控件。
 *
 * 所有页面迁移到 ui_common_new_text16_regular() 后删除。
 *
 * @param parent 父对象
 * @return lv_obj_t* 创建的标签对象
 */
lv_obj_t *ui_common_new_text16(lv_obj_t *parent);

/**
 * @brief [旧 API，待删除] 创建 24px 常规文本标签控件。
 *
 * 所有页面迁移到 ui_common_new_text24_regular() 或
 * ui_common_new_text24_semibold() 后删除。
 *
 * @param parent 父对象
 * @return lv_obj_t* 创建的标签对象
 */
lv_obj_t *ui_common_new_text24(lv_obj_t *parent);

/**
 * @brief [旧 API，待删除] 创建 16px 常规反白文本标签控件。
 *
 * 所有页面迁移到 ui_common_new_inverse_text16_semibold() 后删除。
 *
 * @param parent 父对象
 * @return lv_obj_t* 创建的标签对象
 */
lv_obj_t *ui_common_new_inverse_text16(lv_obj_t *parent);

/**
 * @brief 创建纯色规则线或矩形块。
 *
 * @param parent 父对象
 * @param x 左上角 X 坐标
 * @param y 左上角 Y 坐标
 * @param w 宽度
 * @param h 高度
 * @return lv_obj_t* 创建的不可滚动装饰对象
 */
lv_obj_t *ui_common_new_rule(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * @brief [旧 API，待删除] 创建全宽水平分割线。
 *
 * 所有页面迁移到 ui_common_new_rule() 后删除。
 *
 * @param parent 父对象
 * @param y      Y 坐标
 * @return lv_obj_t* 创建的分割线对象
 */
lv_obj_t *ui_common_new_hline(lv_obj_t *parent, int32_t y);

/**
 * @brief [旧 API，待删除] 创建垂直分割线。
 *
 * 所有页面迁移到 ui_common_new_rule() 后删除。
 *
 * @param parent 父对象
 * @param x      X 坐标
 * @param y      Y 坐标
 * @param h      高度
 * @return lv_obj_t* 创建的分割线对象
 */
lv_obj_t *ui_common_new_vline(lv_obj_t *parent, int32_t x, int32_t y, int32_t h);

/**
 * @brief 创建卡片容器
 *
 * @param parent 父对象
 * @param x      X 坐标
 * @param y      Y 坐标
 * @param w      宽度
 * @param h      高度
 * @return lv_obj_t* 创建的卡片对象
 */
lv_obj_t *ui_common_new_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * @brief 设置标签的文本、位置、尺寸和对齐方式
 *
 * @param label 目标标签对象
 * @param text  文本内容
 * @param x     X 坐标
 * @param y     Y 坐标
 * @param w     宽度
 * @param h     高度
 * @param align 文本对齐方式
 */
void ui_common_set_label(lv_obj_t *label, const char *text, int32_t x, int32_t y, int32_t w, int32_t h,
                         lv_text_align_t align);

/* ── 动画原语（必须在持 LVGL 锁的上下文调用） ── */

/**
 * @brief 纯淡入动画：从透明到完全显示
 *
 * @param obj 目标对象
 * @param ms  动画时长（毫秒）
 */
void ui_common_anim_fade_in(lv_obj_t *obj, uint32_t ms);
