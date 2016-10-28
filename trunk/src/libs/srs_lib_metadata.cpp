#include <srs_lib_metadata.hpp>
#include <srs_lib_log.hpp>
#include <srs_kernel_error.hpp>
#include <assert.h>


enum ProfileIDC
{
    NO_PROFILE     = 0,   //!< disable profile checking for experimental coding (enables FRExt, but
    // disables MV)
    FREXT_CAVLC444 = 44,  //!< YUV 4:4:4/14 "CAVLC 4:4:4"
    BASELINE       = 66,  //!< YUV 4:2:0/8  "Baseline"
    MAIN           = 77,  //!< YUV 4:2:0/8  "Main"
    EXTENDED       = 88,  //!< YUV 4:2:0/8  "Extended"
    FREXT_HP       = 100, //!< YUV 4:2:0/8  "High"
    FREXT_Hi10P    = 110, //!< YUV 4:2:0/10 "High 10"
    FREXT_Hi422    = 122, //!< YUV 4:2:2/10 "High 4:2:2"
    FREXT_Hi444    = 244, //!< YUV 4:4:4/14 "High 4:4:4"
    MULTIVIEW_HIGH = 118, //!< YUV 4:2:0/8  "Multiview High"
    STEREO_HIGH    = 128  //!< YUV 4:2:0/8  "Stereo High"
};



SpsParser::SpsParser(const uint8_t *frame, int nb_frame)
{
    m_frame = new uint8_t[nb_frame];
    memcpy(m_frame, frame + 1, nb_frame - 1);
    m_nb_frame = nb_frame - 1;
    m_parse_pos = 0;
    
    srs_error("SPS Data: %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x",
             m_frame[0], m_frame[1], m_frame[2], m_frame[3], m_frame[4],
             m_frame[5], m_frame[6], m_frame[7], m_frame[8], m_frame[9]);
}


SpsParser::~SpsParser()
{
    delete []m_frame;
}


