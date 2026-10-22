#pragma once
// Модуль crypto — порт crypto.py без OpenSSL (в MinGW его нет).
// Чистые реализации SHA-256 / HMAC / HKDF-SHA256 / AES-256-GCM.
#include <string>

std::string nlcrypto_b64encode(const std::string& s);
std::string nlcrypto_b64decode(const std::string& s);
std::string nlcrypto_randomBytes(long long n);
std::string nlcrypto_keygen();                       // b64 256-bit
std::string nlcrypto_toHex(const std::string& s);
std::string nlcrypto_aes256gcmEncrypt(
    const std::string& keyB64, const std::string& plain);
std::string nlcrypto_aes256gcmDecrypt(
    const std::string& keyB64, const std::string& blobB64);
std::string nlcrypto_hkdfSha256(
    const std::string& ikm, const std::string& salt,
    const std::string& info, long long len);         // hex
