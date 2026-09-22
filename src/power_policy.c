#include "power_policy.h"

void Pwr_Init(PwrPolicy *p, uint32_t light_after_ms, uint32_t deep_after_ms)
{
    if (p == NULL) return;
    p->light_after_ms = light_after_ms;
    p->deep_after_ms = deep_after_ms;
    p->idle_ms = 0u;
    p->sleep_count = 0u;
    p->pending_tx = 0u;
    p->net_connected = 1u;
}

void Pwr_OnActivity(PwrPolicy *p) { if (p) p->idle_ms = 0u; }
void Pwr_SetPendingTx(PwrPolicy *p, uint8_t pending) { if (p) p->pending_tx = pending ? 1u : 0u; }
void Pwr_SetNetConnected(PwrPolicy *p, uint8_t connected) { if (p) p->net_connected = connected ? 1u : 0u; }

PwrMode Pwr_Evaluate(PwrPolicy *p, uint32_t elapsed_ms)
{
    if (p == NULL) return PWR_ACTIVE;
    p->idle_ms += elapsed_ms;
    if (p->pending_tx || !p->net_connected) return PWR_ACTIVE;   /* 有活要干 → 不许睡 */
    if (p->idle_ms >= p->deep_after_ms) {
        p->sleep_count++;
        p->idle_ms = 0u;
        return PWR_DEEP_SLEEP;
    }
    if (p->idle_ms >= p->light_after_ms) return PWR_LIGHT_SLEEP;
    return PWR_ACTIVE;
}

uint32_t Pwr_ExpectedWakeMs(const PwrPolicy *p)
{
    if (p == NULL) return 0u;
    if (p->pending_tx) return 0u;                    /* 立即唤醒处理 */
    if (!p->net_connected) return 200u;              /* 断网重连周期 */
    return 5000u;                                    /* 心跳上报周期 */
}
