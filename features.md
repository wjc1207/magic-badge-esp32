## Feat. 1. lua scripting support
- magic badge integrates Lua scripting support, allowing ai agents to write and execute Lua scripts for dynamic behavior, task automation, and on-the-fly adjustments to their operations, enhancing flexibility and adaptability in various scenarios

## Feat. 2. social buddy

- Social Buddy is a proximity-based social networking feature for ESP32-S3 wearable devices. Two devices discover each other via BLE beacon broadcasts, perform a lightweight handshake, exchange AES-128-GCM encrypted profiles, then optionally call a cloud LLM to score the match and generate a personalized icebreaker.
- [ESP-NOW will follow the channel of STA, if two devices are connected to two different APs, they won't be able to communicate with each other. BLE won't have this problem, as it doesn't rely on Wi-Fi connectivity.]