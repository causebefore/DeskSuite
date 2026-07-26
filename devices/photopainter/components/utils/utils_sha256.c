/**
 * @file utils_sha256.c
 * @brief 实现项目通用的 SHA-256 摘要工具
 */

#include "utils.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "psa/crypto.h"

/** @brief 日志标签 */
static const char *TAG = "utils_sha256";

/** @brief SHA-256 流式计算上下文实现 */
struct utils_sha256_context
{
    psa_hash_operation_t operation; /**< PSA 哈希操作对象 */
    bool                 active;    /**< 是否仍可继续写入数据 */
};

/**
 * @brief 创建 SHA-256 流式计算上下文
 *
 * @param[out] out_context 新建上下文输出指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 内存不足；或加密库错误码
 */
esp_err_t utils_sha256_create(utils_sha256_context_t **out_context)
{
    ESP_RETURN_ON_FALSE(out_context != NULL, ESP_ERR_INVALID_ARG, TAG, "SHA-256 上下文输出为空");
    *out_context = NULL;

    if (psa_crypto_init() != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "初始化 PSA 加密库失败");
        return ESP_FAIL;
    }

    utils_sha256_context_t *context = calloc(1U, sizeof(*context));
    if (context == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /* calloc 已将操作对象清零，这与 PSA_HASH_OPERATION_INIT 的初始状态一致。
     * PSA_HASH_OPERATION_INIT 是声明初始化器，不能直接用于赋值。 */
    if (psa_hash_setup(&context->operation, PSA_ALG_SHA_256) != PSA_SUCCESS)
    {
        free(context);
        ESP_LOGE(TAG, "创建 SHA-256 操作失败");
        return ESP_FAIL;
    }

    context->active = true;
    *out_context    = context;
    return ESP_OK;
}

/**
 * @brief 向 SHA-256 上下文写入一段数据
 *
 * @param[in,out] context 已创建且尚未完成的 SHA-256 上下文
 * @param[in] data 待写入数据
 * @param[in] length 数据长度
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 上下文已完成；或加密库错误码
 */
esp_err_t utils_sha256_update(utils_sha256_context_t *context, const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_FALSE(context != NULL && (data != NULL || length == 0U),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SHA-256 输入参数无效");
    ESP_RETURN_ON_FALSE(context->active, ESP_ERR_INVALID_STATE, TAG, "SHA-256 上下文已完成");
    if (length == 0U)
    {
        return ESP_OK;
    }
    if (psa_hash_update(&context->operation, data, length) != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "更新 SHA-256 失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 完成 SHA-256 流式计算并写出摘要
 *
 * @param[in,out] context 已创建且尚未完成的 SHA-256 上下文
 * @param[out] digest SHA-256 摘要缓冲区
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 上下文已完成；或加密库错误码
 */
esp_err_t utils_sha256_final(utils_sha256_context_t *context,
                             uint8_t                 digest[UTILS_SHA256_DIGEST_SIZE])
{
    ESP_RETURN_ON_FALSE(context != NULL && digest != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SHA-256 输出参数无效");
    ESP_RETURN_ON_FALSE(context->active, ESP_ERR_INVALID_STATE, TAG, "SHA-256 上下文已完成");

    size_t             digest_length = 0U;
    const psa_status_t status =
        psa_hash_finish(&context->operation, digest, UTILS_SHA256_DIGEST_SIZE, &digest_length);
    context->active = false;
    if (status != PSA_SUCCESS || digest_length != UTILS_SHA256_DIGEST_SIZE)
    {
        ESP_LOGE(TAG, "完成 SHA-256 失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 释放 SHA-256 流式计算上下文
 *
 * @param[in] context 待释放上下文；可为空指针
 */
void utils_sha256_destroy(utils_sha256_context_t *context)
{
    if (context == NULL)
    {
        return;
    }
    if (context->active)
    {
        (void) psa_hash_abort(&context->operation);
    }
    free(context);
}

/**
 * @brief 一次性计算数据的 SHA-256 摘要
 *
 * @param[in] data 待计算数据
 * @param[in] length 数据长度
 * @param[out] digest SHA-256 摘要缓冲区
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 内存不足；或加密库错误码
 */
esp_err_t utils_sha256(const uint8_t *data, size_t length, uint8_t digest[UTILS_SHA256_DIGEST_SIZE])
{
    ESP_RETURN_ON_FALSE((data != NULL || length == 0U) && digest != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SHA-256 输入参数无效");

    utils_sha256_context_t *context = NULL;
    esp_err_t               err     = utils_sha256_create(&context);
    if (err == ESP_OK)
    {
        err = utils_sha256_update(context, data, length);
    }
    if (err == ESP_OK)
    {
        err = utils_sha256_final(context, digest);
    }
    utils_sha256_destroy(context);
    return err;
}
