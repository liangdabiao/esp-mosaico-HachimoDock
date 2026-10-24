# Windows 实时对话播放修复（0.1.78）

## 问题证据

0.1.76 / 0.7.63-p4 的反馈日志显示：预先完整合成的 2.85 秒开场白播放了 16.04 秒；另一段 5.72 秒回复播放了 34.70 秒。播放启动约 200 ms 后设备持续报告 `playback_underrun`。同一设备在 Mac 正常，因此首先修复 Windows PC 到设备的播放供流，不改模型或设备固件。

0.1.77 的实测首帧 3200 字节耗时 45 ms，但下一帧出现 `chunk_rejected` / 解码字节数 0，设备只缓冲了 100 ms，并非缓冲溢出。256 字节合并写入未通过该设备的兼容性验证，0.1.78 撤回此项，保留异步读写。

## 修改边界

- Windows 端用 `FILE_FLAG_OVERLAPPED` 打开串口，每个读写克隆各自持有完成事件；与同步串口句柄不同，待完成的读操作不要求写操作排在其后。配置接口继续复用 serialport。
- COM 口、完成事件均不继承给子进程；取消只针对当前请求，并等待真正完成后才释放缓冲区和 OVERLAPPED。
- Windows 的 `audio/play_chunk` 恢复为 64 字节/400 μs 间隔；不以提高单次突发量换取吞吐。控制帧、素材/固件传输节奏和 Mac 分片逻辑不变。
- 实时音频仍不调用 FlushFileBuffers，不暂停接收麦克风，不禁用 AEC、VAD 或打断。
- `playback_tx` 日志记录前三帧、每十帧及所有 ≥80 ms 的写入耗时、字节数、序号和成功状态；`protocol_rejected` 记录白名单指令和拒绝码。失败停止会话且不累计为成功。只记录元数据，不记录声音、对话正文和 Key。
- 沿用 v1/v3 的 0.7.63-p4 固件，无需为该修复刷机。

## 验证与待验证

回归覆盖前端及源码契约测试、独立 Rust 写入测试（64 字节上限、完整/部分写入字节序、失败立即停止、无 flush、空输入）。Windows 包需通过 release 交叉编译及解包检查程序、运行时和两份固件；公开包还需检查没有误带服务凭据、企业 CA 或固定代理。这些不替代实际驱动测试。

仍需 Windows 实机确认：

1. 完全退出旧客户端，安装 0.1.78，再连接原设备；不清除用户配置。
2. 听完整开场白及约 20 秒的连续回复，检查句内是否断音。
3. 说话打断回复，再继续提问，检查停声和识别是否正常。
4. 退出实时对话后验证形象/组件传输；固件升级需另行授权，不作为本次听音测试的必要步骤。
5. 若仍异常，提供 `%LOCALAPPDATA%\com.petmanager.desktop\logs\realtime-chat.jsonl`。对照 `playback_tx` 耗时、设备 `bufferedMs` / `playback_underrun` 及打断事件判断剩余瓶颈。

Windows I/O 生命周期依据：[GetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)、[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。
