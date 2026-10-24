# ESP32 外设支持路线图（草案，未实现）

状态：草案，2026-10-24。lei："所有 esp32 的硬件我们都要支持"。本文只回答"怎么做、先后顺序、要什么实物、manifest 还缺什么"，没有任何实现。
原则不变：**没验证过就不声明、不做假按钮**；板子自检所有外设，观察不到的硬件不报 ok（见 `self-check-v0.1.md`）。

## 1. 怎么做到"新增一种外设不用从头写一整套"

### 现状：哪些已经通用，哪些还逐板写

已通用（`platforms/esp32/`，任何板子直接用）：
- 能力声明：`CapabilitySpec`（name、kind、参数 schema、返回 schema、safety、`available` 门、`expected_duration_ms`）、`x_dc_payload` 大数据提示、错误 `code`。
- 安全内核：`max_rate_hz`、`max_duration_ms` + 安全态 + 看门狗、`emergency`、自检时 409。
- 自检框架：`Detector / Finding / Verdict`、`optional`（只豁免 not_found）、NVS 声明部件（USB `declare`）、I2C 扫描 + 芯片 ID 表（`dc_device_i2c_ids`）、引脚普查、麦克风/摄像头"自己看到数据才算 ok"、"听到才算扬声器 ok"。
- HTTP、USB 配网、Wi-Fi、发布/刷机流程。

还逐板写（`examples/esp32/camera_board/main/main.cpp` 一个大文件）：
- 引脚表（哪个口接什么）、每个外设的驱动代码、能力 handler、检测器、能力与检测器的接线。
- 板型 profile 只有 JSON 一份（`profiles/*.profile.json`），只列必需/可选能力名，不描述外设本身。

### 提案：外设模块（Peripheral module）

一个外设模块 = 一个很小的 C++ 单元，固定四件事：

```
struct PeripheralModule {
  name, class(Signal/Output/Bus...),
  pins          // 由板型表给，不由 AI/用户给
  probe()       -> Finding          // 自己观察到了什么，必须诚实
  capabilities()-> [CapabilitySpec] // 只有 probe/声明通过才出现在 manifest
  safe_state()  // 停下回到什么状态（输出类必填）
}
```

- 模块库放 `platforms/esp32/peripherals/`：`gpio_out`(LED/继电器)、`pwm_out`(LED 亮度/蜂鸣器)、`servo`、`dc_motor`(H 桥)、`adc_in`、`gpio_in`、`i2c_sensor`(按芯片 ID 表选驱动：温湿度/光照/IMU/气压)、`ultrasonic`、`one_wire`……
- 板型文件只写一张表：`{模块, 实例名, 引脚, 参数}`，例如 `{servo, "pan", GPIO47, 500-2500us}`。新增外设 = 加一行 + （若是新种类）加一个模块，不改框架、不改 HTTP。
- 声明规则统一：能自己观察 → probe 观察到才声明；观察不到的输出（舵机、继电器、LED）→ 只有用户用 USB `declare` 声明"接了"才出现，并且 manifest 里标明证据等级（见 §4 `x_dc_evidence`），自检里是 unverified，除非有反馈通道。
- profile 扩展：profile JSON 增加 `peripherals`（该类板子应有哪些外设实例及其必需/可选），DC 据此知道"缺了什么"，而不是只看能力名。
- 命名规范：`<外设>.<动作>`，实例用参数 `port`（如 `servo.set_angle {port:"pan", angle}`），不是每个实例一个能力名。

## 2. 按风险排序的外设清单

| 阶段 | 外设 | 声明方式 | safety 声明 | DC 侧配合 |
|---|---|---|---|---|
| A 只读 | 温湿度(SHT3x/AHT)、光照(BH1750)、气压、IMU(MPU6050/LSM6)、ADC 电压、距离(VL53L0X/超声)、数字输入 | I2C ID 命中 + 读数在量程内 + 多次读数合理 → ok；读数恒定/越界 → unverified | 只读：`max_rate_hz`，无 motion | 无确认卡；读数带单位和量程 |
| B 低风险输出 | LED/RGB、屏幕(已有)、蜂鸣器/扬声器(已有)、OLED | 无回读 → 用户声明 + 人工确认一次；LED 可用光敏/ADC 回读做真实验证 | `max_rate_hz`；蜂鸣器/LED 闪烁加 `max_duration_ms`，安全态=关 | 无确认卡 |
| C 会动但有限 | 舵机 | 无位置回读：用户声明 + 人工确认；有电位器/电流采样才能 ok | `motion:true`、角度范围、最大转速、`max_duration_ms`、安全态=保持/回中(需板主定)、`servo.release` 急停(断 PWM) | 需要确认卡；DC 对 motion 类按 `requires_confirmation` |
| D 高风险 | 直流电机(H 桥)、继电器、加热 | 必须有回读（电流/编码器/负载侧电压）才算 ok；否则只能"声明未验证"并默认**不开放** | 占空比上限、`max_duration_ms`(短)、安全态=断电/刹车、硬件看门狗、`emergency` 能力、上电默认关、浏览器断线/网络断线自动停 | 每次确认；DC 不得缓存确认；急停按钮常驻 |

每阶段共同要求：上电默认安全态；复位/崩溃后输出为安全态；任何输出能力都配套一个 `emergency` 停止能力；自检时不得驱动高风险输出（Full 自检对 D 类只做无负载检查）。

## 3. 没有实物就无法验证的清单（需要 lei 提供）

