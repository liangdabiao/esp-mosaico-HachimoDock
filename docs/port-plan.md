# HachimoDock → ESP-Mosaico 移植改造方案(深度研究 + 解决方案)

调研日期:2026-10-23。源:`D:\HachimoDock-main`(v0.7.72-p4 / PC 0.1.101,源码齐全)。
目标:把 HachimoDock(哈基米机,AI Agent 桌面宠物)改造为 ESP-Mosaico(esp32s31)版本。

## 1. HachimoDock 是什么(源码结论)

三层系统,PC 是大脑、设备是脸:
- **PC 客户端**(Tauri2/Rust ≈52k 行 + React + Node bridge):检测 Claude Code/Codex/OpenClaw/MiMoCode/WorkBuddy 的
  session 状态(hook + 日志监视),归一化为 idle/working/speaking/waiting_user/done/error,经 USB 下发;
  同时负责 ASR(火山流式)/TTS(豆包)/人设 LLM/形象转码(FFmpeg H.264 640×480@15fps)/clawpkg 组件校验下发/固件 OTA。
- **USB 协议**(`firmware/protocol.md`,1112 行,schema 7):JSON Lines `{"topic","payload"}` 三通道
  (UART0 4M / USB-Serial-JTAG / TinyUSB vendor bulk "P4BU"),能力协商靠 hello 大 JSON,逐 topic ACK+事务。
- **ESP32-P4 固件**(`firmware/main/` 实际代码 ≈25.9k 行,另 152k 行中文字库):自研 RGB565 软光栅渲染器
  (LVGL 只用字形)、H.264/MJPEG 形象动画解码(esp_h264 tinyh264 软解 + PPA 旋转缩放)、ES8311/ES7210 全双工音频
  (esp-sr 独立 AEC/NS/VAD)、声明式有界组件运行时(p4-bounded-runtime-v4,纯 C 解释,无 JS/Lua)、
  16 槽 SPIFFS 组件目录(双代际原子切换)、2×10MB 裸分区形象双槽、自研 A/B OTA(2.5MB 槽)。
  无网络栈(板上 C6 未用),`usbOnly:true`。

作者刻意分层:input_core/touch_core/game/assets/behavior/stats/view/widget_data/media_codec/协议 JSON 逻辑
均为零 IDF 依赖或纯 cJSON,主机 pytest 可测(firmware/tests ≈8k 行)。

## 2. 硬件对位(P4 哈基米机 vs Mosaico)

| 项 | HachimoDock (ESP32-P4) | ESP-Mosaico (esp32s31) | 结论 |
|---|---|---|---|
| CPU | 双核 RV 360/400MHz | 双核 RV FPU **320MHz** | 略降,软解帧率需实测 |
| 2D 加速 | PPA | **PPA 有**(CONFIG_SOC_PPA_SUPPORTED) | 直接复用 |
| JPEG 硬解 | 有 | **有**(CONFIG_SOC_JPEG_CODEC_SUPPORTED) | MJPEG 路线保底 |
| H.264 | esp_h264 软解 | esp_h264 **1.4.1 官方支持 s31**(软解) | 首选,需 spike 验帧率 |
| 屏幕 | 480×640 MIPI-DSI 竖屏(渲染 640×480 横屏+PPA 270° 旋转) | **CO5300 480×480 QSPI** 正方形原生 | 更简单:画布 480×480,**免旋转** |
| 触摸 | GT911(有,产品未依赖) | cst9220(BSP 实驱) | 输入主通道 |
| 实体键 | SW1/2/3 + 四向摇杆(12 手势) | **AI(GPIO7)/Boot(GPIO61)** 两键 + 触摸 + IMU/马达 | 手势集重映射 |
| 音频 | ES8311(+ES7210 可选麦) I2S 全双工 | **ES8311 同款** I2S 全双工,PA GPIO45 | 近乎直搬 |
| USB | UART0 4M(CH343)+ USB-Serial-JTAG + OTG HS(TinyUSB vendor) | **OTG HS(UTMI)+ USB-Serial-JTAG 双外设**(同 P4 形态) | 三通道可保真复刻 |
| 存储 | 32MB Flash(2×10MB 形象裸分区+SPIFFS) | 16MB Flash + **NAND /nand LittleFS ≈121.7MB** | 形象/组件改放 NAND |
| 网络 | 无(USB only) | WiFi6+BT+802.15.4 | 可选 Iris-TCP 通道 |
| AEC/NS/VAD | esp-sr 2.5.3 独立调用 | esp-sr **2.6.0 AFE 官方支持 s31** | IDF 6.1-dev 兼容性需 spike |

## 3. 总体架构决策

### 3.1 传输:推荐方案 A —— 宠物链路独占 HS USB,Iris 走 TCP

