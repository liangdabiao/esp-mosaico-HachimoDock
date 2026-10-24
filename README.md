# esp-mosaico-HachimoDock

[HachimoDock(哈基米机)](https://github.com/YizhengWw/HachimoDock)在
[ESP-Mosaico](https://github.com/esp-mosaico)(esp32s31,480×480 正方形 CO5300 触摸屏)
上的移植:把 PC 上 AI Agent(Claude Code / Codex / …)的工作状态变成桌面宠物的
表情、会话气泡与提醒。

> **许可证**:上游 HachimoDock 项目许可证(非商业 source-available)继续适用于
> 本仓库中移植的上游代码(`firmware/main/pet/`、`pc-client-fork/`);本项目仅为
> 个人非商业用途,商用需取得上游权利人书面授权。 cJSON 为 MIT,lvgl 为 MIT。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `firmware/` | 设备固件工程 `hachimo_pet`(构建产物 **3.32MB**,2026-10-24 全绿)。需放在 ESP-Mosaico 工作区 `projects/` 下构建(依赖工作区的 BSP / utils / raylib-lite-engine 子模块与固定 ESP-IDF,见其 README) |
| `pc-client-fork/` | PC 客户端最小面 fork(Tauri/Rust):新增 `esp-mosaico` runtime、480×480 形象转码档、OTA 门控拒绝、CDC 豁免 CH343 节流。patch 面见 `pc-client-fork/README-MOSAICO.md` |
| `docs/port-plan.md` | 移植方案与实施记录(硬件对位、架构决策、坑位全记录) |

## 架构速览

- **PC 链路**:应用自有 USB CDC(TinyUSB,VID `0x303A` PID `0x4AC1`)承载
  HachimoDock JSON Lines 协议(schema 7,topic 全兼容);协议/组件运行时/游戏引擎/
  中文字库等 ~2.6 万行上游代码原样移植。
- **恢复契约(ESP-Mosaico)**:Iris 走 TCP 传输,OTA writer 只在 Vibe Mode;
  固件更新一律 `python mosaico.py iris system-update`,设备对 `firmware/*` topic
  统一 NACK。三重恢复路径:WiFi Iris-TCP / AI 键(GPIO7)物理进 Vibe Mode /
  Boot 键(GPIO61)ROM 模式。
- **存储**:不自定义分区;组件与形象包放 `/nand`(LittleFS,~121MB)。
- **渲染**:上游自研 RGB565 渲染器改 480×480 方形画布(删 270° 旋转),
  零拷贝纹理上屏;PPA 缩放保留(esp32s31 与 P4 同款 2D 加速器)。
- **解码**:esp_h264 软解(esp32s31 官方支持)+ 硬件 JPEG 兜底。
- **输入**:AI/Boot 双实体键 + 触摸手势承载摇杆/SW3(事件名不变,PC 契约零改动)。

## 验证状态(2026-10-24)

- ✅ 设备固件构建全绿(bin 3,318,608 字节)
- ✅ 模拟器 40 帧全绿(状态轮播 + 会话卡,480×480 版式)
- ✅ 宿主逻辑单测 4 套全绿(input_core / game / conversation / widget_data)
- ✅ PC fork patch 就绪(Rust 侧未跑 cargo check,本机无 cargo)
- ⏳ 真机验收待设备(USB 归属三路径 / CDC 握手 / 形象与组件下发 / 音频 PTT)

## 快速开始

```sh
# 1) 把 firmware/ 放入 ESP-Mosaico 工作区(克隆后复制或 symlink)
#    <esp-mosaico-workspace>/projects/hachimo_pet
# 2) 构建(Windows PowerShell,IDF_PATH 指向固定 ESP-IDF)
powershell -ExecutionPolicy Bypass -File projects/hachimo_pet/build_hachimo.ps1
# 3) 模拟器(需 w64devkit/任意 gcc + Pillow)
python mosaico.py game sim --project projects/hachimo_pet --headless --frames 40
# 4) PC 客户端(进入 pc-client-fork/,与上游 HachimoDock 一致)
npm install && npm run tauri dev
```
