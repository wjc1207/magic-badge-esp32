## two-layer config
magic badge uses a two-layer configuration system:
- Layer 1: Compile-time (menuconfig)
- Layer 2: Run-time (web interface or CLI)

## build-time secrets config
```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic" or "openai"
#define MIMI_SECRET_SEARCH_KEY      ""              // optional: Brave Search API key
#define MIMI_SECRET_TAVILY_KEY      ""              // optional: Tavily API key (preferred)
#define MIMI_SECRET_PROXY_HOST      ""              // optional: e.g. "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // optional: e.g. "7897"
```

## build-time hardware config

Use menuconfig to choose which optional hardware tools are included in firmware:

```bash
idf.py menuconfig
```

Path:

- `MagicBadge Optional Hardware Tools`
  - `Enable RGB control tool`
  - `Enable camera capture tool`
  - `Enable BLE listener tool`

If a tool is disabled here, it is not compiled and uses zero code/RAM.

## runtime feature config

After flashing, configure runtime feature toggles via onboarding portal:

- Connect to `MagicBadge-XXXX`
- Open `http://192.168.4.1`
- Go to `Features` section

Rules:

- Layer 1 disabled in menuconfig: tool is not compiled and hidden in onboarding
- Layer 1 enabled in menuconfig: tool is visible and can be enabled/disabled at runtime
- Runtime toggles are persisted in NVS and applied after restart
