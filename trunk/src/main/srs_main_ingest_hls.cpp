/*
The MIT License (MIT)

Copyright (c) 2013-2015 SRS(ossrs)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <srs_core.hpp>

#include <srs_kernel_error.hpp>
#include <srs_app_server.hpp>
#include <srs_app_config.hpp>
#include <srs_app_log.hpp>
#include <srs_kernel_utility.hpp>
//#include <srs_rtmp_stack.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_stream.hpp>
#include <srs_kernel_ts.hpp>
#include <srs_app_http_client.hpp>
#include <srs_core_autofree.hpp>
#include <srs_app_st.hpp>
//#include <srs_rtmp_utility.hpp>
#include <srs_app_st.hpp>
#include <srs_app_utility.hpp>
//#include <srs_rtmp_amf0.hpp>
#include <srs_raw_avc.hpp>
#include <srs_app_http_conn.hpp>
#include <srs_kernel_flv.hpp>

//#define SRS_AUTO_STREAM_CASTER 1

#include <srs_app_caster_flv.hpp>

#include <stdlib.h>
#include <string>
#include <vector>
#include <map>

using namespace std;

#define SRS_HTTP_FLV_STREAM_BUFFER 4096

// pre-declare
int proxy_hls2h264(std::string hls, std::string h264);

// for the main objects(server, config, log, context),
// never subscribe handler in constructor,
// instead, subscribe handler in initialize method.
// kernel module.
ISrsLog* _srs_log = new SrsFastLog();
ISrsThreadContext* _srs_context = new ISrsThreadContext();
// app module.
SrsConfig* _srs_config = NULL;
SrsServer* _srs_server = NULL;

/**
* main entrance.
*/
int main(int argc, char** argv) 
{
    srs_assert(srs_is_little_endian());
    
    std::string in_hls_url = "http://httpflv.fastweb.com.cn.cloudcdn.net/live_fw/mosaic";
    std::string out_h264_file = "/tmp/test.flv";
    
    srs_trace("input:  %s", in_hls_url.c_str());
    srs_trace("output: %s", out_h264_file.c_str());
    
    return proxy_hls2h264(in_hls_url, out_h264_file);
}

/**
 * the http wrapper for file reader,
 * to read http post stream like a file.
 */
class SrsHttpFileReader : public SrsFileReader
{
private:
    ISrsHttpResponseReader* http;
public:
    SrsHttpFileReader(ISrsHttpResponseReader* h);
    virtual ~SrsHttpFileReader();
public:
    /**
     * open file reader, can open then close then open...
     */
    virtual int open(std::string file);
    virtual void close();
public:
    // TODO: FIXME: extract interface.
    virtual bool is_open();
    virtual int64_t tellg();
    virtual void skip(int64_t size);
    virtual int64_t lseek(int64_t offset);
    virtual int64_t filesize();
public:
    /**
     * read from file.
     * @param pnread the output nb_read, NULL to ignore.
     */
    virtual int read(void* buf, size_t count, ssize_t* pnread);
};

SrsHttpFileReader::SrsHttpFileReader(ISrsHttpResponseReader* h)
{
    http = h;
}

SrsHttpFileReader::~SrsHttpFileReader()
{
}

int SrsHttpFileReader::open(std::string /*file*/)
{
    return ERROR_SUCCESS;
}

void SrsHttpFileReader::close()
{
}

bool SrsHttpFileReader::is_open()
{
    return true;
}

int64_t SrsHttpFileReader::tellg()
{
    return 0;
}

void SrsHttpFileReader::skip(int64_t /*size*/)
{
}

int64_t SrsHttpFileReader::lseek(int64_t offset)
{
    return offset;
}

int64_t SrsHttpFileReader::filesize()
{
    return 0;
}

