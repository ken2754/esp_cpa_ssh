#pragma once

#include "app_types.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string>

namespace esp_cpa_ssh {

class SshClient : public ISshClient {
public:
    explicit SshClient(IConfigStore* config_store = nullptr);
    ~SshClient() override;

    void setConfigStore(IConfigStore* config_store);

    void setDataCallback(TerminalDataCallback cb) override;
    void setStatusCallback(SshStatusCallback cb) override;
    void setHostVerifyCallback(HostVerifyCallback cb) override;

    bool connectAsync(const SshProfile& profile, const char* private_key_pem) override;
    void disconnect() override;

    bool isConnected() const override;
    SshState state() const override;

    bool sendBytes(const uint8_t* data, size_t len) override;
    bool sendWindowSize(uint16_t cols, uint16_t rows) override;

private:
    static void taskEntry(void* arg);
    void runSession();
    void setState(SshState new_state, const char* msg);
    bool verifyHostKey(void* session, const char* host, uint16_t port);
    bool authenticate(void* session);
    size_t popTxBytes(uint8_t* out, size_t max_len);

    static constexpr size_t TX_RING_SIZE = 2048;
    static constexpr size_t RX_BUF_SIZE = 1024;
    static constexpr uint32_t SSH_TASK_STACK_BYTES = 16384;
    static constexpr UBaseType_t SSH_TASK_PRIORITY = 2;

    IConfigStore* config_store_ = nullptr;

    TerminalDataCallback data_cb_;
    SshStatusCallback status_cb_;
    HostVerifyCallback host_verify_cb_;

    SshProfile profile_;
    std::string private_key_pem_;

    std::atomic<SshState> state_{SshState::DISCONNECTED};
    std::atomic<bool> stop_requested_{false};

    TaskHandle_t task_handle_ = nullptr;
    SemaphoreHandle_t io_mutex_ = nullptr;
    SemaphoreHandle_t done_sem_ = nullptr;

    uint8_t tx_ring_[TX_RING_SIZE] = {0};
    size_t tx_head_ = 0;
    size_t tx_tail_ = 0;
    size_t tx_count_ = 0;

    bool resize_pending_ = false;
    uint16_t pending_cols_ = TERM_COLS;
    uint16_t pending_rows_ = TERM_ROWS;
};

}  // namespace esp_cpa_ssh
