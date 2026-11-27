#!/usr/bin/env python3
"""Build what the software page takes.

Two things are built here:

- the manual package: the application, the partition table and the resources
  of the interface, with a note that says where each file goes. The offsets
  are read from the bundle, so the note cannot drift away from the bundle.
- the Ideas release ZIP: the bundle, the pages and the presentation of the
  application. The platform validates it and gives a 6-digit install code
  for it.

    python3 tools/build_release.py

The Ideas package needs the host environment of the publication (jsonschema,
markdown-it-py, fonttools, freetype-py and libraqm). It is only built when
the publishing tools are in the repository; otherwise the script says that
the package for the software page is not built here and stops.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parent
BUILD = PROJECT / "build"
DIST = PROJECT / "dist"
BUNDLE = BUILD / "ai_recorder-system-update.irisfw"
VERSION = "1.0.0"
# The service of the private channel has a version of its own: it changes
# on its own, and the image of the application does not change with it.
INBOX_VERSION = "1.1.0"

PIECE_COMMANDS = """   esptool --chip esp32s31 --port <PORT> --flash-mode dio --flash-size 16MB \\
     --flash-freq 80m write_flash 0x8000 partition-table.bin \\
     0x9000 otadata_initial.bin 0x210000 ai_recorder-app.bin \\
     0xf00000 ui_apps.bin"""

NOTE = """AI Recorder {version} — files for a manual flash

These are the files of the application. The recommended way to install it is
the 6-digit install code that the software page gives you: type the code on the
board and the board installs itself. Use these files only when you flash by
yourself.

Two ways to flash

1. The complete image. The file ai_recorder-{version}-factory.bin holds
   everything a board needs, in one file, and one command writes it:

     esptool --chip esp32s31 --port <PORT> --flash-mode dio --flash-size 16MB \\
       --flash-freq 80m write_flash 0x0 ai_recorder-{version}-factory.bin

   It holds the bootloader, the partition table, the boot data, the retained
   Recovery, the application and the resources of the interface, each at the
   offset it belongs to. The board comes up the way a board from the factory
   comes up: into the retained Recovery first.

2. The pieces in this zip. They write only what belongs to the application, so
   the retained Recovery of the board is left as it is:

{commands}

Where each file goes (offsets in the 16 MB flash of the ESP32-S31):

{table}

The partition table is the one of the product (Recovery ABI 2, layout
mosaico-retained-test-2m-v2). It keeps the prefix that the retained Vibe Mode
of the board uses and puts the application at {application}.

The order matters: the partition table first, then the application, then the
resources of the interface.

The package carries nothing of the person who built it: no address, no token,
no name. The board finds the inbox on your own network by itself.
"""


def bundle_manifest() -> dict:
    with zipfile.ZipFile(BUNDLE) as archive:
        return json.loads(archive.read("manifest.json"))


def add(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
    """Put one file in a zip, with a fixed time.

    A file whose time is the time of the build is not the same file twice.
    The publishing package is built the same way upstream builds it, so the
    same source gives the same bytes and the same digest.
    """
    info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data)


def manual_package() -> Path:
    """The zip for a user who flashes the board themselves."""
    manifest = bundle_manifest()
    # The names the user sees. The bundle calls the application ota_0.bin
    # because that is its slot; a person expects a name that says what it is.
    names = {
        "partition-table.bin": "partition-table.bin",
        "ota_0.bin": "ai_recorder-app.bin",
        "ui_apps.bin": "ui_apps.bin",
    }
    target = DIST / f"ai_recorder-{VERSION}-manual.zip"
    DIST.mkdir(parents=True, exist_ok=True)

    rows = []
    with zipfile.ZipFile(BUNDLE) as bundle, \
            zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for component in manifest["components"]:
            source = component["file"]
            name = names.get(source, source)
            add(archive, name, bundle.read(source))
            rows.append(f"  {name:<24} 0x{component['target_offset']:08x}"
                        f"  {component['size']} bytes")
        ota_data = BUILD / "ota_data_initial.bin"
        if not ota_data.is_file():
            print(f"the file {ota_data} is missing", file=sys.stderr)
            return target
        add(archive, "otadata_initial.bin", ota_data.read_bytes())
        rows.append("  otadata_initial.bin      0x00009000  8192 bytes")
        add(
            archive,
            "README-flash.txt",
            NOTE.format(version=VERSION, table="\n".join(rows),
                        commands=PIECE_COMMANDS,
                        application=f"0x{manifest['components'][1]['target_offset']:08x}").encode("utf-8"),
        )
    print(f"the manual package is at {target} ({target.stat().st_size} bytes)")
    return target


def ideas_package() -> int:
    """The ZIP the software page takes.

    The publishing tools are pinned apart from the workspace, because the
    platform moved on: see tools/publish.py. They live on the branch that
    prepares the release, so a build of the application alone does not
    make this one.
    """
    publish = PROJECT / "tools" / "publish.py"
    if not publish.is_file():
        print("the publishing tools are not in this branch; the package for "
              "the software page is not built here")
        return 0
    command = [sys.executable, str(publish), "package"]
    result = subprocess.run(command, cwd=WORKSPACE)
    if result.returncode != 0:
        print("the Ideas package was not built", file=sys.stderr)
        return result.returncode
    return 0


INBOX_README = """私有通道的收件服务
====================

