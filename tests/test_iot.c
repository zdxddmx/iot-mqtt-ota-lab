/* 自检测试：make test 直接跑，输出 PASS/FAIL 与汇总。
 * 覆盖 MQTT 变长长度边界、PUBLISH 往返、报文长度不一致、CONNACK/PUBACK/PINGREQ 字节序、
 * QoS1 包 ID 分配与窗口背压，以及 OTA 正常升级 / 坏 CRC / 乱序 / 越界 / 回滚 / 续传、深睡策略。*/
#include <stdio.h>
#include <string.h>
#include "mqtt_codec.h"
#include "ota_verify.h"
#include "power_policy.h"

static int g_run, g_fail;

static void Group(const char *name) { printf("\n[%s]\n", name); }

static void Check(int cond, const char *msg)
{
    g_run++;
    if (cond) printf("  PASS  %s\n", msg);
    else { g_fail++; printf("  FAIL  %s\n", msg); }
}

static void TestRemainingLength(void)
{
    uint8_t buf[4];
    uint32_t value = 0u;
    size_t used = 0u;
    struct { uint32_t v; int bytes; } cases[] = {
        { 0u, 1 }, { 127u, 1 }, { 128u, 2 }, { 16383u, 2 },
        { 16384u, 3 }, { 2097151u, 3 }, { 2097152u, 4 }, { 268435455u, 4 }
    };

    Group("MQTT 剩余长度（变长整数）编解码");
    for (unsigned i = 0u; i < (unsigned)(sizeof cases / sizeof cases[0]); i++) {
        int n = Mqtt_EncodeRemainingLength(cases[i].v, buf, sizeof buf);
        char msg[96];
        sprintf(msg, "长度 %u 编码为 %d 字节", cases[i].v, cases[i].bytes);
        Check(n == cases[i].bytes, msg);
        if (Mqtt_DecodeRemainingLength(buf, (size_t)n, &value, &used) == 0) {
            sprintf(msg, "长度 %u 解码还原一致", cases[i].v);
            Check(value == cases[i].v && used == (size_t)n, msg);
        } else {
            Check(0, "长度解码失败");
        }
    }
    Check(Mqtt_EncodeRemainingLength(268435456u, buf, sizeof buf) < 0, "超过 4 字节上限 → 编码失败");
    Check(Mqtt_DecodeRemainingLength((const uint8_t *)"\x80\x80\x80\x80\x01", 5u, &value, &used) < 0,
          "非法 5 字节变长长度 → 解码失败");
}

