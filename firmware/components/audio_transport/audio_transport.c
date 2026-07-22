#include "audio_transport.h"

#include <string.h>

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

size_t audio_transport_att_payload_capacity(uint16_t att_mtu)
{
    return att_mtu > AUDIO_TRANSPORT_ATT_OVERHEAD
        ? (size_t)att_mtu - AUDIO_TRANSPORT_ATT_OVERHEAD
        : 0;
}

size_t audio_transport_v2_packet_budget(uint16_t att_mtu, uint8_t packet_count)
{
    if (packet_count == 0) {
        return 0;
    }

    const size_t att_payload = audio_transport_att_payload_capacity(att_mtu);
    const size_t framing = AUDIO_TRANSPORT_HEADER_SIZE + packet_count;
    if (att_payload <= framing) {
        return 0;
    }
    return (att_payload - framing) / packet_count;
}

audio_transport_result_t audio_transport_encode(
    uint8_t version,
    uint32_t session_id,
    uint32_t first_seq,
    uint8_t envelope_flags,
    const audio_transport_packet_t *packets,
    uint8_t packet_count,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len)
{
    if (!output || !output_len || output_capacity < AUDIO_TRANSPORT_HEADER_SIZE) {
        return AUDIO_TRANSPORT_INVALID_ARGUMENT;
    }
    if (packet_count > 0 && !packets) {
        return AUDIO_TRANSPORT_INVALID_ARGUMENT;
    }
    if (version == AUDIO_TRANSPORT_V1 && packet_count != 1) {
        return AUDIO_TRANSPORT_INVALID_ARGUMENT;
    }
    if (version != AUDIO_TRANSPORT_V1 && version != AUDIO_TRANSPORT_V2) {
        return AUDIO_TRANSPORT_INVALID_ARGUMENT;
    }

    size_t payload_len = version == AUDIO_TRANSPORT_V2 ? packet_count : 0;
    uint8_t flags = envelope_flags;
    for (uint8_t index = 0; index < packet_count; index++) {
        if (packets[index].seq != first_seq + index) {
            return AUDIO_TRANSPORT_NONCONTIGUOUS_SEQUENCE;
        }
        if (packets[index].len > 0 && !packets[index].payload) {
            return AUDIO_TRANSPORT_INVALID_ARGUMENT;
        }
        if (version == AUDIO_TRANSPORT_V2 && packets[index].len > UINT8_MAX) {
            return AUDIO_TRANSPORT_PACKET_TOO_LARGE;
        }
        payload_len += packets[index].len;
        flags |= packets[index].flags;
    }

    if (payload_len > UINT16_MAX ||
        AUDIO_TRANSPORT_HEADER_SIZE + payload_len > output_capacity) {
        return AUDIO_TRANSPORT_BUFFER_TOO_SMALL;
    }

    memset(output, 0, AUDIO_TRANSPORT_HEADER_SIZE);
    output[0] = version;
    output[1] = AUDIO_TRANSPORT_TYPE_AUDIO;
    write_le16(&output[2], AUDIO_TRANSPORT_HEADER_SIZE);
    write_le32(&output[4], session_id);
    write_le32(&output[8], first_seq);
    output[12] = flags;
    output[13] = version == AUDIO_TRANSPORT_V2 ? packet_count : 0;
    write_le16(&output[14], (uint16_t)payload_len);

    size_t cursor = AUDIO_TRANSPORT_HEADER_SIZE;
    for (uint8_t index = 0; index < packet_count; index++) {
        if (version == AUDIO_TRANSPORT_V2) {
            output[cursor++] = (uint8_t)packets[index].len;
        }
        if (packets[index].len > 0) {
            memcpy(&output[cursor], packets[index].payload, packets[index].len);
            cursor += packets[index].len;
        }
    }

    *output_len = cursor;
    return AUDIO_TRANSPORT_OK;
}
