#ifndef PROTOCOL_APP_ACK_H
#define PROTOCOL_APP_ACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "protocol/app_frame.h"

/* ACK 结果码。
 * 对应《通信协议v2》中建议的 ACK 语义。
 */
typedef enum
{
    PROTOCOL_ACK_RESULT_OK = 0x00,
    PROTOCOL_ACK_RESULT_REJECTED_BY_STATE = 0x01,
    PROTOCOL_ACK_RESULT_UNSUPPORTED = 0x02,
    PROTOCOL_ACK_RESULT_BAD_FORMAT = 0x03,
    PROTOCOL_ACK_RESULT_FORBIDDEN = 0x05
} ProtocolAckResult;

/* ACK 解析结果。 */
typedef struct
{
    uint8_t acked_func_code;
    uint8_t result_code;
    uint8_t reason_code;
    bool has_ack_func;
    bool has_reason;
} ProtocolAppAckInfo;

/* 根据原请求构造一条 ACK 帧。 */
bool Protocol_AppAck_Build(const ProtocolAppFrame *request,
                           uint8_t result_code,
                           bool has_reason,
                           uint8_t reason_code,
                           ProtocolAppFrame *ack_frame);

/* 解析 ACK 中的关键 TLV 字段。 */
bool Protocol_AppAck_Parse(const ProtocolAppFrame *ack_frame, ProtocolAppAckInfo *ack_info);

/* 判断 ACK 是否匹配原请求。 */
bool Protocol_AppAck_MatchesRequest(const ProtocolAppFrame *request, const ProtocolAppFrame *ack_frame, ProtocolAppAckInfo *ack_info);

#ifdef __cplusplus
}
#endif

#endif
