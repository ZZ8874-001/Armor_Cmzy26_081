#ifndef PROTOCOL_APP_FRAME_H
#define PROTOCOL_APP_FRAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* 应用层固定头长度，对应《通信协议v2》的 8 字节帧头。 */
#define PROTOCOL_APP_FRAME_HEADER_SIZE       8U
/* 数据长度字段为 1 字节，因此应用层数据区上限是 255 字节。 */
#define PROTOCOL_APP_FRAME_MAX_DATA_LENGTH   255U
/* 完整应用层消息最大编码长度 = 8 + 255 = 263 字节。 */
#define PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE  (PROTOCOL_APP_FRAME_HEADER_SIZE + PROTOCOL_APP_FRAME_MAX_DATA_LENGTH)

/* 调度优先级。
 * 该优先级供调度器选消息时使用，不直接映射到 CAN 仲裁优先级。
 */
typedef enum
{
    PROTOCOL_PRIORITY_LOW = 0,
    PROTOCOL_PRIORITY_MEDIUM,
    PROTOCOL_PRIORITY_HIGH,
    PROTOCOL_PRIORITY_HIGHEST
} ProtocolPriority;

/* 应用层数据类型码。 */
typedef enum
{
    PROTOCOL_TYPE_U8 = 0x01,
    PROTOCOL_TYPE_U16 = 0x02,
    PROTOCOL_TYPE_U32 = 0x03,
    PROTOCOL_TYPE_BYTES = 0x08,
    PROTOCOL_TYPE_STRING = 0x0CU,
    PROTOCOL_TYPE_TLV_STREAM = 0x0DU
} ProtocolDataType;

/* 功能码枚举。
 * 根据《通信协议v2》整理了当前常用与已预留的功能码。
 */
typedef enum
{
    PROTOCOL_MSG_HEARTBEAT = 0x01,
    PROTOCOL_MSG_STATUS_REPORT = 0x02,
    PROTOCOL_MSG_INIT_STATE = 0x03,
    PROTOCOL_MSG_CALIBRATION_STATE = 0x04,
    PROTOCOL_MSG_ERROR_REPORT = 0x05,
    PROTOCOL_MSG_ACK = 0x06,
    PROTOCOL_MSG_GAME_STATE = 0x11,
    PROTOCOL_MSG_STAGE_TIME = 0x12,
    PROTOCOL_MSG_GAME_RESULT = 0x13,
    PROTOCOL_MSG_REFEREE_WARNING = 0x14,
    PROTOCOL_MSG_ROBOT_BASIC_STATE = 0x21,
    PROTOCOL_MSG_HP_STATE = 0x22,
    PROTOCOL_MSG_POWER_STATE = 0x23,
    PROTOCOL_MSG_HEAT_STATE = 0x24,
    PROTOCOL_MSG_SHOOT_ALLOWANCE_STATE = 0x25,
    PROTOCOL_MSG_POSITION_STATE = 0x26,
    PROTOCOL_MSG_MODE_STATE = 0x27,
    PROTOCOL_MSG_BARREL_STATE = 0x28,
    PROTOCOL_MSG_HIT_EVENT = 0x31,
    PROTOCOL_MSG_SHOOT_EVENT = 0x32,
    PROTOCOL_MSG_DEATH_EVENT = 0x34,
    PROTOCOL_MSG_REVIVE_EVENT = 0x35,
    PROTOCOL_MSG_HP_UPDATE_EVENT = 0x36,
    PROTOCOL_MSG_POWER_LIMIT_CMD = 0x41,
    PROTOCOL_MSG_HEAT_LIMIT_CMD = 0x42,
    PROTOCOL_MSG_SHOOT_ENABLE_CMD = 0x43,
    PROTOCOL_MSG_CHASSIS_ENABLE_CMD = 0x44,
    PROTOCOL_MSG_GIMBAL_ENABLE_CMD = 0x45,
    PROTOCOL_MSG_SYSTEM_MODE_CMD = 0x46,
    PROTOCOL_MSG_LAUNCHER_CTRL_CMD = 0x51,
    PROTOCOL_MSG_DEBUG_READ_REQ = 0x61
} ProtocolFuncCode;

/* 完整应用层帧，是应用层最常直接使用的消息容器。 */
typedef struct
{
    uint8_t dst_id;
    uint8_t src_id;
    uint16_t module_uid;
    uint8_t func_code;
    uint8_t seq;
    uint8_t data_type;
    uint8_t data_len;
    uint8_t data[PROTOCOL_APP_FRAME_MAX_DATA_LENGTH];
} ProtocolAppFrame;

/* 初始化应用层帧头。 */
void Protocol_AppFrame_Init(ProtocolAppFrame *frame,
                            uint8_t dst_id,
                            uint8_t src_id,
                            uint16_t module_uid,
                            uint8_t func_code,
                            uint8_t seq,
                            uint8_t data_type);

/* 设置数据区。 */
bool Protocol_AppFrame_SetData(ProtocolAppFrame *frame, const uint8_t *data, uint8_t data_len);

/* 将单个 U8 值写入应用层数据区，并设置数据类型为 PROTOCOL_TYPE_U8。 */
bool Protocol_AppFrame_SetU8(ProtocolAppFrame *frame, uint8_t value);

/* 将单个 U16 值写入应用层数据区，并设置数据类型为 PROTOCOL_TYPE_U16。 */
bool Protocol_AppFrame_SetU16(ProtocolAppFrame *frame, uint16_t value);

/* 将单个 U32 值写入应用层数据区，并设置数据类型为 PROTOCOL_TYPE_U32。 */
bool Protocol_AppFrame_SetU32(ProtocolAppFrame *frame, uint32_t value);

/* 将 UTF-8 字符串写入应用层数据区，并设置数据类型为 PROTOCOL_TYPE_STRING。
 * 说明：
 * - 不会自动附加 '\\0'
 * - 传输长度按实际字节数计算
 */
bool Protocol_AppFrame_SetString(ProtocolAppFrame *frame, const char *text);

/* 从应用层数据区读取单个 U8。 */
bool Protocol_AppFrame_GetU8(const ProtocolAppFrame *frame, uint8_t *value);

/* 从应用层数据区读取单个 U16。 */
bool Protocol_AppFrame_GetU16(const ProtocolAppFrame *frame, uint16_t *value);

/* 从应用层数据区读取单个 U32。 */
bool Protocol_AppFrame_GetU32(const ProtocolAppFrame *frame, uint32_t *value);

/* 将应用层帧编码为字节流。 */
bool Protocol_AppFrame_Encode(const ProtocolAppFrame *frame, uint8_t *buffer, uint16_t *encoded_len, uint16_t buffer_size);

/* 从完整字节流解码应用层帧。 */
bool Protocol_AppFrame_Decode(ProtocolAppFrame *frame, const uint8_t *buffer, uint16_t buffer_len);

/* 判断该帧是否要求 ACK。
 * 当前约定：
 * - seq == 0 表示不要求 ACK
 * - seq != 0 表示要求 ACK
 */
bool Protocol_AppFrame_RequiresAck(const ProtocolAppFrame *frame);

/* 对帧结构做轻量合法性检查。 */
bool Protocol_AppFrame_IsValid(const ProtocolAppFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
