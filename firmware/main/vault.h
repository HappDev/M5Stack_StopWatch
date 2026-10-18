#pragma once
#include "model.h"
#include <array>
#include <esp_partition.h>
#include <nvs.h>

namespace otp {
enum class VaultStatus { Empty, Locked, Unlocked, Damaged, StorageError };
class Vault {
public:
    bool begin();
    VaultStatus status() const { return status_; }
    bool create(const char* pin);
    bool unlock(const char* pin);
    void lock();
    bool change_pin(const char* pin);
    bool save();
    bool reset();
    bool replace(const Record* records, size_t count);
    bool upsert(const Record& record);
    bool remove(const char* id);
    size_t count() const { return status_ == VaultStatus::Unlocked ? count_ : 0; }
    const Record& record(size_t i) const { return records_[i]; }
    uint32_t wait_seconds() const;
    const char* error() const { return error_; }
    static bool valid_pin(const char* pin);
private:
    #pragma pack(push, 1)
    struct Header {
        uint32_t magic, version;
        uint64_t generation;
        uint32_t count, length, iterations;
        uint8_t salt[16], wrap_nonce[12], wrapped_key[32], wrap_tag[16], data_nonce[12], data_tag[16];
        uint32_t crc;
    };
    #pragma pack(pop)
    bool inspect(int slot, Header& h, uint8_t* cipher);
    bool write_snapshot(const Header& h, const uint8_t* cipher, int slot);
    bool fail(const char* text);
    bool save_attempts(uint32_t value);
    std::array<Record, MaxRecords> records_{};
    std::array<uint8_t, MaxRecords * sizeof(Record)> cipher_{};
    uint8_t key_[32]{};
    Header header_{};
    const esp_partition_t* parts_[2]{};
    nvs_handle_t nvs_ = 0;
    size_t count_ = 0;
    int slot_ = -1;
    uint32_t attempts_ = 0;
    int64_t retry_at_ = 0;
    VaultStatus status_ = VaultStatus::StorageError;
    const char* error_ = "";
};
}
