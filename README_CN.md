# ESP-Mosaico ai_recorder

ESP-Mosaico 的会议录音机。它录下一场会议，把音频送到云上服务或你自己机器上的
收件箱，然后把纪要显示在板子上。

同一套 GSP 1.5.1 场景和同一套可移植的 C 界面既跑在 PC 上，也跑在板子上，
所以界面可以先在模拟器里验证，再上设备。

## 需要什么

- Python 3.10 或更新
- Git 2.28 或更新
- ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，目标 `esp32s31`。
  没有的话，`bootstrap.sh` 会替你克隆。
- 一块 ESP-Mosaico 开发板

## 取代码并编译

```sh
git clone --recurse-submodules https://github.com/esp-mosaico/esp-ai-recorder.git
cd esp-ai-recorder
./bootstrap.sh
```

`bootstrap.sh` 会初始化子模块、切到固定版本的 ESP-IDF，然后编译固件。
产物在 `build/ai_recorder.bin`。

已经备好那个 ESP-IDF 的话，把 `IDF_PATH` 指过去，脚本就直接用它，不再另克隆。

只想检查机器环境、不想编译：

```sh
python mosaico.py doctor
```

## 装到板子上

```sh
python mosaico.py recover                            # 空白板子，先做一次
python mosaico.py iris system-update --project .     # 新应用
python mosaico.py iris app-update --project .        # 只改代码
```

`system-update` 用于新应用，或者分区表、界面资源有变化。`app-update` 只用于
代码变化，分区表和资源不变。

看看板子现在是什么状态：

```sh
python mosaico.py iris list
python mosaico.py iris device-status
python mosaico.py iris logs
```

`iris` 还有别的：`screenshot` 存屏幕图像，`memory` 读内存和各任务的栈水位，
`rpc` 调应用的服务，`crash` 保留崩溃证据，`run` 在你观察设备时占住一个
Gateway。全部命令看 `python mosaico.py iris --help`。

## 收件箱

板子里没有写死收件箱的地址。开发者在自己机器上跑收件箱，板子在网络上找到它，
所以同一份固件对谁都一样。

**1. 填配置。** 把示例复制到程序旁边，把模型密钥填进去：

```sh
cp tools/inbox/config.example.yaml tools/inbox/inbox-config.yaml
$EDITOR tools/inbox/inbox-config.yaml
```

收件箱只读这一个文件。模型设置放在别处的话，用 `--config PATH` 指过去。

**2. 跑收件箱。**

```sh
python3 tools/inbox/inbox.py --root inbox-data --host 0.0.0.0 --port 8765
```

收件箱以 `_esp-inbox._tcp` 自报家门，板子会去找它。收件箱起来几秒之后，
设置页会显示找到了什么：从主页面往第三个页面滑——设置——看收件箱那一节。
点其中一个地址，它就成了板子要用的收件箱，重启和升级固件之后都还在。

**网络不允许这样的时候**——访客 Wi-Fi、开了客户端隔离的网络——可以在插着
板子的那条链路上把地址告诉板子：

```sh
python3 tools/set_inbox.py --project .
```

跟收件箱一起的是机器的名字，所以笔记本的地址变了，板子那边也不受影响。

### 谁写纪要

默认是收件箱自己调模型，用配置里 `model` 段写的那家接口。如果这台机器上装了
某个智能体程序，也可以让它写：`hermes`、`workbuddy`、`opencode`、`muse`
是按命令名在本机查找的。自己有别的智能体，在配置的 `agents:` 段里声明即可，
见 `tools/inbox/config.example.yaml`。

## 通道

通道是录音送到哪个服务器。私有可能是在你自己的机器上，小智是小智云。两者只能
开一个，关掉的那个就停着。这个选择会被记住，所以板子回来时还在原来的通道上。

收件箱回答三个路径：`POST /health`（链路活着吗）、`POST /v1/slices`（一次一段
录音切片，带校验和与会话头）、`GET /v1/notes`（它知道的每场录音的纪要，
按设备读的卡片格式）。转写、摘要和卡片写在 `--root` 下面，按设备和会话分开。

普通链路不需要墙钟，所以板子在没有外网路由的网络上照样能用——那种网络里
SNTP 不会有回音。

## 发布

`tools/build_release.sh` 构建所有对外共享的东西：镜像、System Update bundle，
以及用户自己刷板子用的包。构建产物里没有任何构建者的东西：没有地址，没有令牌，
没有名字。脚本末尾的检查会确认这一点。

```sh
sh tools/build_release.sh
```

收件箱的地址是在配置阶段读进来的，不是编译阶段。改了
`tools/inbox/inbox-config.yaml` 之后要重新走一遍配置：

```sh
rm -rf build   # 然后再编一次
```

## 目录

- `main`：只做设备启动和依赖装配。
- `components/app_hello`：可移植的界面行为和控制器状态。
- `components/app_display`：BSP 到 GSP 的显示与触摸策略。
- `components/app_recorder`：录音、闪存上的存储、上传。
- `components/app_onboard`：网络、配网、给主机用的服务。
- `components/app_clock`：板子的时钟。
- `components/app_hello/host`：桌面适配器，独立 CMake 工程，复用同一个
  控制器和同一份公开头文件。
- `components/app_hello/ui/main.json`：手写的场景；生成的绑定和 bundle 留在构建里。
- `tools/inbox`：收件箱和它对话的模型服务。
- `tools/quality`：格式规则和工程约定。
- `docs/`：收件箱地址和小智智能体的设计说明。

依赖方向是 `main -> app_hello -> ESP-GSP` 和
`main -> app_display -> BSP/ESP-GSP`。两个应用组件都不依赖 `main`，也不依赖彼此。
公开头文件放在 `include/`，私有状态留在实现文件里。

## 在 PC 上预览

```sh
python mosaico.py project sim --project . --interactive
```

模拟器跑的是和板子上同一份 C 代码，所以界面的改动可以先看再刷。

## 测试

```sh
make -C components/app_hello/host_tests test
make -C components/app_onboard/host_tests test
make -C components/app_recorder/host_tests test
cd tools/inbox && python3 -m unittest discover
```

## 许可

Apache-2.0，见 `LICENSE`。第三方组件保留各自的许可，见 `THIRD_PARTY_NOTICES.md`。
