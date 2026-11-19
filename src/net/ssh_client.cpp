#include "ssh_client.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <libssh/libssh.h>
#include <libssh_esp32.h>
#include <vector>

namespace esp_cpa_ssh {

namespace {

void ensureLibsshInitialized() {
    static std::atomic<int> init_state{0};  // 0: uninit, 1: initializing, 2: done
    int expected = 0;
    if (init_state.compare_exchange_strong(expected, 1)) {
        libssh_begin();
        init_state.store(2);
    } else {
        while (init_state.load() != 2) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

std::string normalizeLineEndings(const char* input) {
    if (!input) {
        return {};
    }
    std::string out;
    out.reserve(strlen(input) + 2);
    for (const char* p = input; *p != '\0'; ++p) {
        if (*p == '\r') {
            if (*(p + 1) == '\n') {
                ++p;
            }
            out.push_back('\n');
        } else {
            out.push_back(*p);
        }
    }
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    return out;
}

void secureClearString(std::string& s) {
    if (!s.empty()) {
        std::fill(s.begin(), s.end(), '\0');
        s.clear();
    }
}

}  // namespace

SshClient::SshClient(IConfigStore* config_store) : config_store_(config_store) {
    io_mutex_ = xSemaphoreCreateMutex();
    done_sem_ = xSemaphoreCreateBinary();
}

SshClient::~SshClient() {
    disconnect();
    if (io_mutex_) {
        vSemaphoreDelete(io_mutex_);
        io_mutex_ = nullptr;
    }
    if (done_sem_) {
        vSemaphoreDelete(done_sem_);
        done_sem_ = nullptr;
    }
}

void SshClient::setConfigStore(IConfigStore* config_store) {
    config_store_ = config_store;
}

void SshClient::setDataCallback(TerminalDataCallback cb) {
    data_cb_ = std::move(cb);
}

void SshClient::setStatusCallback(SshStatusCallback cb) {
    status_cb_ = std::move(cb);
}

void SshClient::setHostVerifyCallback(HostVerifyCallback cb) {
    host_verify_cb_ = std::move(cb);
}

void SshClient::setState(SshState new_state, const char* msg) {
    state_.store(new_state);
    if (status_cb_) {
        status_cb_(new_state, msg ? msg : "");
    }
}

bool SshClient::connectAsync(const SshProfile& profile, const char* private_key_pem) {
    if (profile.host[0] == '\0' || profile.username[0] == '\0') {
        setState(SshState::ERROR, "Invalid SSH profile");
        return false;
    }

    disconnect();

    ensureLibsshInitialized();

    profile_ = profile;
    if (profile_.port == 0) {
        profile_.port = 22;
    }

    secureClearString(private_key_pem_);
    if (private_key_pem && private_key_pem[0] != '\0') {
        private_key_pem_ = normalizeLineEndings(private_key_pem);
    }

    if (io_mutex_) {
        xSemaphoreTake(io_mutex_, portMAX_DELAY);
        tx_head_ = 0;
        tx_tail_ = 0;
        tx_count_ = 0;
        resize_pending_ = false;
        xSemaphoreGive(io_mutex_);
    }

    if (done_sem_) {
        xSemaphoreTake(done_sem_, 0);  // Ensure semaphore is empty before starting
    }

    stop_requested_.store(false);
    setState(SshState::CONNECTING, "Connecting...");

    BaseType_t res = xTaskCreate(
        &SshClient::taskEntry,
        "ssh_io",
        SSH_TASK_STACK_BYTES,
        this,
        SSH_TASK_PRIORITY,
        &task_handle_);

    if (res != pdPASS) {
        task_handle_ = nullptr;
        secureClearString(private_key_pem_);
        setState(SshState::ERROR, "Failed to start SSH task");
        return false;
    }

    return true;
}

void SshClient::disconnect() {
    stop_requested_.store(true);

    if (task_handle_ != nullptr) {
        if (done_sem_) {
            xSemaphoreTake(done_sem_, pdMS_TO_TICKS(5000));
        } else {
            uint32_t waited = 0;
            while (task_handle_ != nullptr && waited < 5000) {
                vTaskDelay(pdMS_TO_TICKS(10));
                waited += 10;
            }
        }
        task_handle_ = nullptr;
    }

    if (io_mutex_) {
        xSemaphoreTake(io_mutex_, portMAX_DELAY);
        tx_head_ = 0;
        tx_tail_ = 0;
        tx_count_ = 0;
        resize_pending_ = false;
        xSemaphoreGive(io_mutex_);
    }

    secureClearString(private_key_pem_);

    if (state_.load() != SshState::DISCONNECTED && state_.load() != SshState::ERROR) {
        setState(SshState::DISCONNECTED, "Disconnected");
    }
}

bool SshClient::isConnected() const {
    return state_.load() == SshState::CONNECTED;
}

SshState SshClient::state() const {
    return state_.load();
}

bool SshClient::sendBytes(const uint8_t* data, size_t len) {
    if (!data || len == 0 || !isConnected() || !io_mutex_) {
        return false;
    }

    xSemaphoreTake(io_mutex_, portMAX_DELAY);
    const size_t free_space = TX_RING_SIZE - tx_count_;
    const size_t to_write = (len <= free_space) ? len : free_space;
    for (size_t i = 0; i < to_write; ++i) {
        tx_ring_[tx_head_] = data[i];
        tx_head_ = (tx_head_ + 1) % TX_RING_SIZE;
    }
    tx_count_ += to_write;
    xSemaphoreGive(io_mutex_);

    return to_write == len;
}

bool SshClient::sendWindowSize(uint16_t cols, uint16_t rows) {
    if (!isConnected() || !io_mutex_) {
        return false;
    }
    xSemaphoreTake(io_mutex_, portMAX_DELAY);
    pending_cols_ = cols;
    pending_rows_ = rows;
    resize_pending_ = true;
    xSemaphoreGive(io_mutex_);
    return true;
}

size_t SshClient::popTxBytes(uint8_t* out, size_t max_len) {
    if (!out || max_len == 0 || !io_mutex_) {
        return 0;
    }
    xSemaphoreTake(io_mutex_, portMAX_DELAY);
    const size_t to_read = (tx_count_ < max_len) ? tx_count_ : max_len;
    for (size_t i = 0; i < to_read; ++i) {
        out[i] = tx_ring_[tx_tail_];
        tx_tail_ = (tx_tail_ + 1) % TX_RING_SIZE;
    }
    tx_count_ -= to_read;
    xSemaphoreGive(io_mutex_);
    return to_read;
}

void SshClient::taskEntry(void* arg) {
    auto* self = static_cast<SshClient*>(arg);
    self->runSession();
    self->task_handle_ = nullptr;
    if (self->done_sem_) {
        xSemaphoreGive(self->done_sem_);
    }
    vTaskDelete(nullptr);
}

bool SshClient::verifyHostKey(void* raw_session, const char* host, uint16_t port) {
    ssh_session session = static_cast<ssh_session>(raw_session);

    ssh_key srv_pubkey = nullptr;
    if (ssh_get_server_publickey(session, &srv_pubkey) != SSH_OK || srv_pubkey == nullptr) {
        setState(SshState::ERROR, "Failed to get server public key");
        return false;
    }

    const char* key_type = ssh_key_type_to_char(ssh_key_type(srv_pubkey));
    if (!key_type) {
        key_type = "unknown";
    }

    unsigned char* hash = nullptr;
    size_t hlen = 0;
    if (ssh_get_publickey_hash(srv_pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != SSH_OK ||
        hash == nullptr) {
        ssh_key_free(srv_pubkey);
        setState(SshState::ERROR, "Failed to hash server key");
        return false;
    }

    char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(srv_pubkey);

    if (!fp) {
        setState(SshState::ERROR, "Failed to format host fingerprint");
        return false;
    }

    bool known = false;
    bool mismatch = false;
    if (config_store_) {
        known = config_store_->checkKnownHost(host, port, key_type, fp, mismatch);
    }

    bool accepted = false;
    if (known && !mismatch) {
        accepted = true;
    } else {
        setState(SshState::HOST_VERIFY, mismatch ? "Host key mismatch!" : "Verify host key");
        if (host_verify_cb_) {
            accepted = host_verify_cb_(host, port, key_type, fp, mismatch);
        }
        if (accepted && config_store_) {
            config_store_->saveKnownHost(host, port, key_type, fp);
        }
    }

    ssh_string_free_char(fp);

    if (!accepted) {
        setState(SshState::ERROR, mismatch ? "Host key mismatch rejected" : "Host key rejected");
        return false;
    }
    return true;
}

bool SshClient::authenticate(void* raw_session) {
    ssh_session session = static_cast<ssh_session>(raw_session);
    setState(SshState::AUTHENTICATING, "Authenticating...");

    int none_rc = ssh_userauth_none(session, nullptr);
    int methods = ssh_userauth_list(session, nullptr);
    Serial.printf("[SSH] user=%s none_rc=%d methods=0x%x auth_type=%d\n",
                  profile_.username, none_rc, methods, static_cast<int>(profile_.auth_type));

    if (profile_.auth_type == AUTH_ED25519) {
        if (private_key_pem_.empty() && config_store_) {
            const char* path = (profile_.key_path[0] != '\0') ? profile_.key_path : DEFAULT_KEY_PATH;
            std::vector<char> key_buf(4096, 0);
            size_t key_len = 0;
            if (config_store_->loadPrivateKey(path, key_buf.data(), key_buf.size() - 1, key_len) &&
                key_len > 0) {
                key_buf[key_len] = '\0';
                private_key_pem_ = normalizeLineEndings(key_buf.data());
            }
            std::fill(key_buf.begin(), key_buf.end(), '\0');
        }

        if (private_key_pem_.empty()) {
            Serial.println("[SSH] Private key not found");
            setState(SshState::ERROR, "Private key not found");
            return false;
        }

        const char* passphrase = (profile_.secret[0] != '\0') ? profile_.secret : nullptr;
        ssh_key privkey = nullptr;
        int rc = ssh_pki_import_privkey_base64(
            private_key_pem_.c_str(), passphrase, nullptr, nullptr, &privkey);
        secureClearString(private_key_pem_);

        if (rc != SSH_OK || privkey == nullptr) {
            Serial.printf("[SSH] ssh_pki_import_privkey_base64 failed rc=%d\n", rc);
            if (privkey) {
                ssh_key_free(privkey);
            }
            setState(SshState::ERROR, "Invalid private key or passphrase");
            return false;
        }

        // Log public key type and SHA256 fingerprint (safe, non-secret)
        unsigned char* pub_hash = nullptr;
        size_t pub_hlen = 0;
        if (ssh_get_publickey_hash(privkey, SSH_PUBLICKEY_HASH_SHA256, &pub_hash, &pub_hlen) == SSH_OK &&
            pub_hash != nullptr) {
            char* pub_fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, pub_hash, pub_hlen);
            Serial.printf("[SSH] Client key type=%s fp=%s\n",
                          ssh_key_type_to_char(ssh_key_type(privkey)),
                          pub_fp ? pub_fp : "?");
            if (pub_fp) ssh_string_free_char(pub_fp);
            ssh_clean_pubkey_hash(&pub_hash);
        }

        rc = ssh_userauth_publickey(session, nullptr, privkey);
        ssh_key_free(privkey);

        Serial.printf("[SSH] ssh_userauth_publickey rc=%d err=%s\n", rc, ssh_get_error(session));
        if (rc != SSH_AUTH_SUCCESS) {
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf), "Key auth failed: %s", ssh_get_error(session));
            setState(SshState::ERROR, err_buf);
            return false;
        }
        return true;
    }

