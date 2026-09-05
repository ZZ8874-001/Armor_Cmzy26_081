#include "protocol/app_frame.h"

#include <string.h>

static bool Protocol_AppFrame_SetTypedData(ProtocolAppFrame *frame, uint8_t data_type, const uint8_t *data, uint8_t data_len)
{
    if (frame == 0)
    {
        return false;
    }

    frame->data_type = data_type;
    return Protocol_AppFrame_SetData(frame, data, data_len);
}

static bool Protocol_AppFrame_CheckScalar(const ProtocolAppFrame *frame, uint8_t expected_type, uint8_t expected_len)
{
    return (frame != 0) &&
           (frame->data_type == expected_type) &&
           (frame->data_len == expected_len);
}

void Protocol_AppFrame_Init(ProtocolAppFrame *frame,
                            uint8_t dst_id,
                            uint8_t src_id,
                            uint16_t module_uid,
                            uint8_t func_code,
                            uint8_t seq,
                            uint8_t data_type)
{
    if (frame == 0)
    {
        return;
    }

    memset(frame, 0, sizeof(*frame));
    frame->dst_id = dst_id;
    frame->src_id = src_id;
    frame->module_uid = module_uid;
    frame->func_code = func_code;
    frame->seq = seq;
    frame->data_type = data_type;
}

bool Protocol_AppFrame_SetData(ProtocolAppFrame *frame, const uint8_t *data, uint8_t data_len)
{
    if ((frame == 0) || (data_len > PROTOCOL_APP_FRAME_MAX_DATA_LENGTH))
    {
        return false;
    }

    if ((data_len > 0U) && (data == 0))
    {
        return false;
    }

    frame->data_len = data_len;
    if ((data_len > 0U) && (data != 0))
    {
        memcpy(frame->data, data, data_len);
    }

    return true;
}

bool Protocol_AppFrame_SetU8(ProtocolAppFrame *frame, uint8_t value)
{
    return Protocol_AppFrame_SetTypedData(frame, PROTOCOL_TYPE_U8, &value, 1U);
}

bool Protocol_AppFrame_SetU16(ProtocolAppFrame *frame, uint16_t value)
{
    uint8_t encoded[2];

    encoded[0] = (uint8_t)(value & 0xFFU);
    encoded[1] = (uint8_t)((value >> 8) & 0xFFU);
    return Protocol_AppFrame_SetTypedData(frame, PROTOCOL_TYPE_U16, encoded, 2U);
}

bool Protocol_AppFrame_SetU32(ProtocolAppFrame *frame, uint32_t value)
{
    uint8_t encoded[4];

    encoded[0] = (uint8_t)(value & 0xFFU);
    encoded[1] = (uint8_t)((value >> 8) & 0xFFU);
    encoded[2] = (uint8_t)((value >> 16) & 0xFFU);
    encoded[3] = (uint8_t)((value >> 24) & 0xFFU);
    return Protocol_AppFrame_SetTypedData(frame, PROTOCOL_TYPE_U32, encoded, 4U);
}

bool Protocol_AppFrame_SetString(ProtocolAppFrame *frame, const char *text)
{
    uint16_t length = 0U;

    if ((frame == 0) || (text == 0))
    {
        return false;
    }

    while ((text[length] != '\0') && (length < PROTOCOL_APP_FRAME_MAX_DATA_LENGTH))
    {
        length++;
    }

    if (text[length] != '\0')
    {
        return false;
    }

    return Protocol_AppFrame_SetTypedData(frame, PROTOCOL_TYPE_STRING, (const uint8_t *)text, (uint8_t)length);
}

bool Protocol_AppFrame_GetU8(const ProtocolAppFrame *frame, uint8_t *value)
{
    if (!Protocol_AppFrame_CheckScalar(frame, PROTOCOL_TYPE_U8, 1U) || (value == 0))
    {
        return false;
    }

    *value = frame->data[0];
    return true;
}

bool Protocol_AppFrame_GetU16(const ProtocolAppFrame *frame, uint16_t *value)
{
    if (!Protocol_AppFrame_CheckScalar(frame, PROTOCOL_TYPE_U16, 2U) || (value == 0))
    {
        return false;
    }

    *value = (uint16_t)frame->data[0] | ((uint16_t)frame->data[1] << 8);
    return true;
}

bool Protocol_AppFrame_GetU32(const ProtocolAppFrame *frame, uint32_t *value)
{
    if (!Protocol_AppFrame_CheckScalar(frame, PROTOCOL_TYPE_U32, 4U) || (value == 0))
    {
        return false;
    }

    *value = (uint32_t)frame->data[0] |
             ((uint32_t)frame->data[1] << 8) |
             ((uint32_t)frame->data[2] << 16) |
             ((uint32_t)frame->data[3] << 24);
    return true;
}

bool Protocol_AppFrame_Encode(const ProtocolAppFrame *frame, uint8_t *buffer, uint16_t *encoded_len, uint16_t buffer_size)
{
    uint16_t total_len;

    if ((frame == 0) || (buffer == 0) || (encoded_len == 0))
    {
        return false;
    }

    total_len = (uint16_t)(PROTOCOL_APP_FRAME_HEADER_SIZE + frame->data_len);
    if ((frame->data_len > PROTOCOL_APP_FRAME_MAX_DATA_LENGTH) || (buffer_size < total_len))
    {
        return false;
    }

    buffer[0] = frame->dst_id;
    buffer[1] = frame->src_id;
    buffer[2] = (uint8_t)(frame->module_uid & 0xFFU);
    buffer[3] = (uint8_t)((frame->module_uid >> 8) & 0xFFU);
    buffer[4] = frame->func_code;
    buffer[5] = frame->seq;
    buffer[6] = frame->data_type;
    buffer[7] = frame->data_len;

    if ((frame->data_len > 0U) && (frame->data_len <= PROTOCOL_APP_FRAME_MAX_DATA_LENGTH))
    {
        memcpy(&buffer[8], frame->data, frame->data_len);
    }

    *encoded_len = total_len;
    return true;
}

bool Protocol_AppFrame_Decode(ProtocolAppFrame *frame, const uint8_t *buffer, uint16_t buffer_len)
{
    uint8_t data_len;

    if ((frame == 0) || (buffer == 0) || (buffer_len < PROTOCOL_APP_FRAME_HEADER_SIZE))
    {
        return false;
    }

    data_len = buffer[7];
    if ((uint16_t)(PROTOCOL_APP_FRAME_HEADER_SIZE + data_len) != buffer_len)
    {
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    frame->dst_id = buffer[0];
    frame->src_id = buffer[1];
    frame->module_uid = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
    frame->func_code = buffer[4];
    frame->seq = buffer[5];
    frame->data_type = buffer[6];
    frame->data_len = data_len;

    if (data_len > 0U)
    {
        memcpy(frame->data, &buffer[8], data_len);
    }

    return true;
}

bool Protocol_AppFrame_RequiresAck(const ProtocolAppFrame *frame)
{
    return (frame != 0) && (frame->seq != 0U);
}

bool Protocol_AppFrame_IsValid(const ProtocolAppFrame *frame)
{
    if (frame == 0)
    {
        return false;
    }

    if (frame->data_len > PROTOCOL_APP_FRAME_MAX_DATA_LENGTH)
    {
        return false;
    }

    return true;
}
