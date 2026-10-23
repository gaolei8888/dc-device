# dc-device × dcd-hw-bench 实机打通实施计划（dc-device 责任书）
日期：2026-10-22 · 状态：跨仓库实施指引，非已完成功能

## 1. 目标与边界
使命是“Connect any device to any AI agent”；本项目承担**设备上的协议、可验证身份、能力清单、调用、自检、诊断和适配器**，不承担 DCD 的用户批准、USB 宿主枚举、工作台 UI、跨设备任务编排。与 [dcd-hw-bench 配套文件](https://github.com/gaolei8888/dcd-hw-bench/blob/feature/equipment-mcp/docs/2026-10-22-dc-device-integration-action-plan.md) 一起实施。不要仅以 README 或单仓单元测试通过宣称打通。

共同 P0 验收：在同一块真实 ESP32-S3 上，用户通过 DCD 硬件工作台看到 USB 候选，主动批准非被动探测/配置步骤，通过 dc-device 固件的 DCSETUP hello（若固件支持）取得稳定身份和网络端点，DCD 核对 HTTP /identity 与 USB 身份一致，用户确认网络设备登记，再通过现有的 dc-device /manifest 和 /invoke 实际执行无动作的 heap.read，显示可核实的结果；断开与重连均不错误复用身份或权限。允许对不支持 DCSETUP 的 bringup 固件走“明确显示不支持 + 用户手动按地址登记”的兼容路径，不得假称自动接入成功。

## 2. 已知事实，开发前重新验证
- `examples/esp32/bringup` 已有 8787 HTTP /health /identity /manifest /status /diagnostics /self_test /invoke；只有 `heap.read`，**不驱动 GPIO 和外设**，不能当成全面硬件控制或 DCSETUP 配网固件。
- `docs/usb-setup-protocol-v0.1.md` 指明 `camera_board` 示例的 `hello/wifi/forget_wifi` 已实现并有部分实板验证，`token` 未实现；原文部分早期描述与其“Implementation”补充不同，以源码、当次固件镜像与实机日志为准。
- USB 串口桥的 VID/PID、路径只识别**接口**，不能证明 ESP32 芯片身份、外设和板型。USB 与 HTTP 绑定必须依赖可信的设备标识和实际核对。
- `dc-device-sim` 可用于契约与故障模拟；不能代替实机证明。

## 3. 本仓库必须交付
### P0-A：明确一款可用于联调的真实固件
1. 固定 ESP32-S3 参考板**准确型号、引脚、flash/PSRAM、串口桥、芯片身份**；声明“已测试组合”，不能把某种 CH343 当成唯一板型。
2. 选择一份正式可发布的 ESP-IDF 固件（优先已有 `camera_board` 路线，在硬件兼容前提下）；产出可验证的 release manifest：目标芯片/板型、版本、SHA-256、映像分段与偏移、分区/NVS 保存策略、支持协议版本、编译来源和适配范围。无合格固件时 DCD 必须禁止烧录。
3. 将固件启动、USB 配网、HTTP 服务能力及安全限制放在同一版本说明，不能把 bringup、camera_board 的特性混称为同一个版本。
4. 实机证明上电后固定超时可获得 `DCSETUP hello`，包括 Wi-Fi 不可用时仍可响应；对开启串口造成复位的 CH343 板子记录实际等待时间与重新发现步骤。
### P0-B：稳定的身份及连接契约
1. `DCSETUP hello` 应返回稳定 `serial` / `device_id` / 固件版本 / setup_protocol / 网络状态、IP 和端口（已连接时）。
2. `GET /identity` 返回可比较的稳定硬件身份。**定义并测试字段规范化**（MAC 大小写、分隔符、serial 与 ID 区别）以及与 `hello` 的严格核对规则。发生不一致时 DCD 必须拒绝自动关联并提示人工处理。
3. `GET /health` 只能表示服务健康，不能据此认定外设正常；`GET /manifest` 是当前固件真实报告的能力，不能按 USB VID/PID 或模板猜能力。
4. 清楚区分 `USB_PRESENT`、`SETUP_RESPONSIVE`、`NETWORK_REACHABLE`、`IDENTITY_VERIFIED`、`CAPABILITIES_VERIFIED`、`AI_ALLOWED`；设备协议只提供证据，DCD 决定用户权限/就绪。
5. 文档给出 **不含凭据** 的真实 `hello`、`identity`、`manifest`、`invoke(heap.read)` 示例与错误码；定义失败、超时、断网时不可自动重试非幂等动作。
### P0-C：实际能力闭环
1. 选定同一固件中的只读 `heap.read`，验证 `/manifest` 包含它、`POST /invoke` 响应符合现有 HTTP 契约，记录实机输出与时间戳（不泄露隐私信息）。
2. 加契约测试覆盖字段类型、边界、错误结果、离线行为、健康/自检语义、能力版本变化；提供供 DCD 和工作台消费的**脱敏固定测试样本**。
3. 与 DCD 项目对齐宿主“经授权按能力读取并回传结果”的最小接口；dc-device **不自行决定批准机制**，也不能绕开 DCD 直接让插件下发真实动作。
4. 实现并验证设备自身的超时安全与本地停止（运动能力纳入 P1 前的阻断条件），不得把 HTTP 接收成功当成物理动作完成。

## 4. P1：外设识别和能力扩展
- 以**明确板型的 profile + 实际固件报告 + 检测证据**为基础，逐一添加 LED、按钮、I2C 传感器、显示屏，再增加舵机/电机/摄像头；记录每个外设的物理实例、引脚、检测覆盖率、来源和未知项。通用 I2C 扫描不能证明所有外设都存在或工作正常。
- `self_test` 对只读检查自动执行；对可能运动、写入或造成复位的检查标注副作用，必须由 DCD 授权后执行，安全状态由设备本地保证。
- 后续整机/子系统/board/peripheral/capability 拓扑与 dcd-hw-bench 设计统一；别从能力名称猜测物理结构。
- MicroPython 可作为**非运动**教学设备的低优先级路径；运动设备保持经验证的 ESP-IDF 本地安全内核。不把某种语言设为 dc-device 架构前提。

## 5. 跨仓库接口表（责任边界）
| 协议/事实 | dc-device | DCD 宿主 | dcd-hw-bench |
|---|---|---|---|
| USB 候选 | 不推断板型 | 被动枚举/可信许可探测 | 展示候选与来源 |
| `DCSETUP hello/wifi` | 固件端协议 | 独占串口、批准与凭据安全输入 | 请求流程、展示状态 |
| `/identity` | 稳定可核对身份 | 核对 USB↔HTTP，维护唯一设备表 | 显示证据及绑定状态 |
| `/manifest`、`/status` | 设备能力与真实健康 | 读写准入、缓存失效 | 可视化 |
| `/invoke` | 按契约执行能力 | 授权、安全、结果/超时 | 发起任务、跟踪而不直连 |
| 运行与多机协作 | 设备本地保护 | 资源仲裁与执行 | 协作草案、UI |
| 仿真 | 共同能力契约 | 同一路由验证 | 明确标注模拟/真实 |

## 6. 联调次序与验收证据
1. **冻结版本矩阵**：实板型号、固件 commit/image SHA、dc-device 版本、DCD 版本、工作台版本与插件签名。
2. 先在真实设备本地验收 `hello`→`identity`→`manifest`→`heap.read`。每一步保存脱敏请求/响应和结果，不把“能 ping”冒充成功。
3. 再与 DCD + 工作台在隔离测试配置中跑全链路，**用户亲自确认**登记/烧录/配网/授权；不得替用户点确认卡。
4. 测插拔、串口路径变化、Wi-Fi 断开重连、IP 变化、不同设备相同转串口芯片、身份不匹配、旧固件不支持 DCSETUP、拒绝授权和请求结果未知。
5. 失败时按边界定位并保留复现报告；不能出现“自动绑定另一台”或“未知即成功”。
6. P0 结束条件：真实板 `heap.read` 经 DCD 授权从工作台成功触发并显示结果。P1 才允许宣称具体外设自动发现/硬件动作可用。

## 7. 本轮非目标及操作安全
不承诺任意陌生硬件无驱动即插即用；不重写 DCD 权限；不因工作台需要就私自开启网络扫描；不执行固件烧录、擦除、写 NVS、主动芯片探测或运动自检，除非在真实宿主中获得用户明确授权。刷机需匹配的正式固件、完整备份及可验证恢复方案。
