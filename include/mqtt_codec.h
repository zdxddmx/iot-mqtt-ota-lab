/* MQTT 3.1.1 报文编解码（主机端可测）：定长头 + 变长"剩余长度" + 主题/payload/QoS 包 ID */
#ifndef MQTT_CODEC_H
#define MQTT_CODEC_H
#include <stdint.h>
#include <stddef.h>

#define MQTT_MAX_TOPIC   128u
#define MQTT_MAX_PAYLOAD 256u
#define MQTT_MAX_PACKET  (MQTT_MAX_PAYLOAD + MQTT_MAX_TOPIC + 16u)
#define MQTT_QOS1_WINDOW 8u

typedef enum {
    MQTT_OK = 0,
    MQTT_ERR_PARAM,
    MQTT_ERR_RANGE,
    MQTT_ERR_MALFORMED,
    MQTT_ERR_BUSY
} MqttStatus;

typedef enum {
    MQTT_TYPE_CONNECT    = 1,
    MQTT_TYPE_CONNACK    = 2,
    MQTT_TYPE_PUBLISH    = 3,
    MQTT_TYPE_PUBACK     = 4,
    MQTT_TYPE_SUBSCRIBE  = 8,
    MQTT_TYPE_SUBACK     = 9,
    MQTT_TYPE_PINGREQ    = 12,
    MQTT_TYPE_PINGRESP   = 13,
    MQTT_TYPE_DISCONNECT = 14
} MqttType;

typedef struct {
    uint8_t  type;
    uint8_t  qos;
    uint8_t  retain;
    uint8_t  dup;
    uint16_t packet_id;
    char     topic[MQTT_MAX_TOPIC];
    uint16_t topic_len;
    uint8_t  payload[MQTT_MAX_PAYLOAD];
    uint16_t payload_len;
} MqttPacket;

/* 剩余长度：变长整数（7 位一组，最多 4 字节） */
int  Mqtt_EncodeRemainingLength(uint32_t len, uint8_t *out, size_t out_size);
int  Mqtt_DecodeRemainingLength(const uint8_t *in, size_t in_size, uint32_t *len, size_t *used);

MqttStatus Mqtt_EncodePublish(const char *topic, const uint8_t *payload, uint16_t payload_len,
                              uint8_t qos, uint8_t retain, uint16_t packet_id,
                              uint8_t *out, size_t out_size, size_t *out_len);
MqttStatus Mqtt_EncodeConnAck(uint8_t session_present, uint8_t return_code,
                              uint8_t *out, size_t out_size, size_t *out_len);
MqttStatus Mqtt_EncodePubAck(uint16_t packet_id, uint8_t *out, size_t out_size, size_t *out_len);
MqttStatus Mqtt_EncodePingReq(uint8_t *out, size_t out_size, size_t *out_len);
MqttStatus Mqtt_Decode(const uint8_t *in, size_t in_len, MqttPacket *pkt, size_t *consumed);

/* QoS1 发送窗口：包 ID 分配与在途报文管理（收到 PUBACK 才释放） */
typedef struct {
    uint16_t next_id;
    uint16_t inflight[MQTT_QOS1_WINDOW];
    uint8_t  count;
} MqttQos1Window;

void       Mqtt_Qos1Init(MqttQos1Window *w);
uint16_t   Mqtt_Qos1NextId(MqttQos1Window *w);
MqttStatus Mqtt_Qos1MarkSent(MqttQos1Window *w, uint16_t packet_id);
MqttStatus Mqtt_Qos1OnPubAck(MqttQos1Window *w, uint16_t packet_id);
uint8_t    Mqtt_Qos1InflightCount(const MqttQos1Window *w);
#endif
