/* SPDX-License-Identifier: MIT
 * Project-owned mirrors of the JA-A12 T23/Ace ABI, recovered from accesses and
 * copy lengths in jooanipc. These are not general Ingenic SDK declarations.
 * In particular, neither SDK version nor symbol names establish compatibility.
 */
#ifndef JIPC_IMP_ABI_H
#define JIPC_IMP_ABI_H
#include "jipc.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[32];
    uint16_t sensor_id, reserved_22;
    uint32_t bus_type;
    char i2c_name[20];
    uint32_t i2c_address, i2c_adapter;
    unsigned char reserved_44[16];
} jipc_imp_sensor;
typedef struct {
    uint32_t width, height, pixel_format;
    uint32_t crop_enable, crop_left, crop_top, crop_width, crop_height;
    uint32_t scaler_enable, scaler_width, scaler_height;
    uint32_t fps_num, fps_den, buffer_count, channel_type, reserved_3c;
    uint32_t frame_crop_enable, frame_crop_left, frame_crop_top;
    uint32_t frame_crop_width, frame_crop_height;
} jipc_imp_fs;
typedef struct { uint32_t words[41]; } jipc_imp_encoder_attr;
typedef struct { int32_t device, group, output; } jipc_imp_cell;
typedef struct {
    uint32_t type, address, length;
    unsigned char reserved[20];
} jipc_imp_pack;
/* Addresses stay u32 even in host builds: host execution of this ABI is denied. */
typedef struct { uint32_t packs, pack_count, sequence, optional; } jipc_imp_stream;
typedef struct {
    int32_t sample_rate, bit_width, sound_mode, frame_count, samples, channels;
} jipc_imp_audio_attr;
typedef struct { int32_t depth, track; } jipc_imp_audio_channel;
typedef struct {
    uint32_t bit_width, sound_mode, address, physical;
    uint64_t timestamp;
    uint32_t sequence, bytes;
} jipc_imp_audio_frame;
typedef struct { uint32_t mode, i_bits, p_bits, reserved, priority; } jipc_imp_super;

_Static_assert(sizeof(jipc_imp_sensor) == 84, "sensor ABI");
_Static_assert(offsetof(jipc_imp_sensor, sensor_id) == 32, "sensor identity");
_Static_assert(offsetof(jipc_imp_sensor, i2c_address) == 60, "sensor address");
_Static_assert(sizeof(jipc_imp_fs) == 84, "FS ABI is 84, not 60 bytes");
_Static_assert(offsetof(jipc_imp_fs, fps_num) == 44, "FS numerator");
_Static_assert(offsetof(jipc_imp_fs, frame_crop_enable) == 64, "extended FS crop");
_Static_assert(sizeof(jipc_imp_encoder_attr) == 164, "encoder ABI");
_Static_assert(sizeof(jipc_imp_pack) == 32, "encoder pack stride");
_Static_assert(sizeof(jipc_imp_stream) == 16, "encoder stream ABI");
_Static_assert(sizeof(jipc_imp_audio_attr) == 24, "audio attributes ABI");
_Static_assert(sizeof(jipc_imp_audio_frame) == 32, "audio frame ABI");
_Static_assert(offsetof(jipc_imp_audio_frame, timestamp) == 16, "audio timestamp");
_Static_assert(offsetof(jipc_imp_audio_frame, bytes) == 28, "audio frame length");

int jipc_imp_open(jipc_app *app, void **state, char *error, size_t capacity);
int jipc_imp_wait(void *state, unsigned timeout_ms);
int jipc_imp_poll(jipc_app *app, void *state, unsigned timeout_ms,
                  char *error, size_t capacity);
void jipc_imp_close(void *state);
int jipc_imp_control(void *state, const char *operation, int value, int value2,
                     char *error, size_t capacity);
int jipc_imp_snapshot(void *state, unsigned channel, jipc_frame *frame,
                      char *error, size_t capacity);
int jipc_imp_talk(void *state, const void *data, size_t bytes,
                  char *error, size_t capacity);
#endif
