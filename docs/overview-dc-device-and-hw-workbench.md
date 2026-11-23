# dc-device 与硬件工作台：一页说明

状态：2026-11-23 草案。"硬件工作台"指运行在 dc_desktop（DC 桌面）里的 dcd-hw-bench 插件及 DC 的设备管理，下文简称"桌面侧"。下面的事实来自本仓库文档和已做过的实机验证，没有实机验证过的写成"计划"。

## 一句话

**dc-device 让一块硬件用统一的方式说清楚"我是谁、我现在能做什么、我是否健康"，并安全地执行动作；桌面侧负责发现设备、让用户批准、把能力交给 AI 使用。** 两边通过一份很小的 HTTP 契约对接，互不绕过。

## 分工

| | dc-device（本仓库，在设备上） | 桌面侧（DC 桌面 + 硬件工作台插件） |
|---|---|---|
| 身份 | `/identity`：稳定序列号（MAC）、型号、固件版本 | 核对 USB 看到的设备与网络里的设备是同一块，维护设备台账 |
| 能力 | `/manifest`：此刻**真正有**的能力（参数、范围、安全、预计耗时）；没证据的外设不声明 | 按清单自动生成界面和给 AI 的工具；清单变了要用户重新确认 |
| 健康 | `/status`、`/diagnostics`、`/self_test`：自己检查每个外设，观察不到就写"未验证"，不报 ok | 展示健康；对"失败/未验证"给人话解释 |
| 执行 | `POST /invoke`：校验参数、限速、超时自动回安全态、急停 | 用户批准、限流（动作 ≤10 次/秒）、超时（预计最长+2 秒）、一次一个动作、记录结果 |
| 配网 | USB `hello / wifi / forget_wifi`，不需要访问码，全在局域网 | 独占串口、让用户输入 Wi-Fi，不看不存明文凭据 |
| 刷机 | 发布包（三段镜像 + manifest.json，含 sha256） | 刷机前核对 sha256 和芯片 MAC，只刷这三段，不擦整片闪存 |

**铁律（两边共同遵守）：没验证过就不声明、不做假按钮；设备自检不到的东西不说 ok；设备不决定谁有权用，桌面侧不替设备猜能力。**

## 数据怎么走

```text
AI / 用户 ──> 硬件工作台界面 ──> DC 桌面（批准、限流、超时、记录）
                                      │  HTTP v0.1，局域网，端口 8787
                                      ▼
   USB 配网(hello) ─────────>  ESP32 设备上的 dc-device
                                /identity /manifest /status
                                /diagnostics /self_test /invoke
                                      │
                                      ▼ 外设模块：摄像头、屏幕、扬声器、……
```

- 大块数据（图片）：`camera.capture` 返回 JPEG（base64，≤100 KB）；`screen.write_block` 每次接收 ≤7680 字节像素，一张 240×320 图是 20 块，由 SDK（Python `show_image`、JavaScript `showImage`）切块发送，裁切充满、不旋转。参数上的 `x_dc_payload` 提示告诉桌面侧"这是大数据，别当文字显示"。
- 设计上板子之间可以直接通信传数据，由桌面侧负责告诉某块板该连哪块板（尚未实现）。

## 现在能用什么（实机验证过）

| 板子 | 有什么 | 清单里的能力 |
|---|---|---|
| 6740 | 摄像头 OV2640、8 MB PSRAM | `camera.capture`（开机时板子自己拍一张有细节才声明；预计 600 ms，最长 15 s）、`heap.read` |
| 6718 | 屏幕 ST7789 240×320（只写）、扬声器 | `screen.fill_test`、`screen.draw_rect`、`screen.write_block`、`speaker.beep`、`heap.read` |

端到端已验证：6740 拍照 → `show_image` → 6718 屏幕显示，颜色和方向正确。两块板固件 `0.1.0+ae20386`/`0.1.0+bd02d5d`，`/status` 均为 ok，重启 4 次清单稳定。

## 仓库里有什么

- `platforms/esp32/`：设备端通用部分（HTTP、清单、安全内核、自检、配网）。
- `examples/esp32/camera_board/`：当前两块板用的固件；`scripts/build-release.sh` 出干净发布包。
- `sdk/python/`、`sdk/js/show_image.mjs`：给 AI/桌面插件用的客户端。
- `docs/`：契约（`http-transport`、`health-contract`、`self-check`、`safety-*`、`usb-setup-protocol`）与设计（`superpowers/specs/`）。

## 已知限制与下一步

- 偶发：开机头十几秒 HTTP 读不到清单；拍照偶尔慢到 4–12 秒（已把预计最长提到 15 s）。
- 屏幕和扬声器设备自己观察不到，只能"人确认"；摄像头把镜头捂住仍会被当作"有画面"（已决定保持）。
- 没做：OTA 无线升级（设计已写，先用 USB）、电机/舵机/传感器等更多外设（路线图在 `superpowers/specs/2026-10-24-esp32-peripheral-roadmap.md`；目前 lei 只有摄像头和屏幕，需要实物才能真实验证）。
- 访问码已决定不做；设备只在局域网内使用，没有认证，不要暴露到公网。
