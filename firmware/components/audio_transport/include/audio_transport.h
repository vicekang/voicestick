#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIO_TRANSPORT_TYPE_AUDIO 0x01
#define AUDIO_TRANSPORT_V1 1
#define AUDIO_TRANSPORT_V2 2
#define AUDIO_TRANSPORT_HEADER_SIZE 16
#define AUDIO_TRANSPORT_ATT_OVERHEAD 3

typedef struct {
    uint32_t seq;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t len;
} audio_transport_packet_t;

typedef enum {
    AUDIO_TRANSPORT_OK = 0,
    AUDIO_TRANSPORT_INVALID_ARGUMENT = -1,
    AUDIO_TRANSPORT_BUFFER_TOO_SMALL = -2,
    AUDIO_TRANSPORT_PACKET_TOO_LARGE = -3,
    AUDIO_TRANSPORT_NONCONTIGUOUS_SEQUENCE = -4,
} audio_transport_result_t;

size_t audio_transport_att_payload_capacity(uint16_t att_mtu);
size_t audio_transport_v2_packet_budget(uint16_t att_mtu, uint8_t packet_count);

audio_transport_result_t audio_transport_encode(
    uint8_t version,
    uint32_t session_id,
    uint32_t first_seq,
    uint8_t envelope_flags,
    const audio_transport_packet_t *packets,
    uint8_t packet_count,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_len);
