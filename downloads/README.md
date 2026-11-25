# Downloads — Ghar Raksha firmware

- `home_guard.ino` — source code (Arduino IDE se upload karo; apna WiFi naam/password code mein bharo).
- `firmware.bin` — compiled app (PlatformIO 6.2.0, esp32dev, Arduino framework — SUCCESS, RAM 14.7%, Flash 77.0%, 25 Nov 2026).
- `bootloader.bin` + `partitions.bin` — .bin flash karne ke liye zaroori (akeli firmware.bin kaam nahi karegi).

Seedha flash karne ka command (Windows par COM3 ki jagah apna port likho):

```
esptool.py --chip esp32 --port COM3 --baud 460800 write_flash -z 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

Note: .bin mein WiFi naam/password placeholder hai — apna WiFi bharna ho to source code (.ino) use karo.