板子把录好的会推到这个服务，这个服务转写、写成纪要，再把纪要推回板子。
音频和纪要都留在你自己的机器上。

需要什么
--------

- Python 3.10 或更高版本（只用标准库，不用装包）
- 一个能转写和写纪要的模型接口。配置是 OpenAI 的格式，所以用哪家大模型
  都可以，MiniMax、千问、别的都一样。

把 config.example.yaml 复制成 config.yaml 放在 inbox.py 旁边，填好模型
那一段。或者指到别处的文件：--config <文件>。

    model:
      base_url: https://<你的接口地址>
      default: <写纪要的模型名>
      api_key: <你的密钥>
      asr: <转写用的模型名>

  base_url、default、api_key 是 OpenAI 的那三个。asr 是转写用的模型：
  不写就按 base_url 猜（千问用 qwen3-asr-flash，MiniMax 用它自己的
  asr-1.0）。转写和写纪要也可以分开放在两家：

        asr_base_url: https://<另一个接口地址>
        asr_api_key: <另一个密钥>

  用阿里云百炼的时候有四个坑，都在 config.example.yaml 里写全了，这里
  只提一句最常撞的：

  - 地址要用公共的 https://dashscope.aliyuncs.com/compatible-mode/v1。
    业务空间专属域名会把 qwen3-asr-flash 路由到「专用 ASR 任务」，报 400。
  - 模型 id 大小写敏感：qwen4-max 能用，Qwen4-Max 报 404。
  - 只有「千问3-ASR-Flash系列」支持 OpenAI 兼容方式（带 input_audio），
    qwen-audio-3.1-asr-flash 不支持。
  - qwen3-asr-flash 一次只收 5 分钟，板子一片录音最长 8 分钟，长会
    会转写失败。换成 qwen3.8-omni-flash 就没这个限制（一次收 3 小时），
    但它不是专用的转写模型，要配 asr_prompt 让它只吐字：

        asr: qwen3.8-omni-flash
        asr_prompt: "把这段录音逐字转写成文字，不要加任何说明或评论。"

  密钥只作为请求头发出去，不会写进转写、纪要或报错里。

选谁写纪要
----------

谁写纪要可以选，写纪要的模型也可以选。写在配置里：

    route:
      agent: direct
      model: qwen

agent 是谁写：

    direct      收件箱自己调模型接口
    hermes      这台机器上的 Hermes
    workbuddy   这台机器上的 WorkBuddy
    opencode    这台机器上的 OpenCode
    muse        这台机器上的 Muse

model 是哪个模型,十个短名:

    minimax  mimo  deepseek  glm  qwen
    doubao   kimi  baidu     spark  hunyuan

