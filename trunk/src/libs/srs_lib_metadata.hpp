#pragma once
#include <stdint.h>


struct MetaData
{
    int width;
    int height;
};


class SpsParser
{
public:
    SpsParser(const uint8_t *frame, int nb_frame);
    
    
    ~SpsParser();

    
    int ParseSps(MetaData &metadata);
    
    
private:
    uint32_t ReadBit();
    
    
    uint32_t ReadBits(uint32_t n);
    
    
    uint32_t ReadExponentialGolombCode();
    
    
    uint32_t ReadSE();
    
    
private:
    const uint8_t  *m_frame;
    uint32_t        m_nb_frame;
    uint32_t        m_parse_pos;
};
