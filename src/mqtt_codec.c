#include "mqtt_codec.h"
#include <string.h>

int Mqtt_EncodeRemainingLength(uint32_t len, uint8_t *out, size_t out_size)
{
    int n = 0;
    if (out == NULL || out_size == 0u || len > 268435455u) return -1;
    do {
        uint8_t byte = (uint8_t)(len % 128u);
        len /= 128u;
        if (len > 0u) byte |= 0x80u;
        if ((size_t)n >= out_size) return -1;
        out[n++] = byte;
    } while (len > 0u);
    return n;
}

int Mqtt_DecodeRemainingLength(const uint8_t *in, size_t in_size, uint32_t *len, size_t *used)
{
    uint32_t value = 0u, multiplier = 1u;
    size_t i = 0u;
    if (in == NULL || len == NULL || used == NULL) return -1;
    for (i = 0u; i < in_size && i < 4u; i++) {
        value += (uint32_t)(in[i] & 0x7Fu) * multiplier;
        if ((in[i] & 0x80u) == 0u) {
            *len = value;
            *used = i + 1u;
            return 0;
        }
        multiplier *= 128u;
    }
    return -1;                                  /* 超过 4 字节或数据不完整 */
}

MqttStatus Mqtt_EncodePublish(const char *topic, const uint8_t *payload, uint16_t payload_len,
                              uint8_t qos, uint8_t retain, uint16_t packet_id,
                              uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t topic_len, remaining, offset, n;
    uint8_t header;

    if (topic == NULL || out == NULL || out_len == NULL) return MQTT_ERR_PARAM;
    if (qos > 1u) return MQTT_ERR_RANGE;
    if (qos == 1u && packet_id == 0u) return MQTT_ERR_PARAM;
    topic_len = strlen(topic);
    if (topic_len == 0u || topic_len > MQTT_MAX_TOPIC) return MQTT_ERR_RANGE;
    if (payload_len > MQTT_MAX_PAYLOAD) return MQTT_ERR_RANGE;

    remaining = 2u + topic_len + (qos ? 2u : 0u) + payload_len;
    offset = 0u;
    header = (uint8_t)((MQTT_TYPE_PUBLISH << 4) | ((qos & 0x03u) << 1) | (retain ? 1u : 0u));
    if (offset + 1u >= out_size) return MQTT_ERR_RANGE;
    out[offset++] = header;
    n = (size_t)Mqtt_EncodeRemainingLength((uint32_t)remaining, &out[offset], out_size - offset);
    if (n == 0u || n == (size_t)-1) return MQTT_ERR_RANGE;
    offset += n;
    if (offset + remaining > out_size) return MQTT_ERR_RANGE;

    out[offset++] = (uint8_t)(topic_len >> 8);
    out[offset++] = (uint8_t)(topic_len & 0xFFu);
    memcpy(&out[offset], topic, topic_len);
    offset += topic_len;
    if (qos == 1u) {
        out[offset++] = (uint8_t)(packet_id >> 8);
        out[offset++] = (uint8_t)(packet_id & 0xFFu);
    }
    if (payload_len && payload != NULL) {
        memcpy(&out[offset], payload, payload_len);
        offset += payload_len;
    }
    *out_len = offset;
    return MQTT_OK;
}

static MqttStatus EncodeShort(uint8_t type, uint16_t value, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 4u) return MQTT_ERR_RANGE;
    out[0] = (uint8_t)(type << 4);
    out[1] = 2u;
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)(value & 0xFFu);
    *out_len = 4u;
    return MQTT_OK;
}

MqttStatus Mqtt_EncodeConnAck(uint8_t session_present, uint8_t return_code,
                              uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 4u) return MQTT_ERR_RANGE;
    out[0] = (uint8_t)(MQTT_TYPE_CONNACK << 4);
    out[1] = 2u;
    out[2] = (uint8_t)(session_present ? 1u : 0u);
    out[3] = return_code;
    *out_len = 4u;
    return MQTT_OK;
}

MqttStatus Mqtt_EncodePubAck(uint16_t packet_id, uint8_t *out, size_t out_size, size_t *out_len)
{
    return EncodeShort(MQTT_TYPE_PUBACK, packet_id, out, out_size, out_len);
}

MqttStatus Mqtt_EncodePingReq(uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 2u) return MQTT_ERR_RANGE;
    out[0] = (uint8_t)(MQTT_TYPE_PINGREQ << 4);
    out[1] = 0u;
    *out_len = 2u;
    return MQTT_OK;
}

