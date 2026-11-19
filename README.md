# ESP-CPA-SSH

**M5Stack Cardputer ADV** (ESP32-S3FN8) 向けの Ed25519 対応 VT100/ANSI SSH クライアントです。  
An Ed25519-capable VT100/ANSI SSH client for the **M5Stack Cardputer ADV** (ESP32-S3FN8).

---

## 概要 / Overview

- **対応ハードウェア / Target Hardware**: M5Stack Cardputer ADV (Stamp-S3A / ESP32-S3FN8, 8MB Flash, PSRAM なし, TCA8418 I2C キーボード, ST7789V2 240x135 IPS 液晶)
- **表示 / Display**: 6x8 ビットマップフォント (`Font0`) による **40列 × 15行** ターミナル領域 + 下部 **15px ステータスバー** (Wi-Fi RSSI / SSH 状態 / 修飾キー / バッテリー残量)
- **ライブラリ / Stack**: PlatformIO (`espressif32 @ 6.11.0`, `m5stack-stamps3`), `M5Cardputer`, `LibSSH-ESP32`, `ArduinoJson`

## 主な機能 / Features

- **Ed25519 & パスワード認証 / Ed25519 & Password Auth**:
  - `ssh-ed25519` 公開鍵認証 (`-----BEGIN OPENSSH PRIVATE KEY-----` 形式、パスフレーズ有無の両対応) および `curve25519-sha256` 鍵交換に対応
  - パスワード認証 (`AUTH_PASSWORD`) にも対応
- **TOFU ホスト鍵検証 / TOFU Host Key Verification**:
  - サーバー公開鍵の SHA-256 フィンガープリントを初回接続時に確認・保存 (`/known_hosts` in LittleFS)、鍵不一致時は警告ダイアログを表示
- **プロファイル管理 / Profiles (LittleFS `/config.json`)**:
  - Wi-Fi プロファイル 最大 **3件** / SSH プロファイル 最大 **3件** を本体フラッシュに保存・編集可能
- **microSD インポート / microSD Import**:
  - microSD カード (FAT32) から `/id_ed25519` 秘密鍵および `/cardputer_ssh.json` 設定を本体 LittleFS へインポート可能 (改行コード `\r\n` → `\n` 自動正規化)
- **M5Launcher 対応 / M5Launcher Compatible**:
  - `default_8MB.csv` 向けの単体アプリバイナリ (`firmware.bin`) を生成
  - メニューの **Exit to M5Launcher** から `otadata` クリア / OTA パーティション切替 + Deep-Sleep リセットにより M5Launcher へ復帰可能

---

## ビルドと M5Launcher への導入 / Build & M5Launcher Installation

### ビルド / Build

```bash
pio run
```

ビルド成功後、`.pio/build/cardputer-adv/firmware.bin` が生成されます。

### M5Launcher での起動 / Running via M5Launcher

1. `.pio/build/cardputer-adv/firmware.bin` を任意の名前 (例: `esp_cpa_ssh.bin`) で FAT32 microSD カードにコピーします。
2. Cardputer ADV で **M5Launcher** を起動し、SD カードから該当 `.bin` を選択してインストール・起動します。
3. アプリ終了時はメインメニューの **5. Exit to M5Launcher** を選択すると M5Launcher へ戻ります。

---

## Ed25519 鍵・SD インポート準備 / Ed25519 Key & SD Import Setup

1. PC 等で Ed25519 鍵ペアを生成し、公開鍵を接続先サーバーの `~/.ssh/authorized_keys` に登録します:
   ```bash
   ssh-keygen -t ed25519 -f id_ed25519
   ```
2. 秘密鍵 `id_ed25519` を microSD カードのルート (`/id_ed25519`) にコピーします。
3. (任意) 一括設定用 `/cardputer_ssh.json` を microSD カードのルートに作成します:
   ```json
   {
     "wifi": [
       { "ssid": "MyWiFi", "password": "secret" }
     ],
     "ssh": [
       {
         "label": "homelab",
         "host": "192.168.1.10",
         "port": 22,
         "username": "ken",
         "auth_type": "ed25519",
         "key_path": "/id_ed25519",
         "secret": ""
       }
     ],
     "last_wifi_idx": 0,
     "last_ssh_idx": 0
   }
   ```
   ※ パスワード認証の場合は `"auth_type": "password"`、`"secret"` にパスワードを指定します。パスフレーズ付き Ed25519 鍵の場合は `"secret"` にパスフレーズを指定します。
4. microSD カードを挿入し、メインメニューで **4. Import from SD** を実行します。

---

## 操作方法・キーバインド / Controls & Keybindings

### メニュー画面 / Menu Mode

| キー / Key | 動作 / Action |
|---|---|
| `↑` / `↓` (`Fn + ;` / `.`, または `;` / `.`) | 項目移動 / Move selection |
| `Enter` (`OK`) | 決定・編集 / Select / Edit |
| `1`〜`5` (メインメニュー), `1`〜`3` (プロファイル一覧) | 番号ショートカット / Quick select |
| `Esc` (`Fn + `` ` `` ` または `BtnA`) | 戻る / Back |

### ターミナル画面 / Terminal Mode

| キー / Key | 動作 / Action |
|---|---|
| `BtnA` (本体側面ボタン) または `Ctrl + ]` (`Opt + ]`) | 切断してメインメニューへ戻る / Disconnect & return to Main Menu |
| `Fn + ;` / `.` / `,` / `/` | 矢印キー `↑` / `↓` / `←` / `→` / Arrow Up / Down / Left / Right |
| `Fn + `` ` `` ` | `ESC` (`0x1B`) |
| `Fn + 1`〜`9`, `0` | `F1`〜`F10` |
| `Fn + [` / `]` | `Home` / `End` |
| `Fn + '` / `\` | `Page Up` / `Page Down` |
| `Fn + Backspace` | `Delete` (`\e[3~`) |
| `Shift + Tab` | `Backtab` (`\e[Z`) |
| `Shift + Fn + ;` / `.` または `Alt + ;` / `.` | スクロールバック上 / 下 (3行) / Scrollback Up / Down (3 lines) |
| `Ctrl` (または `Opt`) + `A`〜`Z` | 制御文字 (`0x01`〜`0x1A`) / Control characters |
| `Ctrl + [` / `\` / `^`(`6`) / `_`(`-`) / `Space` | `0x1B` / `0x1C` / `0x1E` / `0x1F` / `0x00` |
| `Alt + Key` | `ESC` プレフィックス + Key / `ESC` prefix + Key |
