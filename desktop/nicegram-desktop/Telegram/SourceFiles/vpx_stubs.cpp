// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//

#include "stdafx.h"

#ifdef Q_OS_WIN

#include <cstdint>
#include <cstddef>

extern "C" {

struct vpx_codec_iface {
    const char *name;
    int abi_version;
    int caps;
    void *init;
    void *destroy;
    void *ctrl_maps;
    void *dec[5];
    void *enc[8];
};
typedef struct vpx_codec_iface vpx_codec_iface_t;

vpx_codec_iface_t vpx_codec_vp9_cx_algo = {
    "WebM Project VP9 Encoder (stub)",
    5,
    0,
    nullptr, nullptr, nullptr,
    { nullptr, nullptr, nullptr, nullptr, nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }
};

vpx_codec_iface_t vpx_codec_vp9_dx_algo = {
    "WebM Project VP9 Decoder (stub)",
    5,
    0,
    nullptr, nullptr, nullptr,
    { nullptr, nullptr, nullptr, nullptr, nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }
};

vpx_codec_iface_t *vpx_codec_vp9_cx(void) {
    return &vpx_codec_vp9_cx_algo;
}

vpx_codec_iface_t *vpx_codec_vp9_dx(void) {
    return &vpx_codec_vp9_dx_algo;
}

void vpx_sad64x64x4d_avx512(const uint8_t *src_ptr, int src_stride,
                            const uint8_t *const ref_array[4], int ref_stride,
                            uint32_t sad_array[4]) {
    for (int r = 0; r < 4; ++r) {
        const uint8_t *src = src_ptr;
        const uint8_t *ref = ref_array[r];
        uint32_t sum = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                int diff = static_cast<int>(src[x]) - static_cast<int>(ref[x]);
                sum += static_cast<uint32_t>((diff < 0) ? -diff : diff);
            }
            src += src_stride;
            ref += ref_stride;
        }
        sad_array[r] = sum;
    }
}

int vp8dx_receive_compressed_data(void *pbi, int64_t time_stamp) {
    (void)pbi;
    (void)time_stamp;
    return 0;
}

int vp8dx_get_raw_frame(void *pbi, void *sd, int64_t *time_stamp, int64_t *time_end_stamp, void *flags) {
    (void)pbi;
    (void)sd;
    (void)time_stamp;
    (void)time_end_stamp;
    (void)flags;
    return -1;
}

int vp8dx_references_buffer(void *oci, int ref_frame) {
    (void)oci;
    (void)ref_frame;
    return 0;
}

int vp8dx_get_reference(void *pbi, int ref_frame_flag, void *sd) {
    (void)pbi;
    (void)ref_frame_flag;
    (void)sd;
    return -1;
}

int vp8dx_set_reference(void *pbi, int ref_frame_flag, void *sd) {
    (void)pbi;
    (void)ref_frame_flag;
    (void)sd;
    return -1;
}

int vp8dx_get_quantizer(const void *pbi) {
    (void)pbi;
    return 0;
}

int vp8_create_decoder_instances(void *fb, void *oxcf) {
    (void)fb;
    (void)oxcf;
    return 0;
}

int vp8_remove_decoder_instances(void *fb) {
    (void)fb;
    return 0;
}

void vp8_decoder_remove_threads(void *pbi) {
    (void)pbi;
}

void vp8_decoder_create_threads(void *pbi) {
    (void)pbi;
}

void vp8mt_alloc_temp_buffers(void *pbi, int width, int prev_mb_rows) {
    (void)pbi;
    (void)width;
    (void)prev_mb_rows;
}

void vp8mt_de_alloc_temp_buffers(void *pbi, int mb_rows) {
    (void)pbi;
    (void)mb_rows;
}

} // extern "C"

#endif // Q_OS_WIN