int SrsHttpFileReader::read(void* buf, size_t count, ssize_t* pnread)
{
    int ret = ERROR_SUCCESS;

    if (http->eof()) {
        ret = ERROR_HTTP_REQUEST_EOF;
        srs_error("flv: encoder EOF. ret=%d", ret);
        return ret;
    }
//    http->enter_infinite_chunked();


    int total_read = 0;
    while (total_read < (int)count) {
        int nread = 0;
        if ((ret = http->read((char*)buf + total_read, (int)(count - total_read), &nread)) != ERROR_SUCCESS) {
            return ret;
        }

        if (nread == 0) {
            ret = ERROR_HTTP_REQUEST_EOF;
            srs_warn("flv: encoder read EOF. ret=%d", ret);
            break;
        }

        srs_assert(nread);
        total_read += nread;
    }

    if (pnread) {
        *pnread = total_read;
    }

    return ret;
}

class ISrsAacHandler
{
public:
    /**
     * handle the aac frame, which in ADTS format(starts with FFFx).
     * @param duration the duration in seconds of frames.
*/
virtual int on_aac_frame(char* frame, int frame_size, double duration) = 0;
};

// the context to ingest hls stream.
class SrsIngestSrsInput
{
private:
    struct SrsTsPiece {
        double duration;
        std::string url;
        std::string body;
        
        // should skip this ts?
        bool skip;
        // already sent to rtmp server?
        bool sent;
        // whether ts piece is dirty, remove if not update.
        bool dirty;
        
        SrsTsPiece() {
            skip = false;
            sent = false;
            dirty = false;
        }
        
        int fetch(std::string m3u8);
    };
    
public:
    SrsIngestSrsInput(SrsHttpUri* hls, const std::string &h264_file) {
        in_hls = hls;
        next_connect_time = 0;
        
        stream = new SrsStream();
        context = new SrsTsContext();
        pf = std::fopen(h264_file.c_str(), "wb");
    }
    
    ~SrsIngestSrsInput() {
        srs_freep(stream);
        srs_freep(context);
        
        std::vector<SrsTsPiece*>::iterator it;
        for (it = pieces.begin(); it != pieces.end(); ++it) {
            SrsTsPiece* tp = *it;
            srs_freep(tp);
        }
        pieces.clear();
    }
    /**
     * parse the input hls live m3u8 index.
     */
    virtual int connect();
    /**
     * parse the ts and use hanler to process the message.
     */
    virtual int parse(ISrsTsHandler* ts, ISrsAacHandler* aac);

    int do_proxy(ISrsHttpResponseReader* rr, SrsFlvDecoder* dec);
private:
    /**
     * parse the ts pieces body.
     */
    virtual int parseAac(ISrsAacHandler* handler, char* body, int nb_body, double duration);
    virtual int parseTs(ISrsTsHandler* handler, char* body, int nb_body);
    /**
     * parse the m3u8 specified by url.
     */
    virtual int parseM3u8(SrsHttpUri* url, double& td, double& duration);
    /**
     * find the ts piece by its url.
     */
    virtual SrsTsPiece* find_ts(string url);
    /**
     * set all ts to dirty.
     */
    virtual void dirty_all_ts();
    /**
     * fetch all ts body.
     */
    virtual int fetch_all_ts(bool fresh_m3u8);
    /**
     * remove all ts which is dirty.
     */
    virtual void remove_dirty();
    
    /**
     * parse flv video data to h264 data
     */
    void parse_video_data(char *data, int length, int count);
    
    /**
     * parse flv audio data to aac data
     */
    void parse_audio_data(char *data, int length, int count);
    
private:
    SrsHttpUri* in_hls;
    std::vector<SrsTsPiece*> pieces;
    int64_t next_connect_time;
    SrsStream* stream;
    SrsTsContext* context;
    
    FILE *pf;
};

int SrsIngestSrsInput::do_proxy(ISrsHttpResponseReader* rr, SrsFlvDecoder* dec)
{
    int ret = ERROR_SUCCESS;

    char pps[4];
    int count = 0;
    while (!rr->eof()) {
        char tag_header[11];
        char type;
        int32_t size;
        u_int32_t time;
        if ((ret = dec->read_tag_header(&type, &size, &time, tag_header)) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_error("flv: proxy tag header failed. ret=%d", ret);
            }
            return ret;
        }

        std::vector<char> data(size);
        if ((ret = dec->read_tag_data(&data[0], size)) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_error("flv: proxy tag data failed. ret=%d", ret);
            }
            return ret;
        }

        if (static_cast<int>(type) == 9) {
            parse_video_data(&data[0], size, count);
        } else if (static_cast<int>(type) == 8) {
            parse_audio_data(&data[0], size, count);
        }

        if ((ret = dec->read_previous_tag_size(pps)) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_error("flv: proxy tag header pps failed. ret=%d", ret);
            }
            return ret;
        }
    }

    return ret;
}


