#pragma once
// Внутренние крипто-примитивы для модулей:
// b64, SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256.
// (используются в nlm_crypto и nlm_pg — SCRAM-SHA-256)
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nlhash {

using u8 = uint8_t;
using u32 = uint32_t;
using Bytes = std::vector<u8>;

// -------------------- base64 --------------------
inline const char* B64() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
           "abcdefghijklmnopqrstuvwxyz0123456789+/";
}
inline std::string b64enc(const u8* d, size_t n) {
    std::string out;
    const char* B = B64();
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)d[i] << 16;
        if (i + 1 < n) v |= (u32)d[i + 1] << 8;
        if (i + 2 < n) v |= (u32)d[i + 2];
        out += B[(v >> 18) & 63];
        out += B[(v >> 12) & 63];
        out += (i + 1 < n) ? B[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? B[v & 63] : '=';
    }
    return out;
}
inline std::string b64enc(const std::string& s) {
    return b64enc((const u8*)s.data(), s.size());
}
inline std::string b64dec(const std::string& s) {
    int T[256];
    std::memset(T, -1, sizeof(T));
    const char* B = B64();
    for (int i = 0; i < 64; ++i) T[(u8)B[i]] = i;
    std::string out;
    u32 v = 0; int nb = 0;
    for (char c : s) {
        int t = T[(u8)c];
        if (t < 0) continue;
        v = (v << 6) | (u32)t;
        nb += 6;
        if (nb >= 8) { nb -= 8; out += (char)((v >> nb) & 0xFF); }
    }
    return out;
}

// -------------------- SHA-256 --------------------
struct Sha256 {
    u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8 buf[64]; size_t buflen = 0; uint64_t total = 0;
    static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const u8* p) {
        static const u32 K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,
            0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
            0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,
            0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
            0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,
            0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,
            0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,
            0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
            0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,
            0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (u32)p[i*4]<<24 | (u32)p[i*4+1]<<16 |
                   (u32)p[i*4+2]<<8 | p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            u32 s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            u32 s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            u32 S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            u32 ch = (e&f)^(~e&g);
            u32 t1 = hh+S1+ch+K[i]+w[i];
            u32 S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            u32 mj = (a&b)^(a&c)^(b&c);
            u32 t2 = S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void update(const u8* p, size_t n) {
        total += n;
        while (n) {
            size_t k = 64 - buflen;
            if (k > n) k = n;
            std::memcpy(buf + buflen, p, k);
            buflen += k; p += k; n -= k;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }
    Bytes finish() {
        uint64_t bits = total * 8;
        u8 pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (buflen != 56) update(&pad, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = (u8)(bits >> (56 - 8*i));
        update(len, 8);
        Bytes out(32);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j)
                out[i*4+j] = (u8)(h[i] >> (24 - 8*j));
        return out;
    }
};

inline Bytes sha256(const std::string& s) {
    Sha256 x;
    x.update((const u8*)s.data(), s.size());
    return x.finish();
}
inline Bytes hmac_sha256(const std::string& key, const std::string& msg) {
    std::string k = key.size() > 64 ? std::string(
        (const char*)sha256(key).data(), 32) : key;
    k.resize(64, '\0');
    std::string o(64, '\x5c'), i(64, '\x36');
    for (int j = 0; j < 64; ++j) { o[j] ^= k[j]; i[j] ^= k[j]; }
    return sha256(o + std::string((const char*)
        sha256(i + msg).data(), 32));
}
// PBKDF2-HMAC-SHA256, 1 блок (32 байта) — достаточно для SCRAM
inline Bytes pbkdf2(const std::string& pass, const std::string& salt,
                    int iter) {
    std::string msg = salt + '\0' + '\0' + '\0' + '\1';
    Bytes u = hmac_sha256(pass, msg);
    Bytes r = u;
    for (int i = 1; i < iter; ++i) {
        u = hmac_sha256(pass, std::string((const char*)u.data(), 32));
        for (int j = 0; j < 32; ++j) r[j] ^= u[j];
    }
    return r;
}
inline std::string hex(const u8* d, size_t n) {
    static const char H[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += H[d[i] >> 4]; out += H[d[i] & 15];
    }
    return out;
}
inline std::string hex(const std::string& s) {
    return hex((const u8*)s.data(), s.size());
}

// -------------------- MD5 (для pg auth) --------------------
inline Bytes md5(const std::string& s) {
    static const u32 T[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,
        0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,
        0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,
        0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,
        0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,
        0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,
        0xe6db99e5,0x1fa27cf8,0xc4ac5665,0xf4292244,0x432aff97,
        0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,
        0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    std::string msg = s;
    uint64_t bits = s.size() * 8;
    msg += '\x80';
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 0; i < 8; ++i) msg += (char)(u8)(bits >> (8 * i));
    u32 a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe,
        d0 = 0x10325476;
    for (size_t off = 0; off < msg.size(); off += 64) {
        u32 m[16];
        const u8* p = (const u8*)msg.data() + off;
        for (int i = 0; i < 16; ++i)
            m[i] = (u32)p[i*4] | (u32)p[i*4+1]<<8 |
                   (u32)p[i*4+2]<<16 | (u32)p[i*4+3]<<24;
        u32 a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            u32 f; int g;
            if (i < 16)      { f = (b&c)|(~b&d);       g = i; }
            else if (i < 32) { f = (d&b)|(~d&c);       g = (5*i+1)%16; }
            else if (i < 48) { f = b^c^d;              g = (3*i+5)%16; }
            else             { f = c^(b|~d);           g = (7*i)%16; }
            u32 tmp = d; d = c; c = b;
            u32 x = a + f + T[i] + m[g];
            b = b + ((x << S[i]) | (x >> (32 - S[i])));
            a = tmp;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    Bytes out(16);
    u32 v[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i*4+j] = (u8)(v[i] >> (8*j));
    return out;
}

} // namespace nlhash
