// Модуль crypto — чистые реализации без OpenSSL:
// SHA-256 (RFC 6234), HMAC/HKDF (RFC 2104/5869),
// AES-256 в GCM-режиме (NIST SP 800-38D).
#include "nlm_crypto.h"
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

using u8 = uint8_t;
using u32 = uint32_t;
using Bytes = std::vector<u8>;

// -------------------- base64 --------------------
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string nlcrypto_b64encode(const std::string& s) {
    std::string out;
    size_t n = s.size();
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u8)s[i] << 16;
        if (i + 1 < n) v |= (u8)s[i + 1] << 8;
        if (i + 2 < n) v |= (u8)s[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? B64[v & 63] : '=';
    }
    return out;
}
std::string nlcrypto_b64decode(const std::string& s) {
    int T[256];
    std::memset(T, -1, sizeof(T));
    for (int i = 0; i < 64; ++i) T[(u8)B64[i]] = i;
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

// -------------------- random --------------------
std::string nlcrypto_randomBytes(long long n) {
    std::string out((size_t)n, '\0');
#ifdef _WIN32
    BCryptGenRandom(nullptr, (PUCHAR)out.data(), (ULONG)n,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    for (auto& c : out) c = (char)(std::rand() & 0xFF);
#endif
    return out;
}
std::string nlcrypto_keygen() {
    return nlcrypto_b64encode(nlcrypto_randomBytes(32));
}
std::string nlcrypto_toHex(const std::string& s) {
    static const char H[] = "0123456789abcdef";
    std::string out;
    for (u8 c : s) { out += H[c >> 4]; out += H[c & 15]; }
    return out;
}

// -------------------- SHA-256 --------------------
namespace {
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
} // namespace

static Bytes sha256(const std::string& s) {
    Sha256 x;
    x.update((const u8*)s.data(), s.size());
    return x.finish();
}
static Bytes hmac_sha256(const std::string& key, const std::string& msg) {
    std::string k = key.size() > 64 ? std::string(
        (const char*)sha256(key).data(), 32) : key;
    k.resize(64, '\0');
    std::string o(64, '\x5c'), i(64, '\x36');
    for (int j = 0; j < 64; ++j) { o[j] ^= k[j]; i[j] ^= k[j]; }
    return sha256(o + std::string((const char*)
        sha256(i + msg).data(), 32));
}

// -------------------- HKDF-SHA256 --------------------
std::string nlcrypto_hkdfSha256(const std::string& ikm,
                                const std::string& salt,
                                const std::string& info,
                                long long len) {
    std::string s = salt.empty() ? std::string(32, '\0') : salt;
    Bytes prk = hmac_sha256(s, ikm);
    std::string out, t;
    for (u8 cnt = 1; out.size() < (size_t)len; ++cnt) {
        Bytes b = hmac_sha256(std::string((const char*)prk.data(), 32),
                              t + info + (char)cnt);
        t.assign((const char*)b.data(), 32);
        out += t;
    }
    out.resize((size_t)len);
    return nlcrypto_toHex(out);
}

// -------------------- AES-256 + GCM --------------------
namespace {
static const u8 SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static const u8 RCON[15] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,
                            0x1b,0x36,0x6c,0xd8,0xab,0x4d,0x9a};

static u8 xtime(u8 x) { return (u8)((x << 1) ^ (x & 0x80 ? 0x1b : 0)); }
static u8 mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

struct Aes256 {
    u8 rk[240]; // 15 раундов * 16 байт
    Aes256(const u8* key) {
        std::memcpy(rk, key, 32);
        u32 i = 8; // Nk=8 слов
        while (i < 60) {
            u8 t[4] = {rk[(i-1)*4],rk[(i-1)*4+1],rk[(i-1)*4+2],rk[(i-1)*4+3]};
            if (i % 8 == 0) {
                u8 x = t[0];
                t[0]=SBOX[t[1]]^RCON[i/8-1];
                t[1]=SBOX[t[2]]; t[2]=SBOX[t[3]]; t[3]=SBOX[x];
            } else if (i % 8 == 4) {
                for (auto& c : t) c = SBOX[c];
            }
            for (int j = 0; j < 4; ++j) {
                rk[i*4+j] = rk[(i-8)*4+j] ^ t[j];
            }
            ++i;
        }
    }
    void encrypt(u8* p) const {
        for (int i = 0; i < 16; ++i) p[i] ^= rk[i];
        for (int round = 1; round < 14; ++round) {
            // SubBytes + ShiftRows
            u8 t[16];
            static const int SR[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
            for (int i = 0; i < 16; ++i) t[i] = SBOX[p[SR[i]]];
            // MixColumns
            for (int c = 0; c < 4; ++c) {
                u8* m = t + c*4;
                u8 a0=m[0],a1=m[1],a2=m[2],a3=m[3];
                m[0]=mul(a0,2)^mul(a1,3)^a2^a3;
                m[1]=a0^mul(a1,2)^mul(a2,3)^a3;
                m[2]=a0^a1^mul(a2,2)^mul(a3,3);
                m[3]=mul(a0,3)^a1^a2^mul(a3,2);
            }
            for (int i = 0; i < 16; ++i) p[i] = t[i] ^ rk[round*16+i];
        }
        // последний раунд без MixColumns
        u8 t[16];
        static const int SR[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
        for (int i = 0; i < 16; ++i) t[i] = SBOX[p[SR[i]]];
        for (int i = 0; i < 16; ++i) p[i] = t[i] ^ rk[14*16+i];
    }
};

// GHASH: y ^= block; y = y*H в GF(2^128)
static void ghash_mul(u8* y, const u8* H) {
    u8 z[16] = {}; u8 v[16];
    std::memcpy(v, H, 16);
    for (int i = 0; i < 128; ++i) {
        if (y[i >> 3] & (0x80 >> (i & 7)))
            for (int j = 0; j < 16; ++j) z[j] ^= v[j];
        u8 lsb = v[15] & 1;
        for (int j = 15; j > 0; --j) v[j] = (v[j] >> 1) | (v[j-1] << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    std::memcpy(y, z, 16);
}
static void ghash_update(u8* y, const u8* H, const u8* blk) {
    for (int i = 0; i < 16; ++i) y[i] ^= blk[i];
    ghash_mul(y, H);
}
static void ctr_inc(u8* ctr) {
    for (int i = 15; i > 11; --i) {
        if (++ctr[i]) break;
    }
}
} // namespace

// GCM(iv=96bit): blob = iv || ct || tag(16)
static std::string gcm_crypt(const std::string& key,
                             const std::string& data,
                             const u8* iv,
                             u8* tag_out) {
    Aes256 aes((const u8*)key.data());
    u8 H[16] = {}; aes.encrypt(H);
    u8 J0[16] = {}; std::memcpy(J0, iv, 12); J0[15] = 1;
    u8 ctr[16]; std::memcpy(ctr, J0, 16);
    ctr_inc(ctr);
    std::string out(data.size(), '\0');
    u8 y[16] = {};
    for (size_t off = 0; off < data.size(); off += 16) {
        u8 ks[16]; std::memcpy(ks, ctr, 16);
        aes.encrypt(ks);
        size_t n = std::min<size_t>(16, data.size() - off);
        u8 blk[16] = {};
        for (size_t i = 0; i < n; ++i) {
            blk[i] = (u8)data[off+i];
            out[off+i] = data[off+i] ^ (char)ks[i];
        }
        for (size_t i = 0; i < n; ++i) blk[i] ^= ks[i];
        ghash_update(y, H, blk);
        ctr_inc(ctr);
    }
    // длины: 0 || ct_bits
    u8 lenblk[16] = {};
    uint64_t bits = data.size() * 8;
    for (int i = 0; i < 8; ++i) lenblk[8+i] = (u8)(bits >> (56 - 8*i));
    ghash_update(y, H, lenblk);
    u8 s[16]; std::memcpy(s, J0, 16); aes.encrypt(s);
    for (int i = 0; i < 16; ++i) tag_out[i] = y[i] ^ s[i];
    return out;
}

std::string nlcrypto_aes256gcmEncrypt(const std::string& keyB64,
                                      const std::string& plain) {
    std::string key = nlcrypto_b64decode(keyB64);
    if (key.size() != 32) return "";
    std::string iv = nlcrypto_randomBytes(12);
    u8 tag[16];
    std::string ct = gcm_crypt(key, plain, (const u8*)iv.data(), tag);
    return nlcrypto_b64encode(iv + ct +
                              std::string((const char*)tag, 16));
}
std::string nlcrypto_aes256gcmDecrypt(const std::string& keyB64,
                                      const std::string& blobB64) {
    std::string key = nlcrypto_b64decode(keyB64);
    std::string blob = nlcrypto_b64decode(blobB64);
    if (key.size() != 32 || blob.size() < 28) return "";
    std::string iv = blob.substr(0, 12);
    std::string ct = blob.substr(12, blob.size() - 28);
    std::string want_tag = blob.substr(blob.size() - 16);
    u8 tag[16];
    // GCM-симметричен: расшифровка = шифрование; tag — от ct
    std::string pt = gcm_crypt(key, ct, (const u8*)iv.data(), tag);
    // gcm_crypt считает ghash от шифротекста — но мы подали ct
    // и внутри посчитали ghash от xor... аккуратнее: проверяем тег
    // через повторный ghash от ct (ct уже зашифрован)
    // — исправляем: ghash должен идти по ct-байтам
    Aes256 aes((const u8*)key.data());
    u8 H[16] = {}; aes.encrypt(H);
    u8 y[16] = {};
    for (size_t off = 0; off < ct.size(); off += 16) {
        u8 blk[16] = {};
        size_t n = std::min<size_t>(16, ct.size() - off);
        std::memcpy(blk, ct.data() + off, n);
        ghash_update(y, H, blk);
    }
    u8 lenblk[16] = {};
    uint64_t bits = ct.size() * 8;
    for (int i = 0; i < 8; ++i) lenblk[8+i] = (u8)(bits >> (56 - 8*i));
    ghash_update(y, H, lenblk);
    u8 J0[16] = {}; std::memcpy(J0, iv.data(), 12); J0[15] = 1;
    u8 s[16]; std::memcpy(s, J0, 16); aes.encrypt(s);
    for (int i = 0; i < 16; ++i) tag[i] = y[i] ^ s[i];
    if (std::memcmp(tag, want_tag.data(), 16) != 0) return "";
    return pt;
}
