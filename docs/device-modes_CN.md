# 设备模式

[English](device-modes.md)

**Vibe Mode** 是 ESP-Mosaico 内置的应用安装与设备维护模式，它跑 ESP-Iris，负责
应用更新和配网。**ROM Download Mode** 是芯片自带的下载程序，不跑 ESP-Iris。

| 模式 | 跑什么 | 什么时候用 |
| --- | --- | --- |
| 普通应用 | 你的应用；集成并启动后 ESP-Iris 也可用 | 运行、调试、验证应用 |
| Vibe Mode | flash 里 `vibe_mode` 分区中保留的固件，含 ESP-Iris | 安装或更新应用、维护设备 |
| ROM Download Mode | 芯片自带的下载程序，不含 ESP-Iris | 给空白板子做首次烧录，或恢复已经不可用的底层固件 |

设备连不上、握手失败，并不能说明它进了 ROM 模式或者固件坏了。先查连接和设备状态。

## 用哪个命令

| 情况 | 命令 |
| --- | --- |
| 空白板子，或者底层固件已经没了 | `python mosaico.py recover` |
| 新应用，或者分区表、界面资源有变化 | `python mosaico.py iris system-update --project .` |
| 只改代码，分区表和资源都不变 | `python mosaico.py iris app-update --project .` |
| 应用在跑，想观察设备 | `python mosaico.py iris run --project .` |

`recover` 会写入底层固件，然后验证 Vibe Mode。它可以从已连接的设备开始。
**只有工作流要求时才需要手动进 ROM 模式**——命令会告诉你，按下面说的做。

`system-update` 用于新应用。只有分区表和界面资源都不变时，`app-update` 才是安全的。

## 物理进入

两个不同的按钮，两条不同的启动路径。

- **ROM Download Mode —— Boot 键（GPIO61）：** 只有 `recover` 要求时才用。断电，
  **按住 Boot**（USB-C 口左边那个按钮），按住的同时上电，等芯片进入 ROM
  Download Mode 之后松开 Boot。然后告诉工具这一步已经做完，它会验证 ROM 连接
  并继续 `recover`。这个模式里 Vibe Mode 和 ESP-Iris 都不跑。
- **Vibe Mode —— AI 键（GPIO7）：** 在 bootloader 和保留固件都正常的情况下，
  按住 AI 上电，Vibe Mode 起来之后松开。bootloader 会为这一次启动选择
  `vibe_mode` 分区，不会改变 OTA 选择。按住 AI 只进这一次，松开 AI 之后重启
  仍然按原来的 OTA 选择走（如果那个选择是有效的）。

从没配过网的板子没有保留固件，AI 键对它没有用——那种情况要用 `recover`。

## 更新之后

命令报成功不等于事情做完了。在相信它之前：

1. 读设备的身份和状态：
   ```sh
   python mosaico.py iris device-status
   ```
2. 确认装上的正是你要的应用，而且它的行为符合预期。`system-update` 和
   `app-update` 会选择并启动装好的应用；写成功和设备可用不是一回事。
3. 留下证据：命令的输出。出了问题就用 `python mosaico.py iris crash` 保住
   崩溃证据。

## 更新不顺利时

- 板子根本不出现：查 USB 线和口。产品的供电和下载链路都走 USB。
- 出现了但拒绝更新：`python mosaico.py iris status` 会显示 Gateway 知道的设备
  状态，以及是不是有别的客户端占着它。
- 底层固件没了：`python mosaico.py recover`，它要求的话手动进 ROM 模式。
- 底层固件回来之后，先确认 Vibe Mode 能起来，再装应用。