| 外设 | 需要的实物 | 接线（脚由我们定，见下） | 能验证到什么 |
|---|---|---|---|
| 温湿度/光照/气压/IMU(I2C) | 对应模块 1 个 | SDA/SCL，用 §3.1 选定的脚 | ID 响应 + 读数合理 + 手捂/呵气有变化 |
| 距离(VL53L0X 或 HC-SR04) | 模块 + (HC-SR04 需分压到 3.3V) | I2C 或 TRIG/ECHO 各一脚 | 量程内读数 + 手挡变化 |
| ADC 电压 | 电池/分压电阻 | 一个 ADC 口 | 已知电压比对 |
| LED/RGB | LED+限流电阻，或板载 | 一个 GPIO/PWM | 光敏电阻/摄像头看亮灭才算真验证；否则人工确认 |
| 舵机 | SG90 等 + 外部 5V 供电(不要用 3.3V 脚) | 一个 PWM 口 + 共地 | 无回读只能人工确认；有电位器回读才能 ok |
| 直流电机 | 电机 + H 桥(如 TB6612/DRV8833) + 外部电源 | 2 个 PWM/方向口 + 电流采样(可选) | 需电流采样或编码器才能自动验证 |
| 继电器 | 继电器模块 + 隔离的测试负载 | 一个 GPIO | 需负载侧电压/电流回读；否则只"声明" |
| 加热 | 不建议在无温度回读 + 硬件保护下做 | — | 先不做 |

### 3.1 引脚由我们选，lei 只管接线

lei 不需要懂引脚，也不决定引脚。所有引脚（包括 I2C 用哪两个脚）由我们从安全集合里选定，写进板型表；每接一个外设，给 lei 一份大白话接线说明，他照着接、接好告诉我们，我们去验证，读不到就不声明。

选脚规则：只用 `dc_device_pins` 安全集合（1,2,4-18,21,38-42,47,48）里，去掉屏幕/扬声器/麦克风/相机已占用的脚，再去掉启动脚、USB、闪存/PSRAM 脚。6740/6718 上剩下的空脚是 15、38、47、48（待对着实物板确认没有板载 LED 等占用）。暂定分配（草案，接线前对着实物再核一遍）：
- I2C 总线：SDA = 47，SCL = 48（相机的 SCCB 用 4/5，不共用）。
- 舵机信号 = 15；LED/继电器类输出 = 38；其余再从剩下的脚里分。

每份接线说明包含：
1. 你需要的东西（实物名称、几根线）。
2. 每根线："模块上写着 SDA 的针 → 板子上标着 47 的孔"，并写明线的颜色建议（红=电源、黑=地、其余自选，并请 lei 告诉我们实际颜色）。
3. 接之前先断电；电源/地先接。舵机、电机必须用外部电源并共地，说明里明确写。
4. 一张简单接线图：文字图（模块 —— 线 —— 板子孔号）或 SVG 图片，随说明一起给。
5. "接好后告诉我"；我们随后读设备、验证，通过才声明，读不到就告诉 lei 哪根线要检查。

## 4. 通用界面还需要的 manifest 元数据

现有可用：`kind`、参数 JSON Schema（type/enum/minimum/maximum）、`safety.*`、`expected_duration_ms`、`x_dc_payload`、`available`（没声明就不出现）。建议新增（均为可选、向后兼容、`x_dc_` 前缀由 DC 忽略未知项）：

1. **单位与精度**：参数/返回字段加 `unit`（"°C"、"lux"、"deg"）、`precision`（小数位）。
2. **量程与步长**：用标准 `minimum/maximum` + `multipleOf`；`default`；传感器返回加 `x_dc_range`（物理量程）和 `x_dc_resolution`。
3. **安全态**：`safety.safe_state` 一句人话（"断电并刹车"），`safety.emergency_capability` 指向本外设的停止能力名，输出类可有 `x_dc_state_read`（对应读回能力，如 `servo.get`），界面才能显示当前值而不是盲发。
4. **证据等级** `x_dc_evidence`：`"measured"`（板子自己观察到）/ `"declared_by_user"`（用户声明，未验证）/ `"human_confirmed"`。界面按等级加标记或默认折叠，防止把未验证的东西当真。
5. **外设分组**：`x_dc_peripheral:{name, instance}`，一个外设多个能力、多个实例（`port` 参数的 enum 列出实际存在的口），界面按外设分卡。
6. **界面提示（可选）**：`x_dc_ui:{label, widget: slider|toggle|color|readout|button, group}`。没有就用 schema 默认推断，不得凭它显示未声明的东西。
7. **读数新鲜度**：传感器返回带 `timestamp_ms`(设备开机毫秒) 和 `valid`，读不到就 `valid:false`，不返回 0。
8. **流/事件**（以后）：`kind:"stream"` 的采样率和最大时长；现在不做。

## 5. 建议顺序（里程碑）

1. 抽出 `PeripheralModule` 和板型表，把现有屏幕/扬声器/摄像头改造成模块（行为不变，测试保持绿）。不依赖新硬件。
2. 加 A 类只读：先 I2C 传感器通用模块（按 ID 表）+ ADC；需要 lei 提供一个 I2C 传感器模块。
3. manifest 元数据 §4（先 1/3/4/5），和 DC 插件对齐。
4. B 类：LED/蜂鸣器模块（扬声器已有）。
5. C 类舵机（安全内核已具备大部分）；D 类在有回读硬件、lei 在场、板主定了安全态之后才做。

## 待 lei 回答
- 舵机停下后"保持"还是"松开(断 PWM)"？
- lei 手上有哪些实物、接哪块板？（引脚我们选，不问 lei。）
- D 类（电机/继电器）是否要求先有回读硬件才开放？
