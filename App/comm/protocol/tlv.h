#ifndef PROTOCOL_TLV_H
#define PROTOCOL_TLV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* TLV 类型码。
 * 主要根据《通信协议v2》整理。
 */
typedef enum
{
    PROTOCOL_TLV_LEVEL = 0x01,
    PROTOCOL_TLV_SOURCE = 0x02,
    PROTOCOL_TLV_STATE = 0x03,
    PROTOCOL_TLV_RECOVERABLE = 0x04,
    PROTOCOL_TLV_VALUE = 0x05,
    PROTOCOL_TLV_TEXT = 0x06,
    PROTOCOL_TLV_REASON = 0x08,
    PROTOCOL_TLV_HP_CURRENT = 0x0E,
    PROTOCOL_TLV_HP_MAX = 0x0F,
    PROTOCOL_TLV_POWER_CURRENT = 0x10,
    PROTOCOL_TLV_POWER_LIMIT = 0x11,
    PROTOCOL_TLV_BUFFER_ENERGY = 0x12,
    PROTOCOL_TLV_HEAT_CURRENT = 0x13,
    PROTOCOL_TLV_HEAT_LIMIT = 0x14,
    PROTOCOL_TLV_COOLING_RATE = 0x15,
    PROTOCOL_TLV_SHOOT_ENABLE = 0x1B,
    PROTOCOL_TLV_BULLET_TYPE = 0x1D,
    PROTOCOL_TLV_SHOT_SEQ = 0x1E,
    PROTOCOL_TLV_PROJECTILE_SPEED = 0x1F,
    PROTOCOL_TLV_RESULT = 0x20,
    PROTOCOL_TLV_CMD_INDEX = 0x29,
    PROTOCOL_TLV_PAYLOAD = 0x2A,
    PROTOCOL_TLV_ACK_FUNC = 0x2B
} ProtocolTlvType;

/* TLV 写入器。 */
typedef struct
{
    uint8_t *buffer;
    uint16_t capacity;
    uint16_t length;
} ProtocolTlvWriter;

/* 单个 TLV 的只读视图。 */
typedef struct
{
    uint8_t type;
    uint8_t length;
    const uint8_t *value;
} ProtocolTlvView;

/* TLV 遍历器。 */
typedef struct
{
    const uint8_t *buffer;
    uint16_t length;
    uint16_t offset;
} ProtocolTlvReader;

/* 初始化 TLV 写入器。 */
void Protocol_TlvWriter_Init(ProtocolTlvWriter *writer, uint8_t *buffer, uint16_t capacity);

/* 追加一个原始字节流 TLV。 */
bool Protocol_TlvWriter_AppendBytes(ProtocolTlvWriter *writer, uint8_t type, const uint8_t *value, uint8_t length);

/* 追加一个 U8 TLV。 */
bool Protocol_TlvWriter_AppendU8(ProtocolTlvWriter *writer, uint8_t type, uint8_t value);

/* 追加一个 U16 TLV，小端。 */
bool Protocol_TlvWriter_AppendU16(ProtocolTlvWriter *writer, uint8_t type, uint16_t value);

/* 追加一个 U32 TLV，小端。 */
bool Protocol_TlvWriter_AppendU32(ProtocolTlvWriter *writer, uint8_t type, uint32_t value);

/* 追加一个 UTF-8 字符串 TLV。
 * 说明：
 * - 不会自动附加 '\\0'
 * - 长度按实际字节数计算，最大 255
 */
bool Protocol_TlvWriter_AppendString(ProtocolTlvWriter *writer, uint8_t type, const char *text);

/* 追加一个布尔 TLV，编码为 1 字节：0=false，1=true。 */
bool Protocol_TlvWriter_AppendBool(ProtocolTlvWriter *writer, uint8_t type, bool value);

/* 获取当前 TLV 串总长度。 */
uint16_t Protocol_TlvWriter_GetLength(const ProtocolTlvWriter *writer);

/* 初始化 TLV 遍历器。 */
void Protocol_TlvReader_Init(ProtocolTlvReader *reader, const uint8_t *buffer, uint16_t length);

/* 读取下一个 TLV。 */
bool Protocol_TlvReader_Next(ProtocolTlvReader *reader, ProtocolTlvView *view);

/* 按 U8 读取 TLV。 */
bool Protocol_Tlv_ReadU8(const ProtocolTlvView *view, uint8_t *value);

/* 按 U16 读取 TLV，小端。 */
bool Protocol_Tlv_ReadU16(const ProtocolTlvView *view, uint16_t *value);

/* 按 U32 读取 TLV，小端。 */
bool Protocol_Tlv_ReadU32(const ProtocolTlvView *view, uint32_t *value);

/* 按 bool 读取 TLV。 */
bool Protocol_Tlv_ReadBool(const ProtocolTlvView *view, bool *value);

/* 读取字符串 TLV 的只读视图。
 * @param view        输入 TLV 视图。
 * @param text        输出字符串起始地址，直接指向原始缓冲区。
 * @param text_len    输出字符串字节长度，不包含 '\\0'。
 * 说明：
 * - 本函数不会复制，也不会补 '\\0'。
 * - 如果上层要把它当 C 字符串长期保存，请自行拷贝并补终止符。
 */
bool Protocol_Tlv_ReadStringView(const ProtocolTlvView *view, const char **text, uint8_t *text_len);

/* 在 TLV 串中查找某个类型码的第一个 TLV。 */
bool Protocol_Tlv_Find(const uint8_t *buffer, uint16_t length, uint8_t type, ProtocolTlvView *view);

#ifdef __cplusplus
}
#endif

#endif
