# ESP32 bring-up firmware

Joins Wi-Fi and serves the dc-device HTTP contract on port 8787. It drives **no
GPIO pin**, so it is safe on a board whose wiring is not known (for example a
camera board). Its only capability, `heap.read`, returns the free heap.

Verified on one ESP32-S3 board (16MB flash, 8MB PSRAM): `/health`, `/identity`,
`/manifest`, `/status`, `/diagnostics`, `POST /self_test` and `POST /invoke`
answered over Wi-Fi.

## Build (Docker, ESP-IDF v5.4)

The Wi-Fi name and password go in a file outside the repository, for example
`~/.config/esp32-build/bringup/wifi.conf`:

```
CONFIG_DC_DEVICE_WIFI_SSID="name"
CONFIG_DC_DEVICE_WIFI_PASSWORD="password"
```

The password is compiled into the firmware and stays in the build output. Use a
2.4GHz network. Build output and `sdkconfig` are kept outside the repository:

```
docker run --rm -v "$PWD":/project -v ~/esp32-build/bringup:/out \
  -v ~/.config/esp32-build/bringup:/cfg:ro \
  -w /project/examples/esp32/bringup espressif/idf:release-v5.4 \
  idf.py -B /out/build -DIDF_TARGET=esp32s3 -DSDKCONFIG=/out/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;/cfg/wifi.conf" build
```

**Pass `-DIDF_TARGET=esp32s3`.** `CONFIG_IDF_TARGET` in `sdkconfig.defaults` is
not enough on a fresh build: it produces an image for the original ESP32 (the
bootloader offset is 0x1000 instead of 0x0), which will not boot on an S3.
Check that `build/flash_args` starts with `0x0 bootloader/bootloader.bin`.

## Flash

```
cd ~/esp32-build/bringup/build
esptool --port <port> --chip esp32s3 write-flash @flash_args
```

Flashing replaces whatever is on the board. Back it up first with
`esptool read-flash 0 0x1000000 backup.bin`.

## Notes

- Wi-Fi power save is switched off in `dc_device_wifi.cpp`. With it on, a
  few `POST /invoke` calls in the first minutes returned an empty reply; with
  it off there were none in 200 calls. This is one measurement, not a proof.
- `sdkconfig.defaults` selects 16MB flash and the large single-app partition
  table (1.5MB app). Change them for a board with less flash.
