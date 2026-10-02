# magic badge: let badges talk with each other

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>

**The world's first agentic badge that can talk with each other on a $5 chip. No Linux. No Node.js. Just pure C**

magic badge turns a tiny ESP32-S3 board into a agentic badge that can talk with each other. Plug it into USB power, connect to WiFi, and walk on the street to connect with others — all on a chip the size of a thumb. 

## Meet magic badge

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Handy** — Message it from Telegram or Feishu, it handles the rest
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, 0.5 W, runs 24/7
- **Lovable** — One ESP32-S3 board, $5, nothing else

## Hardware Requirements

- ESP32 development board with PSRAM
- WS2812 RGB LED (default GPIO 48)
- camera module (optional, e.g. OV2640)
- ble thermometer sensor (optional, e.g. LYWSD03MMC)

## Software Requirements

<details>
<summary>Obtain Feishu (Lark) Token</summary>

1. Open [Feishu Console](https://open.feishu.cn/app)
2. Click "Create Custom App"
3. Fill in the bot name and upload an avatar (optional)
4. In "Add Features", select "Bot" and click "Confirm"
5. In "Permissions", add the following permissions:
   - "im:message:send_as_bot"
6. In "Events and Callbacks", add the following event subscriptions:
   - "im.message.receive_v1"
7. Save the token for later configuration
</details>

<details>
<summary>Obtain Telegram Bot Token</summary>

1. Search for `@BotFather` in Telegram and send `/start`
2. Send `/newbot` to create a new bot, follow the instructions to set a name and username
3. After completion, BotFather will send a message containing the token (format: `123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11`)
4. Save this token for later configuration
</details>

#### Obtain LLM API Keys

##### Supported LLM Providers and How to Obtain:

<details>
<summary>Anthropic (Claude)</summary>

- Visit [Anthropic Console](https://console.anthropic.com/)
- Create an account and log in
- Navigate to the API Keys page
- Click "Create Key" and save the generated key
</details>

<details>
<summary>OpenAI (GPT)</summary>

- Visit [OpenAI Platform](https://platform.openai.com/)
- Create an account and log in
- Navigate to the API Keys page
- Click "Create new secret key" and save the generated key
</details>

## Quick Start

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/wjc1207/magic-badge-esp32.git
cd magic-badge-esp32

idf.py set-target esp32s3
```

### 1. Build and Flash

```bash
# Clean build (required after any mimi_secrets.h change)
idf.py fullclean && idf.py build

# Find your serial port
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# Flash and monitor (replace PORT with your port)
# USB adapter: likely /dev/cu.usbmodem11401 (macOS) or /dev/ttyACM0 (Linux)
idf.py -p PORT flash monitor
```

### 2. Configure Secrets

You can configure secrets in one of two ways:

#### Method A: Using cli
- use CLI commands to configure secrets and feature toggles
- e.g. `wifi_set MySSID MyPassword` or `set_tg_token 123456:ABC...`

#### Method B: Using Web Configuration Portal
- After first boot, the device will create a WiFi hotspot named `MagicBadge-XXXX`
- Connect to this hotspot and visit `http://192.168.4.1`
- Configure all secrets and settings in the web interface

## two-layer configuration
magic badge uses a two-layer configuration system, see [config.md](config.md) for details.

## Examples
example use cases of magic badge, see [example.md](example.md) for details.

## Features
see [features.md](features.md) for detailed feature list and roadmap.

## Supported Channels

| Channel | Description | Features |
|---------|-------------|----------|
| **Telegram** | Native Telegram bot interface | Full command support, file attachments, inline queries |
| **Feishu** | Feishu/Lark robot integration | Enterprise messaging, group chat support |

> **Enable only one channel at a time.** Each channel holds its own TLS
> connection open, and the LLM proxy opens another one per inference. The
> internal DRAM left over after WiFi, BLE and TLS is only enough for about two
> of these at once: with Telegram, Feishu and an LLM call all live, the TLS
> write path fails to allocate its AES-GCM buffer and every inference dies with
> `esp-aes: Failed to allocate memory` /
> `transport_base: esp_tls_conn_write error`.
>
> That failure looks like a crypto bug and is really a memory budget problem, so
> turn off the channels you are not using:
>
> ```
> set_feature telegram_bot 0
> set_feature feishu_bot 0
> restart
> ```

## Tools

| Tool | Usage | 
|----------|-------|
| **Cron** | run task at given unix timestamp or at given interval | 
| **File** | add, remove, edit and list files | 

| **Device Control (RGB)** | immediate WS2812 RGB control on GPIO48 (`set/off/status`) (optional) |
| **HTTP Request** | execute `http` request to access API | 
| **Script** | write and run `lua` script in real time | 
| **Web Search** | Search anything on the Internet | 
| **camera capture** | Capture images from the onboard camera (optional) | 
| **bthome listener** | Listen for BTHome device updates (optional) | 
 
## Supported LLM Providers

| Provider | Value | API Endpoint | Notes |
|----------|-------|-------------|-------|
| Anthropic (Claude) | `anthropic` | api.anthropic.com | Default |
| OpenAI (GPT) | `openai` | api.openai.com | |
| OpenRouter | `openrouter` | openrouter.ai | Free tier available |
| NVIDIA NIM | `nvidia` | integrate.api.nvidia.com | Free tier available |
| Alibaba Cloud Qwen | `qwen` | dashscope.aliyun.com | |

## Supported Web Search Providers

| Provider | Value | API Endpoint | Notes |
|----------|-------|-------------|-------|
| tavily | `tavily` | api.tavily.com | Default |
| brave | `brave` | api.search.brave.com | |

## Star History

<a href="https://www.star-history.com/?repos=wjc1207%2Fmagic-badge-esp32&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=wjc1207/magic-badge-esp32&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=wjc1207/magic-badge-esp32&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=wjc1207/magic-badge-esp32&type=date&legend=top-left" />
 </picture>
</a>

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Links

- Special thanks to the [mimiclaw](https://github.com/memovai/mimiclaw) project
- Author: Junchi Wang
