#include "service/dispatcher.h"

#include <string.h>

#include "protocol/app_frame.h"

void Service_Dispatcher_Init(ServiceDispatcher *dispatcher)
{
    if (dispatcher == 0)
    {
        return;
    }

    memset(dispatcher, 0, sizeof(*dispatcher));
}

void Service_Dispatcher_Register(ServiceDispatcher *dispatcher, uint8_t func_code, ServiceDispatcherHandler handler, void *user_arg)
{
    if (dispatcher == 0)
    {
        return;
    }

    dispatcher->handlers[func_code] = handler;
    dispatcher->handler_user_args[func_code] = user_arg;
}

void Service_Dispatcher_SetDefault(ServiceDispatcher *dispatcher, ServiceDispatcherHandler handler, void *user_arg)
{
    if (dispatcher == 0)
    {
        return;
    }

    dispatcher->default_handler = handler;
    dispatcher->default_user_arg = user_arg;
}

bool Service_Dispatcher_DispatchFrame(ServiceDispatcher *dispatcher, AppCanPort port, const ProtocolAppFrame *frame)
{
    ServiceDispatcherHandler handler;
    void *user_arg;

    if ((dispatcher == 0) || !Protocol_AppFrame_IsValid(frame))
    {
        return false;
    }

    handler = dispatcher->handlers[frame->func_code];
    user_arg = dispatcher->handler_user_args[frame->func_code];
    if (handler != 0)
    {
        return handler(user_arg, port, frame);
    }

    if (dispatcher->default_handler != 0)
    {
        return dispatcher->default_handler(dispatcher->default_user_arg, port, frame);
    }

    return false;
}

bool Service_Dispatcher_DispatchRaw(ServiceDispatcher *dispatcher, AppCanPort port, const uint8_t *payload, uint16_t length)
{
    ProtocolAppFrame frame;

    if ((dispatcher == 0) || !Protocol_AppFrame_Decode(&frame, payload, length))
    {
        return false;
    }

    return Service_Dispatcher_DispatchFrame(dispatcher, port, &frame);
}