- 设备端应用用 **esp_tinyusb CDC-ACM**(可再加 vendor "P4BU" 复用客户端原生 USB 代码)承载 HachimoDock
  JSONL 协议;UART/USB-Serial-JTAG 通道按需裁剪。即插即用,保真原产品"USB 直连无需入网"。
- **Iris 配置为 TCP 传输**(`CONFIG_ESP_IRIS_TRANSPORT_TCP=y`,端口 19772,参考 qiaopi 已验证配置),
  USB 传输关闭 → 不与 TinyUSB 冲突。开发期用 Vibe Mode 配一次 WiFi,`mosaico.py iris --endpoint tcp://...`
  完成 status/logs/system-update。
- 这是 AGENTS.md 明文允许的产品例外("Normal apps do too unless the product requires it; document that
  exception and preserve Iris operations/recovery through another available transport"),需在项目 README 记录:
  - 恢复路径保留三重:AI 键物理进 Vibe Mode(Vibe Mode 固件自己占 HS USB)→ USB 更新;
    Boot 键 → ROM → `mosaico.py recover`;WiFi 在线 → Iris-TCP 远程更新。
- **备选方案 C**(若 spike 发现 USB 复用/枚举问题):Iris 保 USB,宠物链路走设备 TCP server + WiFi
  (perler_studio SoftAP 配网先例),PC 客户端补 TCP 传输。改动更大、偏离原 UX,仅作回退。

### 3.2 恢复契约(硬约束,全部保留)

- `mosaico_idf_project.cmake` before project()、`esp_mosaico_app_recovery` + `iris_ota_support_start()`、
  `CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y`。
- **删除 HachimoDock 自带 OTA**(pet_p4_ota.c 的 firmware/begin/chunk/commit 改为 NACK unsupported):
  Mosaico 上一切更新走 `mosaico.py iris system-update/app-update`;PC 客户端固件升级入口对
  esp-mosaico runtime 门控禁用。
- 分区:沿用 words_app 式布局,**不自定义分区**——形象/组件全部放 `/nand` LittleFS(121.7MB ≫ 2×10MB),
  好处:布局不变更 → 纯代码更新可走 app-update;NAND 掉电安全与原裸分区+代际方案等价。
  ota_0 14.6MB 足够(app 预计 <3MB)。

### 3.3 设备端 UI:保留 HachimoDock 自研渲染器,后台换成 raylib-fast 画布

- 4,563 行 renderer.c 的绘制逻辑(fill/round_rect/ellipse/文本/卡片/气泡)直搬,画布 640×480→**480×480**,
  删除 rotate_landscape_to_panel(方形屏免旋转);资产帧缩放优先 PPA(S31 有),CPU 回退保留。
- 呈现层二选一(Phase 1 定):
  a) **mosaico_raylib_fast 画布**(推荐):DrawTexture/DrawRectangle 承接原语,享受 `mosaico.py game sim`
     模拟器验证(工作区 sim-first 纪律),中文用 weiqi 式自建点阵(pet_p4_font_cn 16px 1bpp 可直接转表);
  b) 直连 BSP esp_lcd QSPI(性能兜底)。
- GSP 非强制("prefer"),本应用是类游戏自绘,按 raylib 路线合规(weiqi/astock 先例)。
- 应用骨架:`python mosaico.py game create hachimo-pet --template ...`(blank game,mosaico_game_app
  运行时自带显示/输入/Iris 启动/首帧健康验收)。

### 3.4 输入映射(12 手势 → Mosaico 手势集)

| 原 | Mosaico |
|---|---|
| SW1/SW2 短/长按 | AI 键/Boot 键 短/长按(BSP button,物理按键跨任务只投事件) |
| SW3/摇杆四向/中键/旋钮 | 触摸 tap/long_press/四向 swipe(HachimoDock touch_core 已有完整手势识别,cst9220 供坐标即可) |
| (新增可选) | IMU 抖一抖/马达反馈 |

- input/config 的 NVS 版本化配置模型照搬,手势白名单与动作白名单(page_back 逃生键等)保持同名,
  使 PC 端按键配置 UI 与 clawpkg buttons.json 校验**零改动**。
- 组件输入事件白名单 12 项维持原名(button.sw1…、joystick.up…、touch tap/long),PC 端契约不动。

### 3.5 形象资产管线

- 格式双轨:**首选 `p4-h264-v1`**(esp_h264 s31 软解,480×480@15fps ≤225 帧/动作,码率按 75% 像素重标定
  ~700kbps,单 family ≈7-8MB);**保底 `p4-mjpeg-v1`**(S31 硬件 JPEG 解码,renderer 原代码优先硬解路径,
  但体积 3-5×,需压质量/帧数)。hello `assetFormats` 能力协商,PC 端按 runtime 选转码档。