static void TestPublish(void)
{
    uint8_t out[MQTT_MAX_PACKET];
    MqttPacket pkt;
    size_t out_len = 0u, consumed = 0u;
    const uint8_t payload[5] = { 0x01u, 0x02u, 0x03u, 0xFEu, 0xFFu };

    Group("MQTT PUBLISH 编解码（QoS0 / QoS1）");
    Check(Mqtt_EncodePublish("dev/1/up", payload, 5u, 0u, 0u, 0u, out, sizeof out, &out_len) == MQTT_OK,
          "QoS0 PUBLISH 编码成功");
    Check(Mqtt_Decode(out, out_len, &pkt, &consumed) == MQTT_OK, "QoS0 PUBLISH 解码成功");
    Check(pkt.type == MQTT_TYPE_PUBLISH && pkt.qos == 0u, "报文类型与 QoS 正确");
    Check(strcmp(pkt.topic, "dev/1/up") == 0 && pkt.payload_len == 5u, "主题与长度正确");
    Check(memcmp(pkt.payload, payload, 5u) == 0, "payload（含 0x00/0xFF 边界字节）一致");

    Check(Mqtt_EncodePublish("dev/1/up", payload, 5u, 1u, 0u, 0x1234u, out, sizeof out, &out_len) == MQTT_OK,
          "QoS1 PUBLISH 编码成功");
    Check(Mqtt_Decode(out, out_len, &pkt, &consumed) == MQTT_OK, "QoS1 PUBLISH 解码成功");
    Check(pkt.qos == 1u && pkt.packet_id == 0x1234u, "QoS1 携带包 ID 0x1234");
    Check(pkt.payload_len == 5u && memcmp(pkt.payload, payload, 5u) == 0, "QoS1 payload 一致");

    Check(Mqtt_EncodePublish("t", payload, 5u, 1u, 0u, 0u, out, sizeof out, &out_len) == MQTT_ERR_PARAM,
          "QoS1 但包 ID 为 0 → 参数错误");
    Check(Mqtt_EncodePublish("t", payload, 5u, 2u, 0u, 1u, out, sizeof out, &out_len) == MQTT_ERR_RANGE,
          "QoS2 未实现 → 拒绝");
    Check(Mqtt_EncodePublish("", payload, 5u, 0u, 0u, 0u, out, sizeof out, &out_len) == MQTT_ERR_RANGE,
          "空主题 → 拒绝");

    Group("MQTT 报文完整性校验");
    Check(Mqtt_EncodePublish("dev/1/up", payload, 5u, 0u, 0u, 0u, out, sizeof out, &out_len) == MQTT_OK,
          "重新编码一条 PUBLISH");
    Check(Mqtt_Decode(out, out_len - 1u, &pkt, &consumed) == MQTT_ERR_MALFORMED,
          "长度少一字节 → 判定报文残缺（丢弃整包）");
    Check(Mqtt_Decode((const uint8_t *)"\x30\x05\x00", 3u, &pkt, &consumed) == MQTT_ERR_MALFORMED,
          "剩余长度与实到字节数不符 → 拒绝");

    Group("MQTT 定长报文编码（CONNACK / PUBACK / PINGREQ）");
    Check(Mqtt_EncodeConnAck(0u, 0u, out, sizeof out, &out_len) == MQTT_OK &&
          out_len == 4u && out[0] == 0x20u && out[1] == 0x02u && out[2] == 0x00u && out[3] == 0x00u,
          "CONNACK 编码 = 20 02 00 00");
    Check(Mqtt_EncodePubAck(0x1234u, out, sizeof out, &out_len) == MQTT_OK &&
          out_len == 4u && out[0] == 0x40u && out[2] == 0x12u && out[3] == 0x34u,
          "PUBACK 编码 = 40 02 12 34");
    Check(Mqtt_EncodePingReq(out, sizeof out, &out_len) == MQTT_OK && out_len == 2u && out[0] == 0xC0u,
          "PINGREQ 编码 = C0 00");
}

static void TestQos1Window(void)
{
    MqttQos1Window w;
    uint16_t id;

    Group("QoS1 发送窗口（包 ID 分配 / 在途管理 / 背压）");
    Mqtt_Qos1Init(&w);
    id = Mqtt_Qos1NextId(&w);
    Check(id == 1u, "首个包 ID 从 1 开始");
    for (unsigned i = 0u; i < MQTT_QOS1_WINDOW; i++) {
        uint16_t nid = Mqtt_Qos1NextId(&w);
        Check(Mqtt_Qos1MarkSent(&w, nid) == MQTT_OK, "登记在途报文");
    }
    Check(Mqtt_Qos1InflightCount(&w) == MQTT_QOS1_WINDOW, "窗口已满");
    Check(Mqtt_Qos1MarkSent(&w, Mqtt_Qos1NextId(&w)) == MQTT_ERR_BUSY, "窗口满 → 背压（拒绝新报文）");
    Check(Mqtt_Qos1OnPubAck(&w, 3u) == MQTT_OK, "收到 PUBACK 释放包 ID 3");
    Check(Mqtt_Qos1InflightCount(&w) == MQTT_QOS1_WINDOW - 1u, "在途数量减一");
    Check(Mqtt_Qos1OnPubAck(&w, 3u) == MQTT_ERR_PARAM, "重复 PUBACK → 参数错误");
    Check(Mqtt_Qos1OnPubAck(&w, 999u) == MQTT_ERR_PARAM, "未知 PUBACK → 参数错误");
    w.next_id = 65535u;
    Check(Mqtt_Qos1NextId(&w) == 65535u && w.next_id == 1u, "包 ID 到 65535 后回绕到 1（跳过 0）");
}