void SrsIngestSrsInput::parse_video_data(char *data, int length, int count)
{
    std::vector<char> raw_data;
    
    char *cursor = data;
    cursor += 1;
    uint32_t type = static_cast<uint32_t>(*cursor);
    cursor += 4;
    
    if (type == SrsCodecVideoAVCTypeSequenceHeader) {
        cursor += 5;
        
        // sps
        cursor += 1;
        uint32_t sps_len = static_cast<uint32_t>(
            static_cast<uint8_t>(*cursor) << 8 | static_cast<uint8_t>(*(++cursor)));
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x01);
        
        if (13 + sps_len + 3 > length) {
            srs_error("SPS parse error: package %d, data length %d, parser length %d", count, length, 13 + sps_len + 3);
            return;
        }
        
        cursor += 1;
        std::copy(cursor, cursor + sps_len, std::back_inserter(raw_data));
        cursor += sps_len;
        
        // pps
        cursor += 1;
        uint32_t pps_len = static_cast<uint32_t>(
            static_cast<uint8_t>(*cursor) << 8 | static_cast<uint8_t>(*(++cursor)));
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x01);
        
        if (13 + sps_len + 3 + pps_len != length) {
            srs_error("SPS/PPS parse error: package %d, data length %d, parser length %d", count, length, 13 + sps_len + 3 + pps_len);
            return;
        }
        
        cursor += 1;
        std::copy(cursor, cursor + pps_len, std::back_inserter(raw_data));
    }
    
    if (type == SrsCodecVideoAVCTypeNALU) {
        uint32_t ipb_len = static_cast<uint32_t>(
            static_cast<uint8_t>(*cursor) << 24 | static_cast<uint8_t>(*(++cursor)) << 16 |
            static_cast<uint8_t>(*(++cursor)) << 8 | static_cast<uint8_t>(*(++cursor)));
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x00);
        raw_data.push_back(0x01);
        
        if (9 + ipb_len != length) {
            srs_error("I/P/B parse error: package %d, data length %d, parser length %d, data:[%02X][%02X][%02X][%02X]",
                      count, length, 9 + ipb_len, static_cast<uint8_t>(data[5]), static_cast<uint8_t>(data[6]),
                      static_cast<uint8_t>(data[7]), static_cast<uint8_t>(data[8]));
            return;
        }
        
        cursor += 1;
        std::copy(cursor, data + length, std::back_inserter(raw_data));
    }
    
    std::fwrite(&raw_data[0], 1, raw_data.size(), pf);
}

void SrsIngestSrsInput::parse_audio_data(char *data, int length, int count)
{
    uint8_t tag = data[0];
    uint8_t type = tag&0xF0;
    
    if (type != SrsCodecAudioAAC) {
        srs_error("audio data is not aac");
        return;
    }
    
    
}

