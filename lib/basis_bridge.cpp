#include "basis_bridge.h"
#include <encoder/basisu_comp.h>
#include <cstdio>
#include <exception>
#include <limits>

bool bspp_encode_pvrtc2(const uint32_t *argb, uint32_t width, uint32_t height,
                       uint8_t *blocks, size_t size, char *error, size_t error_size) {
    try {
        static const bool initialized = basisu::basisu_encoder_init();
        if (!initialized) {
            std::snprintf(error, error_size, "Basis Universal initialization failed");
            return false;
        }
        constexpr auto format = basist::transcoder_texture_format::cTFPVRTC2_4_RGBA;
        const uint64_t block_count =
            ((static_cast<uint64_t>(width) + 3) / 4) *
            ((static_cast<uint64_t>(height) + 3) / 4);
        if (!width || !height || block_count > UINT32_MAX || size != block_count * 8) {
            std::snprintf(error, error_size, "invalid PVRTC2 dimensions or output size");
            return false;
        }

        basisu::job_pool pool(1);
        basisu::basis_compressor_params params;
        params.m_pJob_pool = &pool;
        params.m_multithreading = false;
        params.m_etc1s_quality_level = 255;
        params.m_perceptual = false;
        params.m_mip_gen = false;
        params.m_force_alpha = true;
        params.m_status_output = false;
        params.m_print_stats = false;
        params.m_source_images.resize(1);
        basisu::image &image = params.m_source_images[0];
        image.resize(width, height);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            uint32_t c = argb[i];
            image.get_ptr()[i].set((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24);
        }

        basisu::basis_compressor compressor;
        if (!compressor.init(params)) {
            std::snprintf(error, error_size, "Basis compressor initialization failed");
            return false;
        }
        auto result = compressor.process();
        if (result != basisu::basis_compressor::cECSuccess) {
            std::snprintf(error, error_size, "Basis encoding failed (code %d)",
                          static_cast<int>(result));
            return false;
        }
        const auto &basis = compressor.get_output_basis_file();
        if (basis.size() > std::numeric_limits<uint32_t>::max()) {
            std::snprintf(error, error_size, "Basis intermediate exceeds 32-bit size");
            return false;
        }
        basist::basisu_transcoder transcoder;
        if (!transcoder.start_transcoding(basis.data(), basis.size()) ||
            !transcoder.transcode_image_level(basis.data(), basis.size(), 0, 0,
                                             blocks, static_cast<uint32_t>(block_count),
                                             format)) {
            std::snprintf(error, error_size, "Basis PVRTC2 transcoding failed");
            return false;
        }
        return true;
    } catch (const std::exception &ex) {
        std::snprintf(error, error_size, "Basis encoding exception: %s", ex.what());
        return false;
    }
}
