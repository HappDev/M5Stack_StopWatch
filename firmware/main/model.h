#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace otp {
constexpr size_t MaxRecords = 100;
constexpr uint32_t KdfIterations = 600000;
enum class Algorithm : uint8_t { Sha1 = 1, Sha256 = 2, Sha512 = 3 };
#pragma pack(push, 1)
struct Record {
    char id[37]{};
    char issuer[97]{};
    char account[129]{};
    uint8_t secret[128]{};
    uint16_t secret_len = 0;
    Algorithm algorithm = Algorithm::Sha1;
    uint8_t digits = 6;
    uint8_t period = 30;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 396, "Persisted v1 record layout changed");
void wipe(void* p, size_t size);
bool valid_record(const Record& record);
bool valid_utf8(const char* text);
bool base32_decode(const char* text, uint8_t* out, size_t cap, size_t& length);
std::string base32_encode(const uint8_t* bytes, size_t length);
const char* algorithm_name(Algorithm a);
bool totp(const Record& r, uint64_t timestamp, char out[9]);
bool derive_key(const char* pin, const uint8_t salt[16], uint8_t key[32]);
bool random_bytes(void* output, size_t size);
bool encrypt(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aad_len,
             const void* plain, size_t size, void* cipher, uint8_t tag[16]);
bool decrypt(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aad_len,
             const void* cipher, size_t size, const uint8_t tag[16], void* plain);
uint32_t retry_delay(uint32_t attempts);
}