int SrsIngestSrsInput::connect()
{
    int ret = ERROR_SUCCESS;
    
    int64_t now = srs_update_system_time_ms();
    if (now < next_connect_time) {
        srs_trace("input hls wait for %dms", next_connect_time - now);
        st_usleep((next_connect_time - now) * 1000);
    }

    SrsHttpClient client;
    srs_trace("parse input hls %s", in_hls->get_url());

    if ((ret = client.initialize(in_hls->get_host(), in_hls->get_port())) != ERROR_SUCCESS) {
        srs_error("connect to server failed. ret=%d", ret);
        return ret;
    }

    ISrsHttpMessage* msg = NULL;
    if ((ret = client.get(in_hls->get_path(), "", &msg)) != ERROR_SUCCESS) {
        srs_error("HTTP GET %s failed. ret=%d", in_hls->get_url(), ret);
        return ret;
    }

    srs_assert(msg);
    SrsAutoFree(ISrsHttpMessage, msg);

//    std::string body;
//    if ((ret = msg->body_read_all(body)) != ERROR_SUCCESS) {
//        srs_error("read m3u8 failed. ret=%d", ret);
//        return ret;
//    }
//
//    if (body.empty()) {
//        srs_warn("ignore empty m3u8");
//        return ret;
//    }

    srs_trace("flv: proxy uri: %s ", msg->uri().c_str());
    srs_trace("flv: proxy path: %s ", msg->uri().c_str());

    char* buffer = new char[SRS_HTTP_FLV_STREAM_BUFFER];
    SrsAutoFreeA(char, buffer);

//    std::string body;
//    if ((ret = msg->body_read_all(body)) != ERROR_SUCCESS) {
//        srs_error("read m3u8 failed. ret=%d", ret);
//        return ret;
//    }

    ISrsHttpResponseReader* rr = msg->body_reader();
    SrsHttpFileReader reader(rr);

    SrsFlvDecoder dec;

    if ((ret = dec.initialize(&reader)) != ERROR_SUCCESS) {
        return ret;
    }


//    flvWriter.open("out.flv");
//    pf = std::fopen("/tmp/test.h264", "wb");
//    if (!flvWriter.is_open() || pf == NULL)
//    {
//        return -1;
//    }

    char header[9];
    if ((ret = dec.read_header(header)) != ERROR_SUCCESS) {
        if (!srs_is_client_gracefully_close(ret)) {
            srs_error("flv: proxy flv header failed. ret=%d", ret);
        }
        return ret;
    }

    srs_trace("flv: proxy drop flv header.");

    char pps[4];
    if ((ret = dec.read_previous_tag_size(pps)) != ERROR_SUCCESS) {
        if (!srs_is_client_gracefully_close(ret)) {
            srs_error("flv: proxy flv header pps failed. ret=%d", ret);
        }
        return ret;
    }

    ret = do_proxy(rr, &dec);

//    // set all ts to dirty.
//    dirty_all_ts();
//    
//    bool fresh_m3u8 = pieces.empty();
//    double td = 0.0;
//    double duration = 0.0;
//    if ((ret = parseM3u8(in_hls, td, duration)) != ERROR_SUCCESS) {
//        return ret;
//    }
//    
//    // fetch all ts.
//    if ((ret = fetch_all_ts(fresh_m3u8)) != ERROR_SUCCESS) {
//        srs_error("fetch all ts failed. ret=%d", ret);
//        return ret;
//    }
//    
//    // remove all dirty ts.
//    remove_dirty();
//    
//    srs_trace("fetch m3u8 ok, td=%.2f, duration=%.2f, pieces=%d", td, duration, pieces.size());

    return ret;
}

int SrsIngestSrsInput::parse(ISrsTsHandler* ts, ISrsAacHandler* aac)
{
    int ret = ERROR_SUCCESS;
    
    for (int i = 0; i < (int)pieces.size(); i++) {
        SrsTsPiece* tp = pieces.at(i);
        
        // sent only once.
        if (tp->sent) {
            continue;
        }
        tp->sent = true;
        
        if (tp->body.empty()) {
            continue;
        }
        
        srs_trace("proxy the ts to rtmp, ts=%s, duration=%.2f", tp->url.c_str(), tp->duration);
        
        if (srs_string_ends_with(tp->url, ".ts")) {
            if ((ret = parseTs(ts, (char*)tp->body.data(), (int)tp->body.length())) != ERROR_SUCCESS) {
                return ret;
            }
        } else if (srs_string_ends_with(tp->url, ".aac")) {
            if ((ret = parseAac(aac, (char*)tp->body.data(), (int)tp->body.length(), tp->duration)) != ERROR_SUCCESS) {
                return ret;
            }
        } else {
            srs_warn("ignore unkown piece %s", tp->url.c_str());
        }
    }
    
    return ret;
}

