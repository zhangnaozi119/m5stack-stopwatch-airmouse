"""M5 AirMouse 语音桥接助手。

订阅设备上的厂商自定义特征，收到命令后用 SendInput 合成 Ctrl+Win
（微信输入法等本地语音输入的长按组合键）。

用法：
    pip install bleak
    python tools/voice_bridge.py

打包成 exe：
    pip install pyinstaller
    pyinstaller --onefile --name M5VoiceBridge --collect-all bleak ^
        --collect-submodules winrt tools/voice_bridge.py

放在后台常驻即可；设备断开后会自动重连。
"""

import asyncio
import ctypes
import os
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "6d3578a0-1e2b-4c3d-9f4a-8b7c6d5e4f01"
CHAR_UUID = "6d3578a1-1e2b-4c3d-9f4a-8b7c6d5e4f01"
DEVICE_NAME = "M5 AirMouse"

CMD_VOICE_DOWN = 0x01
CMD_VOICE_UP = 0x00

VK_LCONTROL = 0xA2
VK_LWIN = 0x5B
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.c_ushort),
        ("wScan", ctypes.c_ushort),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long),
        ("dy", ctypes.c_long),
        ("mouseData", ctypes.c_ulong),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", ctypes.c_ulong),
        ("wParamL", ctypes.c_ushort),
        ("wParamH", ctypes.c_ushort),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("u", _INPUTUNION)]


_user32 = ctypes.windll.user32


def _send(vk: int, up: bool) -> None:
    inp = INPUT(type=INPUT_KEYBOARD)
    inp.u.ki = KEYBDINPUT(
        wVk=vk,
        wScan=0,
        dwFlags=KEYEVENTF_KEYUP if up else 0,
        time=0,
        dwExtraInfo=0,
    )
    _user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


_voice_held = False

_LOG_MAX_BYTES = 512 * 1024


def _log_path() -> Path:
    base = Path(sys.executable if getattr(sys, "frozen", False) else __file__)
    return base.parent / "M5VoiceBridge.log"


def say(msg: str) -> None:
    """无窗口模式没有 stdout，因此同时写日志文件便于事后排查。"""
    line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
    if sys.stdout is not None:
        try:
            print(line, flush=True)
        except Exception:
            pass
    try:
        path = _log_path()
        if path.exists() and path.stat().st_size > _LOG_MAX_BYTES:
            path.unlink()
        with open(path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def voice_down() -> None:
    global _voice_held
    if _voice_held:
        return
    _send(VK_LCONTROL, False)
    _send(VK_LWIN, False)
    _voice_held = True
    say("voice: Ctrl+Win down")


def voice_up() -> None:
    global _voice_held
    if not _voice_held:
        return
    _send(VK_LWIN, True)
    _send(VK_LCONTROL, True)
    _voice_held = False
    say("voice: Ctrl+Win up")


def on_notify(_sender, data: bytearray) -> None:
    if not data:
        return
    cmd = data[0]
    if cmd == CMD_VOICE_DOWN:
        voice_down()
    elif cmd == CMD_VOICE_UP:
        voice_up()


async def main() -> None:
    if sys.platform != "win32":
        say("This helper only works on Windows.")
        return

    # 打包成 exe 后输出默认是块缓冲的, 重定向到文件时看不到任何日志
    if sys.stdout is not None and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    say("helper started, looking for %s" % DEVICE_NAME)

    while True:
        try:
            device = await BleakScanner.find_device_by_filter(
                lambda d, adv: (d.name == DEVICE_NAME)
                or (SERVICE_UUID in (adv.service_uuids or [])),
                timeout=10.0,
            )
            if device is None:
                continue

            say("connecting to %s" % device.address)
            async with BleakClient(device) as client:
                await client.start_notify(CHAR_UUID, on_notify)
                say("connected, listening for voice commands")
                while client.is_connected:
                    await asyncio.sleep(1)
            say("disconnected")
        except Exception as exc:  # 蓝牙掉线、休眠唤醒等都会走到这里
            say("error: %s" % exc)
        finally:
            # 连接断开时必须释放按键，否则 Ctrl+Win 会卡在按下状态
            voice_up()
        await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        voice_up()