static void MakeHeader(uint8_t *h, uint32_t version, uint32_t size, uint32_t crc)
{
    h[0] = (uint8_t)(OTA_MAGIC >> 24); h[1] = (uint8_t)(OTA_MAGIC >> 16);
    h[2] = (uint8_t)(OTA_MAGIC >> 8);  h[3] = (uint8_t)OTA_MAGIC;
    h[4] = (uint8_t)(version >> 24); h[5] = (uint8_t)(version >> 16);
    h[6] = (uint8_t)(version >> 8);  h[7] = (uint8_t)version;
    h[8] = (uint8_t)(size >> 24);    h[9] = (uint8_t)(size >> 16);
    h[10] = (uint8_t)(size >> 8);    h[11] = (uint8_t)size;
    h[12] = (uint8_t)(crc >> 24);    h[13] = (uint8_t)(crc >> 16);
    h[14] = (uint8_t)(crc >> 8);     h[15] = (uint8_t)crc;
}

static void TestOta(void)
{
    OtaContext ctx;
    uint8_t header[OTA_HEADER_LEN];
    uint8_t image[300];
    uint32_t crc;

    Group("OTA 正常升级（头校验 → 顺序写入 → CRC32 → 确认生效）");
    for (unsigned i = 0u; i < sizeof image; i++) image[i] = (uint8_t)(i * 7u + 3u);
    crc = Ota_Crc32(image, sizeof image);
    MakeHeader(header, 0x00020001u, sizeof image, crc);

    Ota_Init(&ctx, 0x00010001u);
    Check(Ota_ActiveVersion(&ctx) == 0x00010001u, "初始运行版本正确");
    Check(Ota_BeginImage(&ctx, header, OTA_HEADER_LEN) == OTA_OK, "镜像头校验通过，开始接收");
    Check(Ota_State(&ctx) == OTA_RECEIVING, "进入接收态");
    Check(Ota_WriteChunk(&ctx, 0u, image, 100u) == OTA_OK, "写入第 1 片（100 字节）");
    Check(Ota_WriteChunk(&ctx, 100u, &image[100], 200u) == OTA_OK, "写入第 2 片（200 字节）");
    Check(Ota_Received(&ctx) == sizeof image, "累计接收长度正确");
    Check(Ota_Finish(&ctx) == OTA_OK, "全镜像 CRC32 校验通过");
    Check(Ota_State(&ctx) == OTA_VERIFIED, "进入待确认态（boot_pending）");
    Check(Ota_ActiveVersion(&ctx) == 0x00010001u, "未确认前仍运行旧版本");
    Check(Ota_Confirm(&ctx) == OTA_OK, "自检通过 → 确认升级");
    Check(Ota_ActiveVersion(&ctx) == 0x00020001u, "确认后运行新版本");
    Check(memcmp(Ota_ActiveSlot(&ctx), image, sizeof image) == 0, "新槽内容与下载镜像逐字节一致");

    Group("OTA 异常路径（坏 CRC / 乱序 / 越界 / 坏头）");
    MakeHeader(header, 0x00030001u, sizeof image, crc ^ 0xFFFFu);   /* 故意写错 CRC */
    Ota_Init(&ctx, 0x00010001u);
    Check(Ota_BeginImage(&ctx, header, OTA_HEADER_LEN) == OTA_OK, "坏 CRC 镜像的头仍然合法（CRC 在结尾判）");
    Check(Ota_WriteChunk(&ctx, 0u, image, 100u) == OTA_OK, "写入第 1 片");
    Check(Ota_WriteChunk(&ctx, 100u, &image[100], 200u) == OTA_OK, "写入第 2 片");
    Check(Ota_Finish(&ctx) == OTA_ERR_CRC, "结尾 CRC 不匹配 → 拒绝生效");
    Check(Ota_State(&ctx) == OTA_FAILED, "状态置为 FAILED");

    MakeHeader(header, 0x00040001u, sizeof image, crc);
    Ota_Init(&ctx, 0x00010001u);
    (void)Ota_BeginImage(&ctx, header, OTA_HEADER_LEN);
    Check(Ota_WriteChunk(&ctx, 0u, image, 100u) == OTA_OK, "写入第 1 片");
    Check(Ota_WriteChunk(&ctx, 200u, &image[200], 100u) == OTA_ERR_OFFSET, "跳过第 2 片（乱序）→ 拒绝");
    Check(Ota_WriteChunk(&ctx, 0u, image, 100u) == OTA_ERR_OFFSET, "重复写同一片 → 拒绝");
    Check(Ota_WriteChunk(&ctx, 100u, image, 300u) == OTA_ERR_SIZE, "越过镜像长度 → 拒绝");

    {
        uint8_t bad[OTA_HEADER_LEN];
        MakeHeader(bad, 1u, sizeof image, crc);
        bad[0] = 0x00u;                                          /* 破坏 magic */
        Ota_Init(&ctx, 0x00010001u);
        Check(Ota_BeginImage(&ctx, bad, OTA_HEADER_LEN) == OTA_ERR_HEADER, "magic 不匹配 → 拒绝");
        MakeHeader(bad, 1u, OTA_SLOT_SIZE + 1u, crc);
        Check(Ota_BeginImage(&ctx, bad, OTA_HEADER_LEN) == OTA_ERR_SIZE, "镜像大于分区 → 拒绝");
    }

    Group("OTA 断电续传与回滚");
    MakeHeader(header, 0x00050001u, sizeof image, crc);
    Ota_Init(&ctx, 0x00010001u);
    (void)Ota_BeginImage(&ctx, header, OTA_HEADER_LEN);
    Check(Ota_WriteChunk(&ctx, 0u, image, 100u) == OTA_OK, "写入 100 字节后“断电”");
    Check(Ota_Received(&ctx) == 100u, "接收进度保留（可从 100 偏移续传）");
    Check(Ota_WriteChunk(&ctx, 100u, &image[100], 200u) == OTA_OK, "续传剩余 200 字节");
    Check(Ota_Finish(&ctx) == OTA_OK, "续传后 CRC 校验通过");
    Check(Ota_Rollback(&ctx) == OTA_OK, "模拟新镜像自检失败 → 回滚");
    Check(Ota_State(&ctx) == OTA_ROLLED_BACK, "状态为 ROLLED_BACK");
    Check(Ota_ActiveVersion(&ctx) == 0x00010001u, "回滚后仍运行旧版本（业务不受影响）");
}