    // AUTH_PASSWORD
    int rc = ssh_userauth_password(session, nullptr, profile_.secret);
    Serial.printf("[SSH] ssh_userauth_password rc=%d err=%s\n", rc, ssh_get_error(session));
    if (rc != SSH_AUTH_SUCCESS) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Password auth failed: %s", ssh_get_error(session));
        setState(SshState::ERROR, err_buf);
        return false;
    }
    return true;
}

void SshClient::runSession() {
    ssh_session session = ssh_new();
    if (!session) {
        setState(SshState::ERROR, "ssh_new failed");
        return;
    }

    ssh_channel channel = nullptr;
    bool clean_disconnect = false;

    do {
        int port = static_cast<int>(profile_.port ? profile_.port : 22);
        long timeout_s = 15;
        int nodelay = 1;
        bool process_cfg = false;
        int verbosity = SSH_LOG_PROTOCOL;

        Serial.printf("[SSH] Connecting to %s:%d as %s\n", profile_.host, port, profile_.username);

        bool opt_ok = true;
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_HOST, profile_.host) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_PORT, &port) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_USER, profile_.username) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout_s) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_NODELAY, &nodelay) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &process_cfg) == SSH_OK);
        opt_ok &= (ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity) == SSH_OK);
        opt_ok &= (ssh_options_set(
                       session,
                       SSH_OPTIONS_KEY_EXCHANGE,
                       "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,diffie-hellman-group14-sha256") == SSH_OK);
        opt_ok &= (ssh_options_set(
                       session,
                       SSH_OPTIONS_HOSTKEYS,
                       "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa") == SSH_OK);
        opt_ok &= (ssh_options_set(
                       session,
                       SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES,
                       "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256") == SSH_OK);

        if (!opt_ok) {
            Serial.printf("[SSH] ssh_options_set failed: %s\n", ssh_get_error(session));
            setState(SshState::ERROR, "SSH option error");
            break;
        }

        if (stop_requested_.load()) {
            clean_disconnect = true;
            break;
        }

        if (ssh_connect(session) != SSH_OK) {
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf), "Connect failed: %s", ssh_get_error(session));
            setState(SshState::ERROR, err_buf);
            break;
        }

        if (stop_requested_.load()) {
            clean_disconnect = true;
            break;
        }

        if (!verifyHostKey(session, profile_.host, static_cast<uint16_t>(port))) {
            break;
        }

        if (stop_requested_.load()) {
            clean_disconnect = true;
            break;
        }

        if (!authenticate(session)) {
            break;
        }

        if (stop_requested_.load()) {
            clean_disconnect = true;
            break;
        }

        channel = ssh_channel_new(session);
        if (!channel) {
            setState(SshState::ERROR, "ssh_channel_new failed");
            break;
        }

        if (ssh_channel_open_session(channel) != SSH_OK) {
            setState(SshState::ERROR, "Failed to open SSH session channel");
            break;
        }

        if (ssh_channel_request_pty_size(channel, "xterm", TERM_COLS, TERM_ROWS) != SSH_OK) {
            setState(SshState::ERROR, "Failed to allocate PTY");
            break;
        }

        if (ssh_channel_request_shell(channel) != SSH_OK) {
            setState(SshState::ERROR, "Failed to start shell");
            break;
        }

        setState(SshState::CONNECTED, "Connected");

        std::vector<uint8_t> rx_buf(RX_BUF_SIZE);
        uint8_t tx_chunk[256];

        while (!stop_requested_.load()) {
            bool did_work = false;

            // Handle pending PTY resize
            bool do_resize = false;
            uint16_t cols = TERM_COLS;
            uint16_t rows = TERM_ROWS;
            if (io_mutex_) {
                xSemaphoreTake(io_mutex_, portMAX_DELAY);
                if (resize_pending_) {
                    do_resize = true;
                    cols = pending_cols_;
                    rows = pending_rows_;
                    resize_pending_ = false;
                }
                xSemaphoreGive(io_mutex_);
            }
            if (do_resize) {
                ssh_channel_change_pty_size(channel, cols, rows);
                did_work = true;
            }

            // Drain TX ring buffer
            size_t tx_len = popTxBytes(tx_chunk, sizeof(tx_chunk));
            if (tx_len > 0) {
                int written = ssh_channel_write(channel, tx_chunk, static_cast<uint32_t>(tx_len));
                if (written < 0) {
                    setState(SshState::ERROR, "SSH write failed");
                    goto loop_exit;
                }
                did_work = true;
            }

            // Read stdout non-blocking
            int n = ssh_channel_read_nonblocking(
                channel, rx_buf.data(), static_cast<uint32_t>(rx_buf.size()), 0);
            if (n > 0) {
                if (data_cb_) {
                    data_cb_(rx_buf.data(), static_cast<size_t>(n));
                }
                did_work = true;
            } else if (n == SSH_EOF || ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) {
                clean_disconnect = true;
                break;
            } else if (n < 0) {
                Serial.printf("[SSH] stdout read error rc=%d err=%s\n", n, ssh_get_error(session));
                setState(SshState::ERROR, "SSH read error");
                break;
            }

            // Read stderr non-blocking
            int n_err = ssh_channel_read_nonblocking(
                channel, rx_buf.data(), static_cast<uint32_t>(rx_buf.size()), 1);
            if (n_err > 0) {
                if (data_cb_) {
                    data_cb_(rx_buf.data(), static_cast<size_t>(n_err));
                }
                did_work = true;
            } else if (n_err == SSH_EOF || ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) {
                clean_disconnect = true;
                break;
            } else if (n_err < 0) {
                Serial.printf("[SSH] stderr read error rc=%d err=%s\n", n_err, ssh_get_error(session));
                setState(SshState::ERROR, "SSH stderr read error");
                break;
            }

            if (ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) {
                clean_disconnect = true;
                break;
            }

            if (!did_work) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                taskYIELD();
            }
        }

        if (stop_requested_.load()) {
            clean_disconnect = true;
        }
    } while (false);

loop_exit:
    if (channel) {
        if (ssh_channel_is_open(channel)) {
            ssh_channel_send_eof(channel);
            ssh_channel_close(channel);
        }
        ssh_channel_free(channel);
        channel = nullptr;
    }

    if (ssh_is_connected(session)) {
        ssh_disconnect(session);
    }
    ssh_free(session);

    secureClearString(private_key_pem_);

    if (clean_disconnect && state_.load() != SshState::ERROR) {
        setState(SshState::DISCONNECTED, "Disconnected");
    }
}

}  // namespace esp_cpa_ssh
