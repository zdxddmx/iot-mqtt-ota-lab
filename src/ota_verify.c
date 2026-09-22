#include "ota_verify.h"
#include <string.h>

uint32_t Ota_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t Rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int Ota_ParseHeader(const uint8_t *header, size_t len, uint32_t *version,
                    uint32_t *image_size, uint32_t *image_crc)
{
    if (header == NULL || len < OTA_HEADER_LEN) return -1;
    if (Rd32(header) != OTA_MAGIC) return -1;
    if (version)    *version    = Rd32(&header[4]);
    if (image_size) *image_size = Rd32(&header[8]);
    if (image_crc)  *image_crc  = Rd32(&header[12]);
    return 0;
}

void Ota_Init(OtaContext *ctx, uint32_t initial_version)
{
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof *ctx);
    ctx->active = 0u;
    ctx->target = 1u;
    ctx->state = OTA_IDLE;
    ctx->version[0] = initial_version;
    ctx->version[1] = 0u;
}

OtaStatus Ota_BeginImage(OtaContext *ctx, const uint8_t *header, uint16_t header_len)
{
    uint32_t version, size, crc;
    if (ctx == NULL || header == NULL) return OTA_ERR_PARAM;
    if (Ota_ParseHeader(header, header_len, &version, &size, &crc) != 0) return OTA_ERR_HEADER;
    if (size == 0u || size > OTA_SLOT_SIZE) return OTA_ERR_SIZE;

    ctx->target = (uint8_t)(ctx->active == 0u ? 1u : 0u);   /* 写入非活动槽 */
    memset(ctx->slot[ctx->target], 0xFF, OTA_SLOT_SIZE);    /* 擦除 */
    memcpy(ctx->header, header, OTA_HEADER_LEN);
    ctx->version[ctx->target] = version;
    ctx->image_size = size;
    ctx->image_crc = crc;
    ctx->received = 0u;
    ctx->running_crc = 0xFFFFFFFFu;                          /* 增量 CRC 初值 */
    ctx->boot_pending = 0u;
    ctx->state = OTA_RECEIVING;
    return OTA_OK;
}

static uint32_t Crc32Update(uint32_t crc, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc;
}

OtaStatus Ota_WriteChunk(OtaContext *ctx, uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (ctx == NULL || data == NULL || len == 0u) return OTA_ERR_PARAM;
    if (ctx->state != OTA_RECEIVING) return OTA_ERR_STATE;
    if (offset != ctx->received) return OTA_ERR_OFFSET;      /* 只接受顺序写入（乱序/重复即拒） */
    if ((uint32_t)len > ctx->image_size - ctx->received) return OTA_ERR_SIZE;

    memcpy(&ctx->slot[ctx->target][ctx->received], data, len);
    ctx->running_crc = Crc32Update(ctx->running_crc, data, len);
    ctx->received += len;
    return OTA_OK;
}

OtaStatus Ota_Finish(OtaContext *ctx)
{
    uint32_t crc;
    if (ctx == NULL) return OTA_ERR_PARAM;
    if (ctx->state != OTA_RECEIVING) return OTA_ERR_STATE;
    if (ctx->received != ctx->image_size) return OTA_ERR_SIZE;
    crc = ctx->running_crc ^ 0xFFFFFFFFu;
    if (crc != ctx->image_crc) {
        ctx->state = OTA_FAILED;                             /* 校验失败 → 不置启动标志 */
        return OTA_ERR_CRC;
    }
    ctx->state = OTA_VERIFIED;
    ctx->boot_pending = 1u;                                  /* 交给 bootloader 试运行 */
    return OTA_OK;
}

OtaStatus Ota_Confirm(OtaContext *ctx)
{
    if (ctx == NULL) return OTA_ERR_PARAM;
    if (ctx->state != OTA_VERIFIED || ctx->boot_pending == 0u) return OTA_ERR_STATE;
    ctx->active = ctx->target;                               /* 新镜像自检通过 → 正式生效 */
    ctx->boot_pending = 0u;
    ctx->state = OTA_IDLE;
    return OTA_OK;
}

OtaStatus Ota_Rollback(OtaContext *ctx)
{
    if (ctx == NULL) return OTA_ERR_PARAM;
    if (ctx->boot_pending == 0u && ctx->state != OTA_FAILED) return OTA_ERR_STATE;
    memset(ctx->slot[ctx->target], 0xFF, OTA_SLOT_SIZE);     /* 丢弃未确认的新镜像 */
    ctx->version[ctx->target] = 0u;
    ctx->target = (uint8_t)(ctx->active == 0u ? 1u : 0u);
    ctx->boot_pending = 0u;
    ctx->state = OTA_ROLLED_BACK;
    return OTA_OK;
}

OtaState  Ota_State(const OtaContext *ctx) { return (ctx == NULL) ? OTA_IDLE : ctx->state; }
uint32_t  Ota_Received(const OtaContext *ctx) { return (ctx == NULL) ? 0u : ctx->received; }
uint32_t  Ota_ActiveVersion(const OtaContext *ctx) { return (ctx == NULL) ? 0u : ctx->version[ctx->active]; }
const uint8_t *Ota_ActiveSlot(const OtaContext *ctx)
{
    return (ctx == NULL) ? NULL : ctx->slot[ctx->active];
}