static void TestPower(void)
{
    PwrPolicy p;

    Group("深睡策略（Activity / Light-sleep / Deep-sleep 分级）");
    Pwr_Init(&p, 1000u, 10000u);
    Check(Pwr_Evaluate(&p, 500u) == PWR_ACTIVE, "500 ms 无活动 → 保持 ACTIVE");
    Check(Pwr_Evaluate(&p, 600u) == PWR_LIGHT_SLEEP, "累计 1100 ms → 进入 Light-sleep");
    Check(Pwr_Evaluate(&p, 9000u) == PWR_DEEP_SLEEP, "累计超 10 s → 进入 Deep-sleep");
    Check(p.sleep_count == 1u, "深睡次数累加");
    Pwr_OnActivity(&p);
    Check(p.idle_ms == 0u, "有活动 → 空闲计时清零");
    Pwr_SetPendingTx(&p, 1u);
    Check(Pwr_Evaluate(&p, 60000u) == PWR_ACTIVE, "有数据待上报 → 禁止休眠");
    Check(Pwr_ExpectedWakeMs(&p) == 0u, "待发数据时立即唤醒");
    Pwr_SetPendingTx(&p, 0u);
    Pwr_SetNetConnected(&p, 0u);
    Check(Pwr_Evaluate(&p, 60000u) == PWR_ACTIVE, "断网 → 保持唤醒以便重连");
    Check(Pwr_ExpectedWakeMs(&p) == 200u, "断网重连周期 200 ms");
    Pwr_SetNetConnected(&p, 1u);
    Check(Pwr_ExpectedWakeMs(&p) == 5000u, "正常心跳上报周期 5 s");
}

int main(void)
{
    printf("=== 物联网协议与 OTA 逻辑层 · 自检测试（MQTT / OTA / 深睡策略） ===\n");
    TestRemainingLength();
    TestPublish();
    TestQos1Window();
    TestOta();
    TestPower();
    printf("\n------------------------------------------------------------\n");
    printf("用例总数 %d，通过 %d，失败 %d\n", g_run, g_run - g_fail, g_fail);
    printf("%s\n", g_fail == 0 ? "RESULT: ALL PASS" : "RESULT: FAILED");
    return g_fail == 0 ? 0 : 1;
}
