#include "protocol/tlv.h"

#include <string.h>

void Protocol_TlvWriter_Init(ProtocolTlvWriter *writer, uint8_t *buffer, uint16_t capacity)
{
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0U;
}

bool Protocol_TlvWriter_AppendBytes(ProtocolTlvWriter *writer, uint8_t type, const uint8_t *value, uint8_t length)
{
    uint16_t required;

    if ((writer == 0) || (writer->buffer == 0))
    {
        return false;
    }

    if ((length > 0U) && (value == 0))
    {
        return false;
    }

    required = (uint16_t)(writer->length + 2U + length);
    if (required > writer->capacity)
    {
        return false;
    }

    writer->buffer[writer->length++] = type;
    writer->buffer[writer->length++] = length;

    for (uint8_t i = 0; i < length; ++i)
    {
        writer->buffer[writer->length++] = value[i];
    }

    return true;
}

bool Protocol_TlvWriter_AppendU8(ProtocolTlvWriter *writer, uint8_t type, uint8_t value)
{
    return Protocol_TlvWriter_AppendBytes(writer, type, &value, 1U);
}

bool Protocol_TlvWriter_AppendU16(ProtocolTlvWriter *writer, uint8_t type, uint16_t value)
{
    uint8_t encoded[2];

    encoded[0] = (uint8_t)(value & 0xFFU);
    encoded[1] = (uint8_t)((value >> 8) & 0xFFU);
    return Protocol_TlvWriter_AppendBytes(writer, type, encoded, 2U);
}

bool Protocol_TlvWriter_AppendU32(ProtocolTlvWriter *writer, uint8_t type, uint32_t value)
{
    uint8_t encoded[4];

    encoded[0] = (uint8_t)(value & 0xFFU);
    encoded[1] = (uint8_t)((value >> 8) & 0xFFU);
    encoded[2] = (uint8_t)((value >> 16) & 0xFFU);
    encoded[3] = (uint8_t)((value >> 24) & 0xFFU);
    return Protocol_TlvWriter_AppendBytes(writer, type, encoded, 4U);
}

bool Protocol_TlvWriter_AppendString(ProtocolTlvWriter *writer, uint8_t type, const char *text)
{
    uint16_t length = 0U;

    if (text == 0)
    {
        return false;
    }

    while ((text[length] != '\0') && (length < 0xFFU))
    {
        length++;
    }

    if (text[length] != '\0')
    {
        return false;
    }

    return Protocol_TlvWriter_AppendBytes(writer, type, (const uint8_t *)text, (uint8_t)length);
}

bool Protocol_TlvWriter_AppendBool(ProtocolTlvWriter *writer, uint8_t type, bool value)
{
    return Protocol_TlvWriter_AppendU8(writer, type, value ? 1U : 0U);
}

uint16_t Protocol_TlvWriter_GetLength(const ProtocolTlvWriter *writer)
{
    if (writer == 0)
    {
        return 0U;
    }

    return writer->length;
}

void Protocol_TlvReader_Init(ProtocolTlvReader *reader, const uint8_t *buffer, uint16_t length)
{
    if (reader == 0)
    {
        return;
    }

    reader->buffer = buffer;
    reader->length = length;
    reader->offset = 0U;
}

bool Protocol_TlvReader_Next(ProtocolTlvReader *reader, ProtocolTlvView *view)
{
    uint16_t next_end;

    if ((reader == 0) || (view == 0) || (reader->buffer == 0))
    {
        return false;
    }

    if ((reader->offset + 2U) > reader->length)
    {
        return false;
    }

    view->type = reader->buffer[reader->offset++];
    view->length = reader->buffer[reader->offset++];
    next_end = (uint16_t)(reader->offset + view->length);
    if (next_end > reader->length)
    {
        return false;
    }

    view->value = &reader->buffer[reader->offset];
    reader->offset = next_end;
    return true;
}

bool Protocol_Tlv_ReadU8(const ProtocolTlvView *view, uint8_t *value)
{
    if ((view == 0) || (value == 0) || (view->length != 1U) || (view->value == 0))
    {
        return false;
    }

    *value = view->value[0];
    return true;
}

bool Protocol_Tlv_ReadU16(const ProtocolTlvView *view, uint16_t *value)
{
    if ((view == 0) || (value == 0) || (view->length != 2U) || (view->value == 0))
    {
        return false;
    }

    *value = (uint16_t)view->value[0] | ((uint16_t)view->value[1] << 8);
    return true;
}

bool Protocol_Tlv_ReadU32(const ProtocolTlvView *view, uint32_t *value)
{
    if ((view == 0) || (value == 0) || (view->length != 4U) || (view->value == 0))
    {
        return false;
    }

    *value = (uint32_t)view->value[0] |
             ((uint32_t)view->value[1] << 8) |
             ((uint32_t)view->value[2] << 16) |
             ((uint32_t)view->value[3] << 24);
    return true;
}

bool Protocol_Tlv_ReadBool(const ProtocolTlvView *view, bool *value)
{
    uint8_t raw;

    if (!Protocol_Tlv_ReadU8(view, &raw) || (value == 0))
    {
        return false;
    }

    *value = (raw != 0U);
    return true;
}

bool Protocol_Tlv_ReadStringView(const ProtocolTlvView *view, const char **text, uint8_t *text_len)
{
    if ((view == 0) || (text == 0) || (text_len == 0))
    {
        return false;
    }

    if ((view->length > 0U) && (view->value == 0))
    {
        return false;
    }

    *text = (const char *)view->value;
    *text_len = view->length;
    return true;
}

bool Protocol_Tlv_Find(const uint8_t *buffer, uint16_t length, uint8_t type, ProtocolTlvView *view)
{
    ProtocolTlvReader reader;
    ProtocolTlvView item;

    if ((buffer == 0) || (view == 0))
    {
        return false;
    }

    Protocol_TlvReader_Init(&reader, buffer, length);
    while (Protocol_TlvReader_Next(&reader, &item))
    {
        if (item.type == type)
        {
            *view = item;
            return true;
        }
    }

    return false;
}
