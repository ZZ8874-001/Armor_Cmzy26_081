#include "protocol/app_ack.h"

#include "protocol/tlv.h"

bool Protocol_AppAck_Build(const ProtocolAppFrame *request,
                           uint8_t result_code,
                           bool has_reason,
                           uint8_t reason_code,
                           ProtocolAppFrame *ack_frame)
{
    ProtocolTlvWriter writer;
    uint8_t tlv_buffer[16];

    if ((request == 0) || (ack_frame == 0))
    {
        return false;
    }

    Protocol_AppFrame_Init(ack_frame,
                           request->src_id,
                           request->dst_id,
                           request->module_uid,
                           PROTOCOL_MSG_ACK,
                           request->seq,
                           PROTOCOL_TYPE_TLV_STREAM);

    Protocol_TlvWriter_Init(&writer, tlv_buffer, sizeof(tlv_buffer));
    if (!Protocol_TlvWriter_AppendU8(&writer, PROTOCOL_TLV_ACK_FUNC, request->func_code))
    {
        return false;
    }

    if (!Protocol_TlvWriter_AppendU8(&writer, PROTOCOL_TLV_RESULT, result_code))
    {
        return false;
    }

    if (has_reason && !Protocol_TlvWriter_AppendU8(&writer, PROTOCOL_TLV_REASON, reason_code))
    {
        return false;
    }

    return Protocol_AppFrame_SetData(ack_frame, tlv_buffer, (uint8_t)Protocol_TlvWriter_GetLength(&writer));
}

bool Protocol_AppAck_Parse(const ProtocolAppFrame *ack_frame, ProtocolAppAckInfo *ack_info)
{
    ProtocolTlvReader reader;
    ProtocolTlvView view;
    bool has_result = false;

    if ((ack_frame == 0) || (ack_info == 0) || (ack_frame->func_code != PROTOCOL_MSG_ACK))
    {
        return false;
    }

    ack_info->acked_func_code = 0U;
    ack_info->result_code = 0U;
    ack_info->reason_code = 0U;
    ack_info->has_ack_func = false;
    ack_info->has_reason = false;

    Protocol_TlvReader_Init(&reader, ack_frame->data, ack_frame->data_len);
    while (Protocol_TlvReader_Next(&reader, &view))
    {
        if (view.type == PROTOCOL_TLV_ACK_FUNC)
        {
            ack_info->has_ack_func = Protocol_Tlv_ReadU8(&view, &ack_info->acked_func_code);
        }
        else if (view.type == PROTOCOL_TLV_RESULT)
        {
            has_result = Protocol_Tlv_ReadU8(&view, &ack_info->result_code);
        }
        else if (view.type == PROTOCOL_TLV_REASON)
        {
            ack_info->has_reason = Protocol_Tlv_ReadU8(&view, &ack_info->reason_code);
        }
    }

    return has_result;
}

bool Protocol_AppAck_MatchesRequest(const ProtocolAppFrame *request, const ProtocolAppFrame *ack_frame, ProtocolAppAckInfo *ack_info)
{
    ProtocolAppAckInfo local_info;

    if ((request == 0) || (ack_frame == 0))
    {
        return false;
    }

    if ((ack_frame->func_code != PROTOCOL_MSG_ACK) || (ack_frame->seq != request->seq))
    {
        return false;
    }

    if (!Protocol_AppAck_Parse(ack_frame, &local_info))
    {
        return false;
    }

    if (local_info.has_ack_func && (local_info.acked_func_code != request->func_code))
    {
        return false;
    }

    if (ack_info != 0)
    {
        *ack_info = local_info;
    }

    return true;
}
