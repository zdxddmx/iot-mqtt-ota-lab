/* 深睡策略（Deep-sleep / Light-sleep 分级）：无活动计时 + 待发数据与联网状态门控 */
#ifndef POWER_POLICY_H
#define POWER_POLICY_H
#include <stdint.h>

typedef enum { PWR_ACTIVE = 0, PWR_LIGHT_SLEEP, PWR_DEEP_SLEEP } PwrMode;

typedef struct {
    uint32_t light_after_ms;
    uint32_t deep_after_ms;
    uint32_t idle_ms;
    uint32_t sleep_count;
    uint8_t  pending_tx;      /* 有数据待上报 → 不许睡 */
    uint8_t  net_connected;   /* 断网时保持唤醒以便重连 */
} PwrPolicy;

void     Pwr_Init(PwrPolicy *p, uint32_t light_after_ms, uint32_t deep_after_ms);
void     Pwr_OnActivity(PwrPolicy *p);
void     Pwr_SetPendingTx(PwrPolicy *p, uint8_t pending);
void     Pwr_SetNetConnected(PwrPolicy *p, uint8_t connected);
PwrMode  Pwr_Evaluate(PwrPolicy *p, uint32_t elapsed_ms);
uint32_t Pwr_ExpectedWakeMs(const PwrPolicy *p);
#endif
