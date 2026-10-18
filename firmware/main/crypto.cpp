#include "model.h"
#include <algorithm>
#include <cstdio>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <bootloader_random.h>
#include <esp_random.h>
#include <esp_bt.h>

namespace otp {
void wipe(void* p, size_t n) { mbedtls_platform_zeroize(p, n); }
const char* algorithm_name(Algorithm a) {
    switch (a) { case Algorithm::Sha1: return "SHA1"; case Algorithm::Sha256: return "SHA256";
        case Algorithm::Sha512: return "SHA512"; default: return ""; }
}
bool valid_record(const Record& r) {
    if (!memchr(r.id, 0, sizeof r.id) || strlen(r.id) != 36 ||
        !memchr(r.issuer, 0, sizeof r.issuer) || !memchr(r.account, 0, sizeof r.account) ||
        (!r.issuer[0] && !r.account[0]) || r.secret_len < 10 || r.secret_len > sizeof r.secret ||
        !*algorithm_name(r.algorithm) || (r.digits != 6 && r.digits != 8) ||
        (r.period != 30 && r.period != 60)) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (r.id[i] != '-') return false; }
        else if (!((r.id[i] >= '0' && r.id[i] <= '9') || (r.id[i] >= 'a' && r.id[i] <= 'f'))) return false;
    }
    for (auto s : {r.issuer, r.account}) {
        if (!valid_utf8(s)) return false;
        for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
            if (*p < 32 || *p == 127) return false;
    }
    return true;
}
bool valid_utf8(const char* text) {
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    while (*p) {
        uint32_t cp=*p++; unsigned extra=0;
        if(cp<128) continue;
        if(cp>=0xc2 && cp<=0xdf) { extra=1;cp&=31; }
        else if(cp>=0xe0 && cp<=0xef) { extra=2;cp&=15; }
        else if(cp>=0xf0 && cp<=0xf4) { extra=3;cp&=7; }
        else return false;
        unsigned n=extra;
        while(n--) { if((*p&0xc0)!=0x80) return false;cp=(cp<<6)|(*p++&63); }
        if((extra==1 && cp<0x80)||(extra==2 && cp<0x800)||(extra==3 && cp<0x10000)||
           cp>0x10ffff||(cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
bool base32_decode(const char* text, uint8_t* out, size_t cap, size_t& length) {
    length = 0; uint32_t buffer = 0; unsigned bits = 0; bool padding = false;
    size_t symbols = 0, pads = 0;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '=') { padding = true; ++pads; continue; }
        if (padding) return false;
        if (c >= 'a' && c <= 'z') c -= 32;
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= '2' && c <= '7' ? c - '2' + 26 : -1;
        if (v < 0) return false;
        ++symbols; buffer = (buffer << 5) | v; bits += 5;
        if (bits >= 8) { bits -= 8; if (length >= cap) return false; out[length++] = (buffer >> bits) & 255; }
    }
    if (symbols % 8 == 1 || symbols % 8 == 3 || symbols % 8 == 6 ||
        (bits && (buffer & ((1u << bits) - 1))) ||
        (pads && ((symbols + pads) % 8 || pads != (8 - symbols % 8) % 8))) return false;
    return length != 0;
}
std::string base32_encode(const uint8_t* data, size_t n) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string s; s.reserve((n * 8 + 4) / 5); uint32_t buffer = 0; unsigned bits = 0;
    for (size_t i = 0; i < n; ++i) {
        buffer = (buffer << 8) | data[i]; bits += 8;
        while (bits >= 5) { bits -= 5; s += alphabet[(buffer >> bits) & 31]; }
    }
    if (bits) s += alphabet[(buffer << (5 - bits)) & 31];
    return s;
}
bool totp(const Record& r, uint64_t timestamp, char out[9]) {
    if (!valid_record(r)) return false;
    auto type = r.algorithm == Algorithm::Sha1 ? MBEDTLS_MD_SHA1 :
                r.algorithm == Algorithm::Sha256 ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA512;
    uint64_t counter = timestamp / r.period; uint8_t message[8], hash[64]{};
    for (int i = 7; i >= 0; --i) { message[i] = counter & 255; counter >>= 8; }
    int ret = mbedtls_md_hmac(mbedtls_md_info_from_type(type), r.secret, r.secret_len, message, 8, hash);
    if (ret) { wipe(hash, sizeof hash); return false; }
    auto length = mbedtls_md_get_size(mbedtls_md_info_from_type(type));
    unsigned offset = hash[length - 1] & 15;
    uint32_t binary = ((hash[offset] & 127u) << 24) | (hash[offset + 1] << 16) |
                      (hash[offset + 2] << 8) | hash[offset + 3];
    const unsigned width=r.digits==8?8:6;
    out[width]=0;
    for(int i=static_cast<int>(width)-1;i>=0;--i) {out[i]='0'+binary%10;binary/=10;}
    wipe(hash, sizeof hash); wipe(message, sizeof message); return true;
}
bool derive_key(const char* credential, const uint8_t salt[16], uint8_t key[32], const KdfService& service) {
    // PBKDF2-HMAC-SHA256, one 32-byte output block. Cache the SHA states after
    // the HMAC ipad/opad blocks instead of hashing those same blocks 600000
    // times. The iteration count and derived key are identical to PKCS5.
    // These credential-dependent states live only for this invocation.
    static_assert(KdfIterations > 0, "PBKDF2 requires at least one iteration");
    mbedtls_sha256_context inner, outer, work;
    mbedtls_sha256_init(&inner); mbedtls_sha256_init(&outer); mbedtls_sha256_init(&work);
    uint8_t pad[64]{}, u[32]{};
    const size_t length = strlen(credential);
    const auto* password = reinterpret_cast<const uint8_t*>(credential);
    int ret = 0;
    if (length > sizeof pad) ret = mbedtls_sha256(password, length, pad, 0);
    else memcpy(pad, password, length);
    for (auto& byte : pad) byte ^= 0x36;
    if (!ret) ret = mbedtls_sha256_starts(&inner, 0);
    if (!ret) ret = mbedtls_sha256_update(&inner, pad, sizeof pad);
    for (auto& byte : pad) byte ^= 0x36 ^ 0x5c;
    if (!ret) ret = mbedtls_sha256_starts(&outer, 0);
    if (!ret) ret = mbedtls_sha256_update(&outer, pad, sizeof pad);
    wipe(pad, sizeof pad);

    // U1 = HMAC(password, salt || INT32_BE(1)); Ui = HMAC(password, Ui-1).
    memcpy(u, salt, 16); u[19] = 1;
    memset(key, 0, 32);
    for (uint32_t i = 0; !ret && i < KdfIterations; ++i) {
        mbedtls_sha256_clone(&work, &inner);
        ret = mbedtls_sha256_update(&work, u, i == 0 ? 20 : sizeof u);
        if (!ret) ret = mbedtls_sha256_finish(&work, u);
        if (!ret) {
            mbedtls_sha256_clone(&work, &outer);
            ret = mbedtls_sha256_update(&work, u, sizeof u);
        }
        if (!ret) ret = mbedtls_sha256_finish(&work, u);
        if (!ret) for (size_t j = 0; j < sizeof u; ++j) key[j] ^= u[j];
        if (service && (i & 2047u) == 2047u) service();
    }
    mbedtls_sha256_free(&work); mbedtls_sha256_free(&outer); mbedtls_sha256_free(&inner);
    wipe(u, sizeof u);
    if (ret) wipe(key, 32);
    return ret == 0;
}
bool random_bytes(void* out, size_t n) {
    // The Bluetooth controller supplies RF entropy when enabled. Enabling the
    // bootloader SAR entropy source at that time would conflict with RF use.
    if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_fill_random(out,n);return true;
    }
    bootloader_random_enable(); esp_fill_random(out, n); bootloader_random_disable(); return true;
}
bool encrypt(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aad_len,
             const void* plain, size_t n, void* cipher, uint8_t tag[16]) {
    mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!ret) ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, n, nonce, 12,
        static_cast<const uint8_t*>(aad), aad_len, static_cast<const uint8_t*>(plain),
        static_cast<uint8_t*>(cipher), 16, tag);
    mbedtls_gcm_free(&ctx); return ret == 0;
}
bool decrypt(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aad_len,
             const void* cipher, size_t n, const uint8_t tag[16], void* plain) {
    mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!ret) ret = mbedtls_gcm_auth_decrypt(&ctx, n, nonce, 12, static_cast<const uint8_t*>(aad),
        aad_len, tag, 16, static_cast<const uint8_t*>(cipher), static_cast<uint8_t*>(plain));
    mbedtls_gcm_free(&ctx); if (ret) wipe(plain, n); return ret == 0;
}
uint32_t retry_delay(uint32_t attempts) {
    if (attempts < 5) return 0;
    return attempts >= 12 ? 3600 : std::min<uint32_t>(3600, 30u << (attempts - 5));
}
}
