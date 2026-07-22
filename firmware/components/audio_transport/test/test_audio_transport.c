#include "audio_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_capacity(void)
{
    assert(audio_transport_att_payload_capacity(247) == 244);
    assert(audio_transport_v2_packet_budget(247, 4) == 56);
    assert(audio_transport_v2_packet_budget(185, 3) == 54);
    assert(audio_transport_v2_packet_budget(3, 1) == 0);
}

static void test_v1(void)
{
    const uint8_t payload[] = {0xaa, 0xbb};
    const audio_transport_packet_t packet = {
        .seq = 7,
        .flags = 1,
        .payload = payload,
        .len = sizeof(payload),
    };
    uint8_t output[32] = {0};
    size_t output_len = 0;
    assert(audio_transport_encode(1, 9, 7, 0, &packet, 1,
                                  output, sizeof(output), &output_len) == 0);
    assert(output_len == 18);
    assert(output[0] == 1 && output[1] == 1 && output[2] == 16);
    assert(output[8] == 7 && output[12] == 1 && output[14] == 2);
    assert(output[16] == 0xaa && output[17] == 0xbb);
}

static void test_v2_bundle(void)
{
    const uint8_t first[] = {0xaa, 0xbb};
    const uint8_t second[] = {0xcc};
    const audio_transport_packet_t packets[] = {
        {.seq = 40, .flags = 1, .payload = first, .len = sizeof(first)},
        {.seq = 41, .flags = 0, .payload = second, .len = sizeof(second)},
    };
    uint8_t output[32] = {0};
    size_t output_len = 0;
    assert(audio_transport_encode(2, 9, 40, 0, packets, 2,
                                  output, sizeof(output), &output_len) == 0);
    assert(output_len == 21);
    assert(output[0] == 2 && output[13] == 2 && output[14] == 5);
    const uint8_t expected_payload[] = {2, 0xaa, 0xbb, 1, 0xcc};
    assert(memcmp(&output[16], expected_payload, sizeof(expected_payload)) == 0);
}

static void test_rejects_invalid_input(void)
{
    const uint8_t payload[] = {0xaa};
    const audio_transport_packet_t packets[] = {
        {.seq = 4, .payload = payload, .len = 1},
        {.seq = 6, .payload = payload, .len = 1},
    };
    uint8_t output[32] = {0};
    size_t output_len = 0;
    assert(audio_transport_encode(2, 1, 4, 0, packets, 2,
                                  output, sizeof(output), &output_len) ==
           AUDIO_TRANSPORT_NONCONTIGUOUS_SEQUENCE);
    assert(audio_transport_encode(1, 1, 4, 0, packets, 2,
                                  output, sizeof(output), &output_len) ==
           AUDIO_TRANSPORT_INVALID_ARGUMENT);
}

int main(void)
{
    test_capacity();
    test_v1();
    test_v2_bundle();
    test_rejects_invalid_input();
    puts("audio_transport tests passed");
    return 0;
}