- PC 转码档新增 `m31-480x480-15fps-h264`(FFmpeg 参数仅改 W/H 与 pad 方形画布);双槽(slot0 内置出厂
  slot1 自定义)语义不变,存储从裸分区改为 /nand 目录 + packId(SHA-256)原子切换,asset/raw-chunk
  分块协议直搬(块级 FNV 校验/幂等重试/16KiB 擦除让 watchdog 的逻辑在 LittleFS 上简化为 append+fsync+rename)。
- 出厂西高地形象:用 PC 客户端对 terrier-clips 重新转码 480×480 档,首次安装时经 USB 下发到 slot0
  (或 Phase 3 后置)。

### 3.6 音频

- 采集/播放:I2S 全双工 + esp_codec_dev → 直接换 BSP audio 句柄(bsp_audio_codec_speaker_init /
  bsp_audio_codec_microphone_init,16k/mono/S16LE 一致),PTT 上行 `audio/chunk`(base64 20ms 帧 + FNV)
  与下行 `audio/play_chunk`(3s 环形缓冲 + play_flush 打断)协议不变。
- AEC/NS/VAD:esp-sr 2.6 独立 esp_aec/ns/vad(s31 官方支持)。**若 pinned IDF 构建不过 → 降级半双工**
  (原固件本就有 aec_fallback 能力位,PC 端能力门控自动适配),不阻塞主线。
- 音乐播放器(p4-media-v1 IMA ADPCM)与提示音(WAV cue/敲木鱼)纯 C 可搬,Phase 3+。

### 3.7 组件运行时(PetUI)

- miniapp.c(4,317 行)+ game.c(1,083 行)+ widget_data.c 纯逻辑直搬;SPIFFS 16 槽 → /nand 目录,
  双包代际 + 目录快照 + 启动回滚机制照搬;`p4-bounded-runtime-v4` 引擎名不变 → **PC 端 clawpkg 校验、
  petui Skill、组件中心 UI 全部零改动**(仅 Skill 文案里"480×640 三键摇杆"改"480×480 触摸+两键")。
- .p4s 精灵格式(RGB565+alpha)设备端照收。

## 4. PC 客户端策略:最小面 fork,不重写

52k 行 Rust/Node 全部保留(Agent 检测、ASR/TTS/LLM、clawpkg 校验、UI),patch 面收敛为:
1. `runtime_from_hello_payload`/`validate_hello_message` 增加 `"esp-mosaico"` runtime;
2. 形象转码档参数化(per-runtime profile,新增 480×480 方形档,MJPEG 档可选);
3. 固件 OTA 对 esp-mosaico 拒绝并提示走 `mosaico.py`(复用 linux runtime 的拒绝逻辑);
4. 串口枚举/CH343 节流对 CDC 端口跳过(64B 分片是 UART 桥专用);原生 USB "P4BU" 视设备侧实现取舍;
5. petui Skill 契约文案更新设备能力描述。
fork 放工作区外独立目录(如 `D:\hachimo-mosaico-pc`,参照 esp-mosaico-claw 同级仓先例),
遵循 HachimoDock 非商业许可证(个人非商业修改允许,保留版权与第三方声明)。

## 5. 需要删/改/保留清单(设备端)

| 动作 | 模块 | 说明 |
|---|---|---|
| 直搬(轻改) | protocol.c、miniapp.c、game.c、input_core.c、touch_core.c、behavior/stats/view/widget_data、media_codec、renderer.c 主体、assets 目录逻辑 | 传输回调/存储路径替换 |
| 重写 | lcd.c(DSI→BSP QSPI)、touch.c(GT911→cst9220 坐标源)、input.c GPIO/ADC(BSP button)、audio.c(BSP codec 句柄)、usb_native.c(→esp_tinyusb CDC/vendor)、raw_assets.c(裸分区→LittleFS) | 硬件适配层 |
| 删除 | pet_p4_ota.c 整块、PPA 旋转路径、esp_lvgl_adapter stub、微雪 BSP/st7701 组件、PlatformIO 构建 | 换 mosaico 工程体系 |
| 新增 | Iris 契约接线(mosaico_game_app 运行时自带)、/nand 资产布局、esp32s31 sdkconfig、cjson 显式依赖(IDF6.x 无内建)、PPA 缩放(S31 同 API) | |
| 保留复用 | firmware/tests pytest+C 主机测试(≈8k 行) | 纯逻辑零 IDF 依赖,直接进 host_tests |

## 6. 分期与工作量(单人+agent 协作估算)

- **Phase 0 spike(0.5-1.5 天,部分需真机)**:①esp_h264 s31 480×480@15fps 软解帧率+esp-sr 2.6 在
  pinned IDF 7b9cc1a 构建;②真机:app 模式 TinyUSB CDC 枚举 + Boot/AI 物理通道仍可用;③/nand 顺序读吞吐;
  ④确认 PSRAM 容量(开机日志)。