MqttStatus Mqtt_Decode(const uint8_t *in, size_t in_len, MqttPacket *pkt, size_t *consumed)
{
    uint32_t remaining = 0u;
    size_t used = 0u, offset;
    int rc;

    if (in == NULL || pkt == NULL || consumed == NULL || in_len < 2u) return MQTT_ERR_PARAM;
    memset(pkt, 0, sizeof *pkt);
    pkt->type = (uint8_t)(in[0] >> 4);
    rc = Mqtt_DecodeRemainingLength(&in[1], in_len - 1u, &remaining, &used);
    if (rc != 0) return MQTT_ERR_MALFORMED;
    offset = 1u + used;
    if (offset + remaining != in_len) return MQTT_ERR_MALFORMED;   /* 长度不一致 → 丢弃整包 */

    switch (pkt->type) {
    case MQTT_TYPE_PUBLISH: {
        uint16_t topic_len;
        uint32_t remain = remaining;
        if (remain < 2u) return MQTT_ERR_MALFORMED;
        topic_len = (uint16_t)((in[offset] << 8) | in[offset + 1u]);
        offset += 2u;
        remain -= 2u;
        if (topic_len == 0u || topic_len > MQTT_MAX_TOPIC || topic_len > remain) return MQTT_ERR_MALFORMED;
        memcpy(pkt->topic, &in[offset], topic_len);
        pkt->topic[topic_len] = '\0';
        pkt->topic_len = topic_len;
        offset += topic_len;
        remain -= topic_len;
        pkt->qos = (uint8_t)((in[0] >> 1) & 0x03u);
        pkt->retain = (uint8_t)(in[0] & 0x01u);
        pkt->dup = (uint8_t)((in[0] >> 3) & 0x01u);
        if (pkt->qos > 1u) return MQTT_ERR_MALFORMED;
        if (pkt->qos == 1u) {
            if (remain < 2u) return MQTT_ERR_MALFORMED;
            pkt->packet_id = (uint16_t)((in[offset] << 8) | in[offset + 1u]);
            offset += 2u;
            remain -= 2u;
            if (pkt->packet_id == 0u) return MQTT_ERR_MALFORMED;
        }
        if (remain > MQTT_MAX_PAYLOAD) return MQTT_ERR_MALFORMED;
        memcpy(pkt->payload, &in[offset], remain);
        pkt->payload_len = (uint16_t)remain;
        offset += remain;
        break;
    }
    case MQTT_TYPE_PUBACK:
    case MQTT_TYPE_SUBACK:
    case MQTT_TYPE_CONNACK:
        if (remaining != 2u) return MQTT_ERR_MALFORMED;
        pkt->packet_id = (uint16_t)((in[offset] << 8) | in[offset + 1u]);
        offset += 2u;
        break;
    case MQTT_TYPE_PINGREQ:
    case MQTT_TYPE_PINGRESP:
    case MQTT_TYPE_DISCONNECT:
        if (remaining != 0u) return MQTT_ERR_MALFORMED;
        break;
    default:
        break;                                   /* 其它类型：按长度跳过（保留给上层） */
    }
    *consumed = offset;
    return MQTT_OK;
}

void Mqtt_Qos1Init(MqttQos1Window *w)
{
    if (w == NULL) return;
    w->next_id = 1u;
    w->count = 0u;
    memset(w->inflight, 0, sizeof w->inflight);
}

uint16_t Mqtt_Qos1NextId(MqttQos1Window *w)
{
    uint16_t id;
    if (w == NULL) return 0u;
    id = w->next_id;
    w->next_id = (uint16_t)(w->next_id == 65535u ? 1u : w->next_id + 1u);   /* 跳过 0 */
    return id;
}

MqttStatus Mqtt_Qos1MarkSent(MqttQos1Window *w, uint16_t packet_id)
{
    if (w == NULL || packet_id == 0u) return MQTT_ERR_PARAM;
    if (w->count >= MQTT_QOS1_WINDOW) return MQTT_ERR_BUSY;                /* 窗口满 → 上层背压 */
    for (uint8_t i = 0u; i < w->count; i++) {
        if (w->inflight[i] == packet_id) return MQTT_ERR_PARAM;            /* 重复登记 */
    }
    w->inflight[w->count++] = packet_id;
    return MQTT_OK;
}

MqttStatus Mqtt_Qos1OnPubAck(MqttQos1Window *w, uint16_t packet_id)
{
    if (w == NULL || packet_id == 0u) return MQTT_ERR_PARAM;
    for (uint8_t i = 0u; i < w->count; i++) {
        if (w->inflight[i] == packet_id) {
            for (uint8_t j = (uint8_t)(i + 1u); j < w->count; j++) w->inflight[j - 1u] = w->inflight[j];
            w->count--;
            return MQTT_OK;
        }
    }
    return MQTT_ERR_PARAM;                                                 /* 未知包 ID → 忽略/告警 */
}

uint8_t Mqtt_Qos1InflightCount(const MqttQos1Window *w) { return (w == NULL) ? 0u : w->count; }
