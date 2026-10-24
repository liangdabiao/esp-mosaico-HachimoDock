# HachimoDock PC 客户端 — ESP-Mosaico fork

本目录是 [HachimoDock](https://github.com/YizhengWw/HachimoDock) PC 客户端的最小面 fork,
用于连接 **ESP-Mosaico 移植版哈基米机**(esp32s31,480×480 正方形屏,应用自有 USB CDC 链路)。
上游许可证(HachimoDock 项目许可证,非商业)继续适用;本 fork 仅为个人非商业用途。

## 相对上游的 patch 面(全部已应用)

| 模块 | 修改 |
|---|---|
| `src-tauri/src/usb_serial.rs` | ① `canonical_runtime_id`/`runtime_from_hello_payload`/`validate_hello_message` 接受 `esp-mosaico` runtime(deviceModel 含 `esp32s31`/`esp-mosaico` 亦识别);② 新增 `is_p4_wire_runtime()`(esp-p4 与 esp-mosaico 同走 pet-usb-jsonl 全特性面);③ 形象转码几何可经环境变量 `HACHIMO_MOSAICO_GEOMETRY=1` 切为 480×480(`appearance_geometry()`);④ raw-chunk 大块写入仅对 CH34x UART 桥做 64 字节节流,esp-mosaico CDC 全尺寸直写 |
| `src-tauri/src/lib.rs` | 会话绑定/资产/组件/命令通道等特性门控改用 `is_p4_wire_runtime`;**固件升级入口对 esp-mosaico 显式拒绝**并提示走 `mosaico.py iris system-update` |
| `src-tauri/src/usb_serial/widget_transaction.rs` | 组件能力校验门控开放给 esp-mosaico |
| `skills/petui/SKILL.md` | 目标设备能力描述改为 480×480 触摸 + 两键 + 手势映射(事件名不变,契约零改动) |

设备侧协议端点(对应固件 `projects/hachimo_pet`):USB CDC VID `0x303A` PID `0x4AC1`,
JSON Lines 帧与全部 topic 与上游 schema 7 兼容;`firmware/*` topic 设备侧统一 NACK。

## 已知限制(fork TODO)

- `HACHIMO_MOSAICO_GEOMETRY` 是进程级环境变量:同一进程同时连 P4 与 Mosaico 板时,
  形象转码几何按全局变量走。单设备使用无影响;多设备并存需把几何下放到 per-connection
  (改 `appearance_geometry()` 消费点约 10 处)。
- 原生 USB(P4BU vendor bulk)通道对 esp-mosaico 未启用,串口 CDC 是唯一链路
  (hello 里 `nativeProtocol` 不再上报)。
- Rust 侧改动未跑 `cargo check`(本机无 cargo);模式替换均经唯一性断言,
  首次构建如遇字段名出入,按编译器提示微调即可。

## 构建

与上游一致:`npm install` → `npm run tauri dev`(或 `build`)。桥/Node 依赖随仓库自带,
Windows 构建需要 VS Build Tools 与 WebView2(见上游 `pc/README.md`)。