int SrsIngestSrsInput::parseTs(ISrsTsHandler* handler, char* body, int nb_body)
{
    int ret = ERROR_SUCCESS;
    
    // use stream to parse ts packet.
    int nb_packet =  (int)nb_body / SRS_TS_PACKET_SIZE;
    for (int i = 0; i < nb_packet; i++) {
        char* p = (char*)body + (i * SRS_TS_PACKET_SIZE);
        if ((ret = stream->initialize(p, SRS_TS_PACKET_SIZE)) != ERROR_SUCCESS) {
            return ret;
        }
        
        // process each ts packet
        if ((ret = context->decode(stream, handler)) != ERROR_SUCCESS) {
            srs_error("mpegts: ignore parse ts packet failed. ret=%d", ret);
            return ret;
        }
        srs_info("mpegts: parse ts packet completed");
    }
    srs_info("mpegts: parse udp packet completed");
    
    return ret;
}

int SrsIngestSrsInput::parseAac(ISrsAacHandler* handler, char* body, int nb_body, double duration)
{
    int ret = ERROR_SUCCESS;
    
    if ((ret = stream->initialize(body, nb_body)) != ERROR_SUCCESS) {
        return ret;
    }
    
    // atleast 2bytes.
    if (!stream->require(3)) {
        ret = ERROR_AAC_BYTES_INVALID;
        srs_error("invalid aac, atleast 3bytes. ret=%d", ret);
        return ret;
    }
    
    u_int8_t id0 = (u_int8_t)body[0];
    u_int8_t id1 = (u_int8_t)body[1];
    u_int8_t id2 = (u_int8_t)body[2];
    
    // skip ID3.
    if (id0 == 0x49 && id1 == 0x44 && id2 == 0x33) {
        /*char id3[] = {
            (char)0x49, (char)0x44, (char)0x33, // ID3
            (char)0x03, (char)0x00, // version
            (char)0x00, // flags
            (char)0x00, (char)0x00, (char)0x00, (char)0x0a, // size
            
            (char)0x00, (char)0x00, (char)0x00, (char)0x00, // FrameID
            (char)0x00, (char)0x00, (char)0x00, (char)0x00, // FrameSize
            (char)0x00, (char)0x00 // Flags
         };*/
        // atleast 10 bytes.
        if (!stream->require(10)) {
            ret = ERROR_AAC_BYTES_INVALID;
            srs_error("invalid aac ID3, atleast 10bytes. ret=%d", ret);
            return ret;
        }
        
        // ignore ID3 + version + flag.
        stream->skip(6);
        // read the size of ID3.
        u_int32_t nb_id3 = stream->read_4bytes();
        
        // read body of ID3
        if (!stream->require(nb_id3)) {
            ret = ERROR_AAC_BYTES_INVALID;
            srs_error("invalid aac ID3 body, required %dbytes. ret=%d", nb_id3, ret);
            return ret;
        }
        stream->skip(nb_id3);
    }
    
    char* frame = body + stream->pos();
    int frame_size = nb_body - stream->pos();
    return handler->on_aac_frame(frame, frame_size, duration);
}