- **Phase 1 骨架(2-4 天)**:game create 建工程、Iris-TCP 契约接线、raylib-fast 画布 + 中文字模、
  两键+触摸输入、CDC JSONL 传输、hello/state/session 气泡 UI(静态占位动画)、sim + host 测试绿。
- **Phase 2 协议与组件(3-5 天)**:protocol 全量 topic、input/config、miniapp/game 组件运行时、widget
  安装/目录原子切换、widget/data;主机 pytest 全绿。
- **Phase 3 形象与音频(3-5 天)**:esp_h264/MJPEG 双解码路径、PPA 缩放、/nand 双槽资产、asset/raw-chunk
  同步、ES8311 PTT/播放、(可选)AEC;PC 端转码档联调。
- **Phase 4 PC 客户端 patch(1-3 天)**:runtime 门控、转码档、OTA 拒绝、Skill 文案;端到端联调
  (Claude Code hook → 状态气泡 → 语音注入)。
- **Phase 5 真机验收(0.5-1 天,待设备)**:mosaico-device-operations 流程,normal↔Vibe↔normal 证据链,
  HachimoDock 客户端自身 debug/screenshot 留证。

## 7. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| esp_h264/esp-sr 与 pinned IDF(6.1-dev)不兼容 | 中 | MJPEG 硬解保底(S31 有 JPEG codec);AEC 砍掉走半双工;必要时本地 patch 组件 |
| 320MHz 软解 + WiFi/USB 任务余量不足 | 中 | 降 fps/质量档;PPA 缩放替代 CPU;硬解 MJPEG 兜底;spike 先行 |
| USB 归属:app 占 OTG 后 mosaico.py 常规 USB 流程失效 | 中 | 方案 A 文档化例外 + TCP Iris;spike 验证三重恢复路径;不行退方案 C |
| Boot 键兼作输入与 ROM strap 冲突 | 低 | 仅复位时按住才进 ROM,运行期作普通键安全 |
| NAND 同步 20MB 包时长(CDC 实际吞吐未知) | 低 | raw-chunk 裸二进制 + HS bulk;实测后必要时上 vendor 端点 |
| 客户端 fork 维护(上游更新) | 低 | patch 面最小化+配置化;个人非商业使用合规 |
| PC 端 Windows 语音注入本就受限(MiMoCode/部分通道) | 无新增 | 沿用上游现状,不属移植范围 |

## 8. 验证缺口(本次调研未做)

纯源码静态分析 + 组件 registry 元数据;未编译 esp_h264/esp-sr 于 pinned IDF、未真机测 USB 归属与
解码帧率。以上均列入 Phase 0 spike。


---

## 9. 实施进度(2026-10-24 当日更新)

**已完成:**
- 工程落地 `projects/hachimo_pet`(blank game 模板):hd_pet/hd_cdc/hd_storage 接线层 +
  45 个 pet 源文件移植(含 renderer 480×480 手术与宿主隔离、protocol 身份/固件门控、
  /nand 存储路径、input/touch 注入式 API、audio BSP 化、OTA fail-closed 存根)。
- 模拟器全绿(40 帧,状态轮播 + 双会话卡;修复 cJSON dllexport 禁用 MinGW auto-export、
  clock_gettime 拉入 libwinpthread 两大宿主坑,均记录于 README/记忆)。
- 宿主单测全绿 4 套(input_core/game/conversation/widget_data)。
- PC fork `D:\hachimo-mosaico-pc`:runtime 枚举/is_p4_wire_runtime 门控/转码几何环境变量/
  OTA 拒绝/CH343 节流 CDC 豁免/petui Skill 文案;patch 面见其 README-MOSAICO.md。
- 工程 README(USB 归属例外三路径)与 build_hachimo.ps1。

**完成:** 设备构建全绿(2026-10-24 09:10,bin 3,318,608 字节)。构建迭代记录:
transport_config 补拷、on_start 签名/const、PRIx32、CONFIG_NAND_FLASH_ENABLE_BDL +
CONFIG_TINYUSB_CDC_ENABLED + CONFIG_LV_USE_TJPGD + CONFIG_ESP_H264_DUAL_TASK 需显式
进 sdkconfig.application.defaults(Iris USB 传输关闭后不再被连带带入;defaults 变更
必须删 build/sdkconfig 重投影)、lvgl 9.5 tjpgd 路径不变但要开 LV_USE_TJPGD、
-Wno-misleading-indentation、内嵌资产(wooden_fish + 3 个 cue WAV)EMBED 补齐、
.ninja_deps /D:/ 坑每次重配置后必撞。

**待办:** 设备构建全绿 → 真机验收(待设备插回):三重恢复路径、CDC 握手 hello、
PC fork 端到端(Claude Code hook→气泡)、/nand 组件与形象下发、音频 PTT。
