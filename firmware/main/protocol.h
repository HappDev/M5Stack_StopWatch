#pragma once
#include "ui.h"
#include <array>
#include <cJSON.h>
namespace otp {
class Protocol {
public:
    Protocol(Vault& vault, Ui& ui, bool& authorized) : vault_(vault), ui_(ui), authorized_(authorized) {}
    bool begin();
    void tick();
    void disconnected();
private:
    void receive(char* line);
    void reply(int id, cJSON* result);
    void error(int id, const char* code, const char* message);
    bool send(const char* text);
    void clear_transfers();
    void status(int id);
    Vault& vault_; Ui& ui_; bool& authorized_;
    std::array<Record, MaxRecords> staging_{};
    size_t staged_ = 0, expected_ = 0, export_index_ = 0;
    bool restoring_ = false, exporting_ = false, was_connected_ = false, pending_ = false;
    char line_[4096]{}; size_t line_size_ = 0; bool overflow_ = false;
    int64_t line_started_ = 0, transfer_activity_ = 0;
};
}