int SrsIngestSrsInput::parseM3u8(SrsHttpUri* url, double& td, double& duration)
{
    int ret = ERROR_SUCCESS;
    
    SrsHttpClient client;
    srs_trace("parse input hls %s", url->get_url());
    
    if ((ret = client.initialize(url->get_host(), url->get_port())) != ERROR_SUCCESS) {
        srs_error("connect to server failed. ret=%d", ret);
        return ret;
    }
    
    ISrsHttpMessage* msg = NULL;
    if ((ret = client.get(url->get_path(), "", &msg)) != ERROR_SUCCESS) {
        srs_error("HTTP GET %s failed. ret=%d", url->get_url(), ret);
        return ret;
    }
    
    srs_assert(msg);
    SrsAutoFree(ISrsHttpMessage, msg);
    
    std::string body;
    if ((ret = msg->body_read_all(body)) != ERROR_SUCCESS) {
        srs_error("read m3u8 failed. ret=%d", ret);
        return ret;
    }
    
    if (body.empty()) {
        srs_warn("ignore empty m3u8");
        return ret;
    }
    
    std::string ptl;
    while (!body.empty()) {
        size_t pos = string::npos;
        
        std::string line;
        if ((pos = body.find("\n")) != string::npos) {
            line = body.substr(0, pos);
            body = body.substr(pos + 1);
        } else {
            line = body;
            body = "";
        }
        
        line = srs_string_replace(line, "\r", "");
        line = srs_string_replace(line, " ", "");
        
        // #EXT-X-VERSION:3
        // the version must be 3.0
        if (srs_string_starts_with(line, "#EXT-X-VERSION:")) {
            if (!srs_string_ends_with(line, ":3")) {
                srs_warn("m3u8 3.0 required, actual is %s", line.c_str());
            }
            continue;
        }
        
        // #EXT-X-PLAYLIST-TYPE:VOD
        // the playlist type, vod or nothing.
        if (srs_string_starts_with(line, "#EXT-X-PLAYLIST-TYPE:")) {
            ptl = line;
            continue;
        }
        
        // #EXT-X-TARGETDURATION:12
        // the target duration is required.
        if (srs_string_starts_with(line, "#EXT-X-TARGETDURATION:")) {
            td = ::atof(line.substr(string("#EXT-X-TARGETDURATION:").length()).c_str());
        }
        
        // #EXT-X-ENDLIST
        // parse completed.
        if (line == "#EXT-X-ENDLIST") {
            break;
        }
        
        // #EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=73207,CODECS="mp4a.40.2"
        if (srs_string_starts_with(line, "#EXT-X-STREAM-INF:")) {
            if ((pos = body.find("\n")) == string::npos) {
                srs_warn("m3u8 entry unexpected eof, inf=%s", line.c_str());
                break;
            }
            
            std::string m3u8_url = body.substr(0, pos);
            body = body.substr(pos + 1);
            
            if (!srs_string_is_http(m3u8_url)) {
                m3u8_url = srs_path_dirname(url->get_url()) + "/" + m3u8_url;
            }
            srs_trace("parse sub m3u8, url=%s", m3u8_url.c_str());
            
            if ((ret = url->initialize(m3u8_url)) != ERROR_SUCCESS) {
                return ret;
            }
            
            return parseM3u8(url, td, duration);
        }
        
        // #EXTINF:11.401,
        // livestream-5.ts
        // parse each ts entry, expect current line is inf.
        if (!srs_string_starts_with(line, "#EXTINF:")) {
            continue;
        }
        
        // expect next line is url.
        std::string ts_url;
        if ((pos = body.find("\n")) != string::npos) {
            ts_url = body.substr(0, pos);
            body = body.substr(pos + 1);
        } else {
            srs_warn("ts entry unexpected eof, inf=%s", line.c_str());
            break;
        }
        
        // parse the ts duration.
        line = line.substr(string("#EXTINF:").length());
        if ((pos = line.find(",")) != string::npos) {
            line = line.substr(0, pos);
        }
        
        double ts_duration = ::atof(line.c_str());
        duration += ts_duration;
        
        SrsTsPiece* tp = find_ts(ts_url);
        if (!tp) {
            tp = new SrsTsPiece();
            tp->url = ts_url;
            tp->duration = ts_duration;
            pieces.push_back(tp);
        } else {
            tp->dirty = false;
        }
    }
    
    return ret;
}

SrsIngestSrsInput::SrsTsPiece* SrsIngestSrsInput::find_ts(string url)
{
    std::vector<SrsTsPiece*>::iterator it;
    for (it = pieces.begin(); it != pieces.end(); ++it) {
        SrsTsPiece* tp = *it;
        if (tp->url == url) {
            return tp;
        }
    }
    return NULL;
}

void SrsIngestSrsInput::dirty_all_ts()
{
    std::vector<SrsTsPiece*>::iterator it;
    for (it = pieces.begin(); it != pieces.end(); ++it) {
        SrsTsPiece* tp = *it;
        tp->dirty = true;
    }
}