每个智能体自己知道这些短名背后是哪个模型，所以换一家大模型不用改
收件箱。agent 直接问模型时，model 要和 base_url 是一家：配置的是千问
就写 qwen，配置的是智谱就写 glm，写错了收件箱会直接说。

命令行上可以临时换，不用改文件：

    python3 inbox.py --root inbox-data --agent hermes --model qwen

每个智能体的模型 id 和 provider 都能改，写在 route 下面：

    route:
      agent: hermes
      model: qwen
      provider: openrouter
      model_id: qwen/qwen3.8-max-0902

收件箱不认识的智能体，可以在配置里声明。command 是一行参数，问题写在
{prompt} 里就放进去，没有就走上标准输入：

    agents:
      my-agent:
        command: "my-agent --model {model} --stdin"

这样任何"读问题、给答案"的命令行工具都能用，不必改收件箱。

怎么跑
------

    python3 inbox.py --root inbox-data --port 8765

--root 是它放录音和纪要的目录，--port 是它听端口的号。
配置放在 inbox.py 旁边的 config.yaml 就自动找到了，也可以用
--config <文件> 指到别处。

板子怎么找到它
--------------

板子的地址是运行时学到的，不是编译时写死的，所以固件对所有人都是同一份。

1. 板子在设置页上会找到它（它会把自己报在网络上），点一下确认它就是
   板子用的收件箱。板子记住这个地址，所以你的电脑换了 IP、或者板子重启了，
   地址都还在。
2. 网络不让组播的时候（访客 Wi-Fi、有客户端隔离的公司网络），把地址
   从刷写那条链路给板子：

       python3 mosaico.py iris rpc 0x1002 1 \
           --payload "http://your-laptop.local:8765" \
           --project .

3. 也可以写进 .env 重新编译固件，UPLOAD_ORIGIN=http://your-laptop.local:8765
   （改了 .env 要 rm -rf build 重新 configure，否则旧地址还在固件里）。

macOS 第一次运行会问要不要让 Python 接收传入连接，要选“允许”，不然板子
连不上。

板子不会自己推录音
------------------

录完要在录音页按上传，或者在文件面板里勾选几行再上传。不按，Mac 上就
不会有东西。

收件箱答三个路径
----------------

POST /health      板子问这条链路还活着吗
POST /v1/slices   一片录音，带校验和和这一场的头
GET  /v1/notes    它知道的每一场的纪要，板子显示的卡片格式

转写、纪要、卡片都写在 --root 下面，按设备和场次分目录。

可以加两个参数，把纪要同时发到 RainMaker：

    --rainmaker-token-file <文件>   --rainmaker-node-id <节点>

不想要就别加，纪要照样回到板子上。

注意
----

有些模型一次只收 5 分钟的音频。板子的一片录音最长 8 分钟，所以一场会
超过 5 分钟的会在这类模型上转写失败。这种失败会写在该场目录下的
transcript-error.txt 里，写着原因。

里面有什么
----------

    inbox.py        服务本身
    agent_route.py  谁写纪要，用哪个模型
    model_api.py    模型层：OpenAI 的格式，转写和写纪要
    asr.py          哪一家来转写
    writers.py      各种写法：模型自己、Hermes、WorkBuddy、OpenCode
    device_note.py  把纪要转成板子显示的卡片
    minimax.py      MiniMax 自己的转写接口
    rainmaker.py    可选：把纪要发到 RainMaker
    config.example.yaml  配置的样子，复制成 config.yaml 填好
