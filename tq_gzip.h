#pragma once
// 极简 gzip / DEFLATE (RFC 1951/1952) 解压器 —— 纯 C++，零外部依赖。
// 用途：解开在线星表站发布的 .csv.gz（Windows Compression API 不支持标准 GZIP）。
#include <string>
#include <cstring>
#include <cstdint>
#include "tq_util.h"

namespace tq { namespace gzip {

struct BitReader {
    const unsigned char* p;
    size_t   n, pos;
    uint32_t buf;
    int      cnt;
    bool     eof;
    BitReader(const unsigned char* d, size_t len)
        : p(d), n(len), pos(0), buf(0), cnt(0), eof(false) {}
    int Bit() {
        if (!cnt) {
            if (pos >= n) { eof = true; return 0; }
            buf = p[pos++]; cnt = 8;
        }
        int b = (int)(buf & 1u);
        buf >>= 1; --cnt;
        return b;
    }
    int Read(int k) {
        int v = 0;
        for (int i = 0; i < k; ++i) v |= Bit() << i;
        return v;
    }
    void Align() { buf = 0; cnt = 0; }
};

// 规范 Huffman 码表（canonical decode，同 zlib puff.c 的算法）
struct Huff {
    int count[16];
    int symbol[288];
    Huff() { std::memset(count, 0, sizeof(count)); std::memset(symbol, 0, sizeof(symbol)); }
    bool Build(const unsigned char* lens, int n) {
        std::memset(count, 0, sizeof(count));
        for (int i = 0; i < n; ++i) {
            if (lens[i] > 15) return false;
            ++count[lens[i]];
        }
        count[0] = 0;
        int offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; ++i)
            if (lens[i]) symbol[offs[lens[i]]++] = i;
        return true;
    }
    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= br.Bit();
            int c = count[len];
            if (code - first < c) return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
            if (br.eof) return -1;
        }
        return -1;
    }
};

static const int kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
    2049,3073,4097,6145,8193,12289,16385,24577
};
static const int kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const unsigned char kClOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

