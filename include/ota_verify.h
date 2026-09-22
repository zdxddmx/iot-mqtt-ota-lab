/* OTA 镜像校验与 A/B 分区升级状态机（主机端可测）
 * 覆盖：镜像头校验 → 顺序写入（拒绝乱序/越界）→ 全镜像 CRC32 → 启动标志 → 确认或回滚。*/
#ifndef OTA_VERIFY_H
#define OTA_VERIFY_H
#include <stdint.h>
#include <stddef.h>

#define OTA_MAGIC      0x4F544131u     /* "OTA1" */
#define OTA_HEADER_LEN 16u
#define OTA_SLOT_SIZE  4096u

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
    OTA_VERIFIED,
    OTA_FAILED,
    OTA_ROLLED_BACK
} OtaState;

typedef enum {
    OTA_OK = 0,
    OTA_ERR_PARAM,
    OTA_ERR_HEADER,
    OTA_ERR_OFFSET,
    OTA_ERR_SIZE,
    OTA_ERR_CRC,
    OTA_ERR_STATE
} OtaStatus;

typedef struct {
    uint8_t  slot[2][OTA_SLOT_SIZE];
    uint8_t  active;                   /* 当前运行槽 */
    uint8_t  target;                   /* 本次写入槽 */
    uint8_t  boot_pending;             /* 新镜像已就绪、等待自检确认 */
    OtaState state;
    uint32_t version[2];
    uint32_t image_size;
    uint32_t image_crc;
    uint32_t received;
    uint32_t running_crc;
    uint8_t  header[OTA_HEADER_LEN];
} OtaContext;

uint32_t Ota_Crc32(const uint8_t *data, uint32_t len);
int      Ota_ParseHeader(const uint8_t *header, size_t len, uint32_t *version,
                         uint32_t *image_size, uint32_t *image_crc);

void      Ota_Init(OtaContext *ctx, uint32_t initial_version);
OtaStatus Ota_BeginImage(OtaContext *ctx, const uint8_t *header, uint16_t header_len);
OtaStatus Ota_WriteChunk(OtaContext *ctx, uint32_t offset, const uint8_t *data, uint16_t len);
OtaStatus Ota_Finish(OtaContext *ctx);
OtaStatus Ota_Confirm(OtaContext *ctx);
OtaStatus Ota_Rollback(OtaContext *ctx);
OtaState  Ota_State(const OtaContext *ctx);
uint32_t  Ota_Received(const OtaContext *ctx);
uint32_t  Ota_ActiveVersion(const OtaContext *ctx);
const uint8_t *Ota_ActiveSlot(const OtaContext *ctx);
#endif