int SrsIngestSrsInput::fetch_all_ts(bool fresh_m3u8)
{
    int ret = ERROR_SUCCESS;
    
    for (int i = 0; i < (int)pieces.size(); i++) {
        SrsTsPiece* tp = pieces.at(i);
        
        // when skipped, ignore.
        if (tp->skip) {
            continue;
        }
        
        // for the fresh m3u8, skip except the last one.
        if (fresh_m3u8 && i != (int)pieces.size() - 1) {
            tp->skip = true;
            continue;
        }
        
        if ((ret = tp->fetch(in_hls->get_url())) != ERROR_SUCCESS) {
            srs_error("fetch ts %s for error. ret=%d", tp->url.c_str(), ret);
            tp->skip = true;
            return ret;
        }
        
        // only wait for a duration of last piece.
        if (i == (int)pieces.size() - 1) {
            next_connect_time = srs_update_system_time_ms() + (int)tp->duration * 1000;
        }
    }
    
    return ret;
}


void SrsIngestSrsInput::remove_dirty()
{
    std::vector<SrsTsPiece*>::iterator it;
    for (it = pieces.begin(); it != pieces.end();) {
        SrsTsPiece* tp = *it;
        
        if (tp->dirty) {
            srs_trace("erase dirty ts, url=%s, duration=%.2f", tp->url.c_str(), tp->duration);
            srs_freep(tp);
            it = pieces.erase(it);
        } else {
            ++it;
        }
    }
}

int SrsIngestSrsInput::SrsTsPiece::fetch(string m3u8)
{
    int ret = ERROR_SUCCESS;
    
    if (skip || sent || !body.empty()) {
        return ret;
    }
    
    SrsHttpClient client;
    
    std::string ts_url = url;
    if (!srs_string_is_http(ts_url)) {
        ts_url = srs_path_dirname(m3u8) + "/" + url;
    }
    
    SrsHttpUri uri;
    if ((ret = uri.initialize(ts_url)) != ERROR_SUCCESS) {
        return ret;
    }
    
    // initialize the fresh http client.
    if ((ret = client.initialize(uri.get_host(), uri.get_port()) != ERROR_SUCCESS)) {
        return ret;
    }
    
    ISrsHttpMessage* msg = NULL;
    if ((ret = client.get(uri.get_path(), "", &msg)) != ERROR_SUCCESS) {
        srs_error("HTTP GET %s failed. ret=%d", uri.get_url(), ret);
        return ret;
    }
    
    srs_assert(msg);
    SrsAutoFree(ISrsHttpMessage, msg);
    
    if ((ret = msg->body_read_all(body)) != ERROR_SUCCESS) {
        srs_error("read ts failed. ret=%d", ret);
        return ret;
    }
    
    srs_trace("fetch ts ok, duration=%.2f, url=%s, body=%dB", duration, url.c_str(), body.length());
    
    return ret;
}

// the context for ingest hls stream.
class SrsIngestSrsContext
{
public:
    SrsIngestSrsContext(SrsHttpUri* hls, const std::string& h264) {
        ic = new SrsIngestSrsInput(hls, h264);
    }
    
    ~SrsIngestSrsContext() {
        srs_freep(ic);
    }
    
    int proxy() {
        int ret = ERROR_SUCCESS;
        
        if ((ret = ic->connect()) != ERROR_SUCCESS) {
            srs_error("connect ic failed. ret=%d", ret);
            return ret;
        }

        return ret;
    }

private:
    SrsIngestSrsInput* ic;

};

int proxy_hls2h264(std::string hls, std::string h264)
{
    int ret = ERROR_SUCCESS;
    
    // init st.
    if ((ret = srs_st_init()) != ERROR_SUCCESS) {
        srs_error("init st failed. ret=%d", ret);
        return ret;
    }
    
    SrsHttpUri hls_uri;
    if ((ret = hls_uri.initialize(hls)) != ERROR_SUCCESS) {
        srs_error("hls uri invalid. ret=%d", ret);
        return ret;
    }

    SrsIngestSrsContext context(&hls_uri, h264);
    for (;;) {
        if ((ret = context.proxy()) != ERROR_SUCCESS) {
            srs_error("proxy hls to h264 failed. ret=%d", ret);
            return ret;
        }
    }
    
    return ret;
}