inline bool Inflate(BitReader& br, std::string& out, size_t maxOut, const char** err) {
    Huff lit, dist;
    for (;;) {
        int last = br.Bit();
        int type = br.Read(2);
        if (type == 0) {                       // 存储块
            br.Align();
            if (br.pos + 4 > br.n) { *err = "truncated stored block"; return false; }
            int len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
            int nlen = br.p[br.pos + 2] | (br.p[br.pos + 3] << 8);
            if ((len ^ 0xFFFF) != nlen) { *err = "stored block length mismatch"; return false; }
            br.pos += 4;
            if (br.pos + (size_t)len > br.n) { *err = "truncated stored data"; return false; }
            if (out.size() + (size_t)len > maxOut) { *err = "output too large"; return false; }
            out.append(reinterpret_cast<const char*>(br.p + br.pos), (size_t)len);
            br.pos += (size_t)len;
        } else if (type == 1 || type == 2) {
            if (type == 1) {                   // 固定 Huffman
                unsigned char l[288], d[32];
                for (int i = 0; i < 288; ++i)
                    l[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                for (int i = 0; i < 32; ++i) d[i] = 5;
                if (!lit.Build(l, 288) || !dist.Build(d, 32)) { *err = "bad fixed tree"; return false; }
            } else {                           // 动态 Huffman
                int hlit  = br.Read(5) + 257;
                int hdist = br.Read(5) + 1;
                int hclen = br.Read(4) + 4;
                if (hlit > 286 || hdist > 30) { *err = "bad dynamic header"; return false; }
                unsigned char cl[19] = {};
                for (int i = 0; i < hclen; ++i)
                    cl[kClOrder[i]] = (unsigned char)br.Read(3);
                Huff clh;
                if (!clh.Build(cl, 19)) { *err = "bad code-length tree"; return false; }

                unsigned char lens[320] = {};
                int total = hlit + hdist, i = 0;
                while (i < total) {
                    int sym = clh.Decode(br);
                    if (sym < 0) { *err = "bad code-length symbol"; return false; }
                    if (sym < 16) lens[i++] = (unsigned char)sym;
                    else {
                        int rep = 0;
                        unsigned char val = 0;
                        if (sym == 16) {
                            if (i == 0) { *err = "repeat with no previous"; return false; }
                            val = lens[i - 1]; rep = 3 + br.Read(2);
                        } else if (sym == 17) rep = 3 + br.Read(3);
                        else                  rep = 11 + br.Read(7);
                        if (i + rep > total) { *err = "code-length overflow"; return false; }
                        while (rep-- > 0) lens[i++] = val;
                    }
                    if (br.eof) { *err = "truncated code lengths"; return false; }
                }
                if (!lit.Build(lens, hlit) || !dist.Build(lens + hlit, hdist)) {
                    *err = "bad literal/distance tree"; return false;
                }
            }

            for (;;) {                        // 解码符号流
                int sym = lit.Decode(br);
                if (sym < 0 || sym > 285) { *err = "bad literal symbol"; return false; }
                if (sym < 256) {
                    if (out.size() >= maxOut) { *err = "output too large"; return false; }
                    out.push_back((char)sym);
                } else if (sym == 256) break;
                else {
                    int li = sym - 257;
                    int length = kLenBase[li] + br.Read(kLenExtra[li]);
                    int ds = dist.Decode(br);
                    if (ds < 0 || ds >= 30) { *err = "bad distance symbol"; return false; }
                    int distance = kDistBase[ds] + br.Read(kDistExtra[ds]);
                    if (distance <= 0 || (size_t)distance > out.size()) {
                        *err = "distance out of range"; return false;
                    }
                    if (out.size() + (size_t)length > maxOut) { *err = "output too large"; return false; }
                    size_t start = out.size() - (size_t)distance;
                    for (int k = 0; k < length; ++k) {
                        char c = out[start + (size_t)k];
                        out.push_back(c);
                    }
                }
                if (br.eof) { *err = "truncated deflate stream"; return false; }
            }
        } else { *err = "reserved block type"; return false; }

        if (last) break;
        if (br.eof) { *err = "truncated stream"; return false; }
    }
    return true;
}

struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
inline uint32_t Crc32(const unsigned char* data, size_t n) {
    static const Crc32Table ct;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = ct.t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
inline uint32_t ReadLE32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 输入完整 gzip 文件，输出解压后的原始字节。
inline bool Decompress(const std::string& in, std::string& out,
                       std::wstring* err = nullptr, size_t maxOut = 128u * 1024u * 1024u) {
    auto fail = [&](const char* why) -> bool {
        if (err) *err = Utf8ToWide(std::string("gzip: ") + why);
        return false;
    };
    if (in.size() < 18) return fail("file too small");
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return fail("bad header");
    unsigned flg = p[3];
    if (flg & 0xE0) return fail("reserved flags");
    size_t pos = 10;
    if (flg & 4) {                            // FEXTRA
        if (pos + 2 > in.size()) return fail("bad extra field");
        size_t xlen = (size_t)(p[pos] | (p[pos + 1] << 8));
        pos += 2 + xlen;
    }
    if (flg & 8) {                            // FNAME
        while (pos < in.size() && p[pos]) ++pos;
        ++pos;
    }
    if (flg & 16) {                           // FCOMMENT
        while (pos < in.size() && p[pos]) ++pos;
        ++pos;
    }
    if (flg & 2) pos += 2;                    // FHCRC
    if (pos > in.size()) return fail("bad header length");

    BitReader br(p + pos, in.size() - pos);
    out.clear();
    out.reserve(1u << 20);
    const char* why = "unknown";
    if (!Inflate(br, out, maxOut, &why)) return fail(why);

    // 校验 gzip 尾部（CRC32 + 原始长度 ISIZE）：下载被截断时及时报错，
    // 避免把不完整的星表悄悄写进数据库。
    size_t tail = pos + br.pos;
    if (tail + 8 > in.size()) return fail("missing trailer (truncated download?)");
    uint32_t crc   = ReadLE32(p + tail);
    uint32_t isize = ReadLE32(p + tail + 4);
    if (isize != (uint32_t)out.size()) return fail("size mismatch (truncated download?)");
    if (Crc32(reinterpret_cast<const unsigned char*>(out.data()), out.size()) != crc)
        return fail("CRC mismatch (corrupted download)");
    return true;
}

} } // namespace tq::gzip
