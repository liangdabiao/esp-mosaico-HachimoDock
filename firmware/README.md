# hachimo_pet — HachimoDock(哈基米机)ESP-Mosaico 移植

把 [HachimoDock](https://github.com/YizhengWw/HachimoDock)(AI Agent 桌面宠物,P4 小屏 +
PC 客户端)移植到 ESP-Mosaico(esp32s31,480×480 正方形触摸屏)。上游许可证(非商业)
继续适用于 `main/pet/` 下移植的源码。

## 架构(方案全文见 `.agents/analysis/hachimodock-mosaico-port-plan.md`)

- **PC 链路**:应用自有 USB CDC(TinyUSB,VID `0x303A` PID `0x4AC1`,hd_cdc.c)承载
  HachimoDock JSON Lines 协议(schema 7,topic 全兼容);PC 端用最小面 fork
  `D:\hachimo-mosaico-pc`(见其 `README-MOSAICO.md`)。
- **恢复契约**:Iris 走 **TCP 传输**(WiFi),OTA writer 只在 Vibe Mode;固件更新一律
  `python mosaico.py iris system-update`,设备对 `firmware/*` topic 统一 NACK。
- **存储**:不自定义分区。组件/形象包放 `/nand/pet`(LittleFS,~121MB,
  perler_studio 已验证的挂载序列),ota_0 14MB 足够。
- **渲染**:HachimoDock 自研 RGB565 渲染器(4.5k 行)画布改 480×480,呈现层注册
  `Mosaico2DRegisterRGB565` 零拷贝纹理整幅上屏;PPA 缩放保留(s31 同款外设),
  270° 旋转删除(方形屏无需)。
- **输入**:AI(GPIO7)=SW1,Boot(GPIO61)=SW2;摇杆/SW3 由触摸手势承载
  (touch_core 的 tap/长按/四向滑动,事件名不变)。
- **音频**:BSP ES8311 全双工,16k/mono/S16LE 协议不变;AEC/NS(v1 砍掉)走原
  aec_fallback 半双工路径,hello 上报 audioAec=false。

## USB High-Speed 归属例外(产品例外,AGENTS.md 授权)

本应用占用 High-Speed USB 做宠物链路,因此 **ESP-Iris 配置为 TCP-only**
(`CONFIG_ESP_IRIS_TRANSPORT_USB` 关闭)。设备维护与恢复路径:

1. **WiFi 在线**:`python mosaico.py iris status/logs/system-update --endpoint tcp://<device-ip>:19772`
   (Vibe Mode 或正常应用均可;WiFi 经 Vibe Mode 配置一次)。
2. **AI 键物理进 Vibe Mode**:断电按住 AI(GPIO7)上电 → Vibe Mode 固件自行接管
   High-Speed USB → 常规 USB 工作流可用。
3. **Boot 键 ROM**:`python mosaico.py recover` 请求手动进 ROM 时,断电按住
   Boot(GPIO61)上电。

## 常用命令(工作区根目录)

```sh
# 模拟器(480×480,脚本化 agent 状态轮播,验版式/中文/会话卡)
python mosaico.py game sim --project projects/hachimo_pet
python mosaico.py game sim --project projects/hachimo_pet --headless --frames 40

# 宿主逻辑单测(input/game/conversation/widget_data,移植自上游 firmware/tests)
bash projects/hachimo_pet/host_tests/run_host_tests.sh

# 设备构建
powershell -ExecutionPolicy Bypass -File projects/hachimo_pet/build_hachimo.ps1

# 安装/更新(布局未变时 app-update 即可)
python mosaico.py iris system-update --project projects/hachimo_pet
```

## 目录

- `main/hd_pet.c`:运行时协调器(协议 worker/状态机/呈现),替代上游 pet_p4_main.c。
- `main/hd_cdc.c/.h`:USB CDC 宠物链路。
- `main/hd_storage.c/.h`:/nand 挂载 + 形象双槽后端(`pet_p4_raw_assets_*` 文件版)。
- `main/pet/`:上游移植层。`pet_p4_platform.h` 是宿主/设备分界;`host_only/` 是宿主
  shim(vendored cJSON、lv_font 字体引擎 shim、base64);设备专属解码管线
  (esp_h264/硬件 JPEG/PPA)在 `#ifndef MOSAICO_HOST_SIMULATION` 内。
- `main/pet/pet_p4_ota_stub.c`:firmware topic 的 fail-closed 存根(更新走 mosaico.py)。
- `host_tests/`:纯 C 逻辑回归。
- `game.sim.json`:模拟器源清单(排除 protocol/audio/touch/input 等设备模块)。

## 验证状态(2026-10-24)

- 模拟器:40 帧全绿,状态轮播(working/waiting_user/done/error/idle)+ 双会话卡渲染验证。
- 宿主单测:4 套全绿(input_core/game/conversation/widget_data)。
- 设备构建:**全绿(2026-10-24,bin 3.32MB < ota_0 14.6MB)**;factory 分区过小警告与
  qiaopi/weiqi 相同(factory 住 Vibe Mode 固件,应用走 ota_0,属预期)。
- 真机验收:**待设备**(USB 归属例外三路径、CDC 链路、/nand 双槽、音频、组件下发)。

## 上游差异备忘

- hello:`runtime="esp-mosaico"`、`deviceModel="esp32s31-esp-mosaico"`、display
  480×480、`firmwareOta=false`、无 `usbUartBaud`。
- 形象转码档:PC fork `HACHIMO_MOSAICO_GEOMETRY=1` → 480×480。
- 输入手势:触摸 swipe → knob.rotate_cw/ccw(左右)与 joystick.up/down(上下),
  名称与动作白名单不变(PC 端按键配置/组件 buttons.json 契约零改动)。