"""

INBOX_FILES = ("inbox.py", "agent_route.py", "model_api.py", "asr.py", "writers.py",
               "device_note.py", "minimax.py", "rainmaker.py", "config.example.yaml")

# The complete image. One file that puts a board into the state the
# application needs: the product bootloader, the partition table of the
# application, the boot data that starts the retained Recovery, the
# retained Recovery itself, the application and the resources of its
# interface.
#
# The bootloader and the Recovery are the reviewed ones of the product. The
# application is proven on the board with that bootloader, and a system
# update does not replace it.
PREBUILT = (PROJECT.parent / "submodule" / "esp-mosaico-utils" /
            "esp-mosaico-vibe-mode" / "firmware" / "vibe-mode" / "prebuilt" / "recovery")

FACTORY_PARTS = (
    (0x2000, PREBUILT / "bootloader.bin", "the product bootloader"),
    (0x8000, BUILD / "partition_table" / "partition-table.bin", "the partition table"),
    (0x9000, PREBUILT / "ota_data_initial.bin", "the boot data"),
    (0x20000, PREBUILT / "vibe-mode.bin", "the retained Recovery"),
    (0x210000, BUILD / "ai_recorder.bin", "the application"),
    (0xF00000, BUILD / "ui_apps.bin", "the resources of the interface"),
)


def idf_path() -> Path:
    """The pinned ESP-IDF, unless the environment names a usable one.

    A stale IDF_PATH in the environment points at a checkout that is not
    there, so the path is checked before it is used.
    """
    pinned = Path.home() / "esp" / "esp-idf-mosaico-7b9cc1a"
    for candidate in (os.environ.get("IDF_PATH"), str(pinned)):
        if candidate and (Path(candidate) / "tools" / "idf.py").is_file():
            return Path(candidate)
    return pinned


def factory_image() -> Path:
    """The one file that flashes a board completely, and its own check."""
    import json

    sys.path.insert(0, str(WORKSPACE / "submodule" / "esp-mosaico-utils" / "mosaico-tools" / "tools"))
    from mosaico_cli.host import prepare_idf_environment

    prepared = prepare_idf_environment(idf_path())

    # The flashing arguments are the ones the build generated. They are not
    # ours to change.
    arguments = json.loads((BUILD / "flasher_args.json").read_text(encoding="utf-8"))
    settings = arguments["flash_settings"]

    target = DIST / f"ai_recorder-{VERSION}-factory.bin"
    DIST.mkdir(parents=True, exist_ok=True)
    command = [
        str(prepared.python), "-m", "esptool", "--chip", "esp32s31", "merge-bin",
        "--format", "raw", "--output", str(target),
        "--flash-mode", settings["flash_mode"],
        "--flash-size", settings["flash_size"],
        "--flash-freq", settings["flash_freq"],
    ]
    for offset, source, _ in FACTORY_PARTS:
        if not source.is_file():
            print(f"the file {source} is missing", file=sys.stderr)
            return target
        command.extend([hex(offset), str(source)])
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print("the complete image was not built:", result.stderr.strip()[-400:], file=sys.stderr)
        return target

    image = target.read_bytes()
    for offset, source, what in FACTORY_PARTS:
        data = source.read_bytes()
        if image[offset:offset + len(data)] != data:
            print(f"the complete image does not carry {what} at 0x{offset:08x}", file=sys.stderr)
            return target
    print(f"the complete image is at {target} ({target.stat().st_size} bytes); "
          f"every part of it was read back and matched")
    return target


def inbox_package() -> Path:
    """The service of the private channel, for the user who installs the app.

    The application alone cannot transcribe: the private channel needs a
    machine that listens. The listing tells the reader to run a script, so
    the script has to be something the reader can get.
    """
    source = PROJECT / "tools" / "inbox"
    target = DIST / f"ai_recorder-inbox-{INBOX_VERSION}.zip"
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in INBOX_FILES:
            path = source / name
            if not path.is_file():
                print(f"the file {path} is missing", file=sys.stderr)
                return target
            add(archive, name, path.read_bytes())
        add(archive, "README.txt", INBOX_README.encode("utf-8"))
    print(f"the inbox package is at {target} ({target.stat().st_size} bytes)")
    return target


def main() -> int:
    if not BUNDLE.is_file():
        print(f"the bundle {BUNDLE} is missing; build it first", file=sys.stderr)
        return 2
    manual_package()
    factory_image()
    inbox_package()
    return ideas_package()


if __name__ == "__main__":
    sys.exit(main())
