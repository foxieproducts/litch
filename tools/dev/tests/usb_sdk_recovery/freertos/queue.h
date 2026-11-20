#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreate(unsigned,unsigned);
BaseType_t xQueueGenericSend(QueueHandle_t,const void*,TickType_t,BaseType_t);
BaseType_t xQueueReceive(QueueHandle_t,void*,TickType_t);
BaseType_t xQueueSendToBackFromISR(QueueHandle_t,const void*,BaseType_t*);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
BaseType_t xQueueReset(QueueHandle_t);
#define xQueueSendToBack(q,p,t) xQueueGenericSend(q,p,t,0)
#ifdef __cplusplus
}
#endif
