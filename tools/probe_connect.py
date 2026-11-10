"""从 Windows 连一次 AI Passport，验证设备的连接/配对/服务发现链路。

设备是 GATT Client（ANCS consumer），Windows 这边没有 ANCS 服务，
所以预期结果是：连上 → 设备主动发起配对 → 发现不到 ANCS →
设备自己断链并重新广播（fail_and_retry 生效）。日志能验证这条链路通不通。

用法: python probe_connect.py [MAC]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

TARGET = (sys.argv[1] if len(sys.argv) > 1 else "4C:11:AE:32:93:CE").upper()


async def main() -> None:
    print(f"扫描 {TARGET} …")
    device = await BleakScanner.find_device_by_address(TARGET, timeout=15.0)
    if device is None:
        print("没找到设备（可能没在广播）")
        return
    print(f"找到 {device.address} name={device.name!r}")

    disconnected = asyncio.Event()

    def on_disconnect(_client):
        print(">> 设备主动断开了连接")
        disconnected.set()

    try:
        async with BleakClient(device, timeout=20.0, disconnected_callback=on_disconnect) as client:
            print(f"已连接, mtu={client.mtu_size}")
            services = client.services
            for svc in services:
                print(f"  service {svc.uuid}  {svc.description}")
            print(f"服务数量 = {len(list(services))}（设备是 Client，自己的 GATT 库为空是正常的）")
            await asyncio.wait_for(disconnected.wait(), timeout=15.0)
    except asyncio.TimeoutError:
        print("等设备断链超时（可能还连着想等 ANCS）")
    except Exception as exc:  # noqa: BLE001
        print(f"连接异常: {type(exc).__name__}: {exc}")


asyncio.run(main())
