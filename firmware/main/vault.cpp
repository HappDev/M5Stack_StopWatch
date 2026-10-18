#include "vault.h"
#include <algorithm>
#include <cstddef>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <nvs_flash.h>

namespace otp {
constexpr uint32_t Magic = 0x3156544f, Commit = 0x54494d43;
constexpr char WrapAad[] = "StopWatch/DEK/v1";
bool Vault::fail(const char* text) { error_ = text; return false; }
bool Vault::valid_pin(const char* pin) {
    size_t n = strlen(pin); if (n < 8 || n > 12) return false;
    for (size_t i = 0; i < n; ++i) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}
bool Vault::begin() {
    if (nvs_flash_init() != ESP_OK || nvs_open("otp", NVS_READWRITE, &nvs_) != ESP_OK)
        return fail("Ошибка NVS. Требуется восстановление.");
    // Missing counter is valid only on a pristine device; never erase NVS automatically.
    esp_err_t ret = nvs_get_u32(nvs_, "attempts", &attempts_);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return fail("Ошибка счётчика PIN");
    for (int i = 0; i < 2; ++i) parts_[i] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY, i ? "vault_b" : "vault_a");
    if (!parts_[0] || !parts_[1]) return fail("Не найдены разделы хранилища");
    Header a{}, b{}; bool va = inspect(0, a, cipher_.data()), vb = inspect(1, b, cipher_.data());
    wipe(cipher_.data(), cipher_.size());
    if (va || vb) {
        slot_ = vb && (!va || b.generation > a.generation) ? 1 : 0;
        header_ = slot_ ? b : a; status_ = VaultStatus::Locked;
        // Reserve a counter on first initialization, fail closed on missing counters thereafter.
        if (ret == ESP_ERR_NVS_NOT_FOUND) { status_ = VaultStatus::StorageError; return fail("Повреждён счётчик PIN"); }
    } else {
        bool blank = true; uint8_t buf[256];
        for (int s = 0; s < 2 && blank; ++s) for (size_t off = 0; off < parts_[s]->size; off += sizeof buf) {
            if (esp_partition_read(parts_[s], off, buf, sizeof buf) != ESP_OK) { blank = false; break; }
            for (auto byte : buf) if (byte != 255) { blank = false; break; }
            if (!blank) break;
        }
        status_ = blank ? VaultStatus::Empty : VaultStatus::Damaged;
    }
    retry_at_ = esp_timer_get_time() + static_cast<int64_t>(retry_delay(attempts_)) * 1000000;
    return true;
}
bool Vault::inspect(int slot, Header& h, uint8_t* cipher) {
    uint32_t marker = 0;
    if (esp_partition_read(parts_[slot], parts_[slot]->size - 4, &marker, 4) != ESP_OK || marker != Commit ||
        esp_partition_read(parts_[slot], 0, &h, sizeof h) != ESP_OK || h.magic != Magic || h.version != 1 ||
        !h.generation || h.count > MaxRecords || h.length != h.count * sizeof(Record) ||
        h.iterations != KdfIterations || esp_partition_read(parts_[slot], sizeof h, cipher, h.length) != ESP_OK) return false;
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(Header, crc));
    crc = esp_rom_crc32_le(crc, cipher, h.length); return crc == h.crc;
}
uint32_t Vault::wait_seconds() const {
    return static_cast<uint32_t>(std::max<int64_t>(0, (retry_at_ - esp_timer_get_time() + 999999) / 1000000));
}
bool Vault::save_attempts(uint32_t value) {
    if (nvs_set_u32(nvs_, "attempts", value) != ESP_OK || nvs_commit(nvs_) != ESP_OK) {
        status_ = VaultStatus::StorageError; lock(); return fail("Ошибка записи счётчика PIN");
    }
    attempts_ = value; return true;
}
bool Vault::create(const char* pin) {
    if (status_ != VaultStatus::Empty || !valid_pin(pin)) return fail("Нужен PIN из 8–12 цифр");
    header_ = {}; header_.magic = Magic; header_.version = 1; header_.iterations = KdfIterations;
    uint8_t kek[32]{}; random_bytes(key_, 32); random_bytes(header_.salt, 16); random_bytes(header_.wrap_nonce, 12);
    bool ok = derive_key(pin, header_.salt, kek) && encrypt(kek, header_.wrap_nonce, WrapAad, sizeof WrapAad,
        key_, 32, header_.wrapped_key, header_.wrap_tag);
    wipe(kek, sizeof kek);
    if (!ok || !save_attempts(0)) { lock(); return fail("Не удалось создать ключ"); }
    status_ = VaultStatus::Unlocked; count_ = 0;
    if (!save()) { status_ = VaultStatus::Damaged; lock(); return false; } return true;
}
bool Vault::unlock(const char* pin) {
    if (status_ != VaultStatus::Locked || wait_seconds() || !valid_pin(pin)) return fail("PIN недоступен или неверен");
    // Persist the attempt BEFORE the slow KDF: cutting power cannot bypass the counter.
    if (!save_attempts(std::min<uint32_t>(attempts_ + 1, 1000))) return false;
    retry_at_ = esp_timer_get_time() + static_cast<int64_t>(retry_delay(attempts_)) * 1000000;
    uint8_t kek[32]{};
    bool ok = derive_key(pin, header_.salt, kek) && decrypt(kek, header_.wrap_nonce, WrapAad, sizeof WrapAad,
        header_.wrapped_key, 32, header_.wrap_tag, key_);
    wipe(kek, sizeof kek);
    if (!ok) {
        wipe(key_, 32);
        retry_at_=esp_timer_get_time()+static_cast<int64_t>(retry_delay(attempts_))*1000000;
        return fail("Неверный PIN");
    }
    Header h{};
    ok = inspect(slot_, h, cipher_.data()) && decrypt(key_, h.data_nonce, &h, offsetof(Header, data_tag),
        cipher_.data(), h.length, h.data_tag, records_.data());
    wipe(cipher_.data(), cipher_.size());
    if (ok) for (size_t i = 0; i < h.count; ++i) {
        if (!valid_record(records_[i])) { ok = false; break; }
        for (size_t j = 0; j < i; ++j) if (!strcmp(records_[i].id, records_[j].id)) ok = false;
    }
    if (!ok) { status_ = VaultStatus::Damaged; lock(); return fail("Хранилище повреждено. Восстановите копию."); }
    count_ = h.count; header_ = h;
    if (!save_attempts(0)) return false;
    retry_at_ = 0; status_ = VaultStatus::Unlocked; error_ = ""; return true;
}
void Vault::lock() {
    wipe(records_.data(), sizeof records_); wipe(key_, sizeof key_); wipe(cipher_.data(), sizeof cipher_); count_ = 0;
    if (status_ == VaultStatus::Unlocked) status_ = VaultStatus::Locked;
}
bool Vault::write_snapshot(const Header& h, const uint8_t* cipher, int slot) {
    auto p = parts_[slot];
    if (esp_partition_erase_range(p, 0, p->size) != ESP_OK || esp_partition_write(p, 0, &h, sizeof h) != ESP_OK ||
        esp_partition_write(p, sizeof h, cipher, h.length) != ESP_OK) return false;
    // Read back ciphertext and header before committing the slot.
    Header check{}; uint8_t block[256]; uint32_t crc;
    if (esp_partition_read(p, 0, &check, sizeof check) != ESP_OK || memcmp(&h, &check, sizeof h)) return false;
    crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&check), offsetof(Header, crc));
    for (size_t off = 0; off < h.length; off += sizeof block) {
        size_t n = std::min<size_t>(sizeof block, h.length - off);
        if (esp_partition_read(p, sizeof h + off, block, n) != ESP_OK) return false;
        crc = esp_rom_crc32_le(crc, block, n);
    }
    if (crc != h.crc) return false;
    uint32_t marker = Commit, actual = 0;
    return esp_partition_write(p, p->size - 4, &marker, 4) == ESP_OK &&
        esp_partition_read(p, p->size - 4, &actual, 4) == ESP_OK && actual == Commit;
}
bool Vault::save() {
    if (status_ != VaultStatus::Unlocked) return fail("Хранилище заблокировано");
    Header next = header_; ++next.generation; next.count = count_; next.length = count_ * sizeof(Record);
    random_bytes(next.data_nonce, 12);
    bool ok = encrypt(key_, next.data_nonce, &next, offsetof(Header, data_tag), records_.data(),
                      next.length, cipher_.data(), next.data_tag);
    next.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&next), offsetof(Header, crc));
    next.crc = esp_rom_crc32_le(next.crc, cipher_.data(), next.length);
    int target = slot_ == 0 ? 1 : 0;
    if (ok) ok = write_snapshot(next, cipher_.data(), target);
    wipe(cipher_.data(), cipher_.size());
    if (!ok) { lock(); return fail("Запись не подтверждена. Разблокируйте заново."); }
    header_ = next; slot_ = target; return true;
}
bool Vault::change_pin(const char* pin) {
    if (status_ != VaultStatus::Unlocked || !valid_pin(pin)) return fail("Нужен PIN из 8–12 цифр");
    Header next=header_;
    uint8_t kek[32]{}; random_bytes(next.salt, 16); random_bytes(next.wrap_nonce, 12);
    bool ok = derive_key(pin, next.salt, kek) && encrypt(kek, next.wrap_nonce, WrapAad, sizeof WrapAad,
        key_, 32, next.wrapped_key, next.wrap_tag); wipe(kek, 32);
    if(!ok) return fail("Не удалось изменить PIN");
    header_=next;
    // Overwrite both snapshots so an ordinary rollback cannot retain the previous PIN wrapper.
    if(save() && save()) return true;
    // A partial PIN change can leave either PIN committed. Re-read the real flash,
    // rather than trying to unlock with an uncommitted in-memory key wrapper.
    Header a{},b{};bool va=inspect(0,a,cipher_.data()),vb=inspect(1,b,cipher_.data());
    wipe(cipher_.data(),cipher_.size());
    if(va||vb) {slot_=vb&&(!va||b.generation>a.generation)?1:0;header_=slot_?b:a;}
    else status_=VaultStatus::Damaged;
    return false;
}
bool Vault::replace(const Record* records, size_t n) {
    if (status_ != VaultStatus::Unlocked || n > MaxRecords) return fail("Недопустимый набор аккаунтов");
    for (size_t i = 0; i < n; ++i) {
        if (!valid_record(records[i])) return fail("Неверная запись");
        for (size_t j = 0; j < i; ++j) if (!strcmp(records[i].id, records[j].id)) return fail("Повтор идентификатора");
    }
    wipe(records_.data(), sizeof records_); memcpy(records_.data(), records, n * sizeof(Record)); count_ = n;
    return save();
}
bool Vault::upsert(const Record& r) {
    if (status_ != VaultStatus::Unlocked || !valid_record(r)) return fail("Неверная запись");
    size_t i = 0; for (; i < count_; ++i) if (!strcmp(records_[i].id, r.id)) break;
    if (i == count_) { if (count_ == MaxRecords) return fail("Лимит: 100 аккаунтов"); ++count_; }
    records_[i] = r; return save();
}
bool Vault::remove(const char* id) {
    if (status_ != VaultStatus::Unlocked) return fail("Хранилище заблокировано");
    for (size_t i = 0; i < count_; ++i) if (!strcmp(records_[i].id, id)) {
        for (size_t j = i + 1; j < count_; ++j) records_[j - 1] = records_[j];
        wipe(&records_[--count_], sizeof(Record)); return save();
    }
    return fail("Аккаунт не найден");
}
bool Vault::reset() {
    lock();
    for (auto p : parts_) if (!p || esp_partition_erase_range(p, 0, p->size) != ESP_OK) {
        status_ = VaultStatus::StorageError; return fail("Сброс не завершён");
    }
    if (!save_attempts(0)) return false;
    header_ = {}; slot_ = -1; count_ = 0; retry_at_ = 0; status_ = VaultStatus::Empty; error_ = ""; return true;
}
}
