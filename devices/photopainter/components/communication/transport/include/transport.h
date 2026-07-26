/**
 * @file transport.h
 * @brief 聚合 Transport 组件的 HTTP 与 WebSocket 公共接口
 *
 * 这是其他组件使用 Transport 能力的唯一公共入口。Transport 组件内部实现可继续直接包含
 * 对应的子接口头文件。
 */
#pragma once

#include "transport_http.h"
#include "transport_websocket.h"