int SpsParser::ParseSps(MetaData &metadata)
{
    int ret = ERROR_SUCCESS;
    if ((ret = EBSPtoRBSP()) != ERROR_SUCCESS) {
        srs_error("SPS Data EBSP to RBSP error");
        return ret;
    }
    
    srs_error("SPS Data: %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x",
              m_frame[0], m_frame[1], m_frame[2], m_frame[3], m_frame[4],
              m_frame[5], m_frame[6], m_frame[7], m_frame[8], m_frame[9]);
    
    
    int frame_crop_left_offset=0;
    int frame_crop_right_offset=0;
    int frame_crop_top_offset=0;
    int frame_crop_bottom_offset=0;
    
    int profile_idc = ReadBits(8);
    // int constraint_set0_flag = ReadBit();
    ReadBit();
    // int constraint_set1_flag = ReadBit();
    ReadBit();
    // int constraint_set2_flag = ReadBit();
    ReadBit();
    // int constraint_set3_flag = ReadBit();
    ReadBit();
    // int constraint_set4_flag = ReadBit();
    ReadBit();
    // int constraint_set5_flag = ReadBit();
    ReadBit();
    // int reserved_zero_2bits  = ReadBits(2);
    ReadBits(2);
    // int level_idc = ReadBits(8);
    ReadBits(8);
    // int seq_parameter_set_id = ReadExponentialGolombCode();
    ReadExponentialGolombCode();
    
    if (profile_idc != BASELINE && profile_idc != MAIN && profile_idc != EXTENDED &&
        profile_idc != FREXT_HP && profile_idc != FREXT_Hi10P && profile_idc != FREXT_Hi422 &&
        profile_idc != FREXT_Hi444 && profile_idc != FREXT_CAVLC444) {
        return ERROR_H264_SPS_PARSE_ERROR;
    }
    
    if (profile_idc == FREXT_HP || profile_idc == FREXT_Hi10P || profile_idc == FREXT_Hi422 ||
        profile_idc == FREXT_Hi444 || profile_idc == FREXT_CAVLC444) {
        int chroma_format_idc = ReadExponentialGolombCode();
        
        if (chroma_format_idc == 3) {
            // int residual_colour_transform_flag = ReadBit();
            ReadBit();
        }
        // int bit_depth_luma_minus8 = ReadExponentialGolombCode();
        ReadExponentialGolombCode();
        // int bit_depth_chroma_minus8 = ReadExponentialGolombCode();
        ReadExponentialGolombCode();
        // int qpprime_y_zero_transform_bypass_flag = ReadBit();
        ReadBit();
        int seq_scaling_matrix_present_flag = ReadBit();
        
        if (seq_scaling_matrix_present_flag) {
            for (int i = 0; i < 8; i++) {
                int seq_scaling_list_present_flag = ReadBit();
                if (seq_scaling_list_present_flag) {
                    int sizeOfScalingList = (i < 6) ? 16 : 64;
                    int lastScale = 8;
                    int nextScale = 8;
                    for (int j = 0; j < sizeOfScalingList; j++) {
                        if (nextScale != 0) {
                            int delta_scale = ReadSE();
                            nextScale = (lastScale + delta_scale + 256) % 256;
                        }
                        lastScale = (nextScale == 0) ? lastScale : nextScale;
                    }
                }
            }
        }
    }
    
    // int log2_max_frame_num_minus4 = ReadExponentialGolombCode();
    ReadExponentialGolombCode();
    int pic_order_cnt_type = ReadExponentialGolombCode();
    if (pic_order_cnt_type == 0) {
        // int log2_max_pic_order_cnt_lsb_minus4 = ReadExponentialGolombCode();
        ReadExponentialGolombCode();
    } else if (pic_order_cnt_type == 1) {
        // int delta_pic_order_always_zero_flag = ReadBit();
        ReadBit();
        // int offset_for_non_ref_pic = ReadSE();
        ReadSE();
        // int offset_for_top_to_bottom_field = ReadSE();
        ReadSE();
        int num_ref_frames_in_pic_order_cnt_cycle = ReadExponentialGolombCode();
        
        for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            // sps->offset_for_ref_frame[i] = ReadSE();
            ReadSE();
        }
    }
    
    // int max_num_ref_frames = ReadExponentialGolombCode();
    ReadExponentialGolombCode();
    // int gaps_in_frame_num_value_allowed_flag = ReadBit();
    ReadBit();
    int pic_width_in_mbs_minus1 = ReadExponentialGolombCode();
    int pic_height_in_map_units_minus1 = ReadExponentialGolombCode();
    int frame_mbs_only_flag = ReadBit();
    if (!frame_mbs_only_flag) {
        // int mb_adaptive_frame_field_flag = ReadBit();
        ReadBit();
    }
    
    // int direct_8x8_inference_flag = ReadBit();
    ReadBit();
    int frame_cropping_flag = ReadBit();
    if (frame_cropping_flag) {
        frame_crop_left_offset = ReadExponentialGolombCode();
        frame_crop_right_offset = ReadExponentialGolombCode();
        frame_crop_top_offset = ReadExponentialGolombCode();
        frame_crop_bottom_offset = ReadExponentialGolombCode();
    }
    
    // int vui_parameters_present_flag = ReadBit();
    ReadBit();
    
    metadata.width = ((pic_width_in_mbs_minus1 + 1) * 16) - frame_crop_bottom_offset * 2 - frame_crop_top_offset * 2;
    metadata.height = ((2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16) - (frame_crop_right_offset * 2) - (frame_crop_left_offset * 2);
    srs_error("SPS Data: %u %u", metadata.width, metadata.height);
    return ret;
}


uint32_t SpsParser::ReadBit()
{
    assert(m_parse_pos <= m_nb_frame * 8);
    
    uint32_t nIndex = m_parse_pos / 8;
    uint32_t nOffset = m_parse_pos % 8 + 1;
    
    ++m_parse_pos;
    return (m_frame[nIndex] >> (8-nOffset)) & 0x01;
}


uint32_t SpsParser::ReadBits(uint32_t n)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) {
        r |= (ReadBit() << (n - i - 1));
    }
    return r;
}


uint32_t SpsParser::ReadExponentialGolombCode()
{
    uint32_t i = 0;
    while((ReadBit() == 0) && (i < 32))
        ++i;
    
    uint32_t r = ReadBits(i);
    r += (1 << i) - 1;
    return r;
}


uint32_t SpsParser::ReadSE()
{
    uint32_t r = ReadExponentialGolombCode();
    if (r & 0x01)
        r = (r + 1) / 2;
    else
        r = -(r / 2);

    return r;
}


int SpsParser::EBSPtoRBSP()
{
    int count = 0;
    int j = 0;
    for (int i = 0; i < m_nb_frame; ++i)
    {
        if ((count == 2) && (m_frame[i] < 0x03))
            return ERROR_H264_SPS_EBSP_ERROR;
        
        if ((count == 2) && (m_frame[i] == 0x03))
        {
            if ((i < m_nb_frame - 1) && (m_frame[i + 1] > 0x03))
                return ERROR_H264_SPS_EBSP_ERROR;
            
            if (i == m_nb_frame - 1) {
                m_nb_frame = j;
                return ERROR_SUCCESS;
            }
            
            ++i;
            count = 0;
        }
        
        m_frame[j] = m_frame[i];
        if (m_frame[i] == 0x00)
            ++count;
        else
            count = 0;
        ++j;
    }
    
    m_nb_frame = j;
    return ERROR_SUCCESS;
}



