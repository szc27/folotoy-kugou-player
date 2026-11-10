"""读一次设备的 GAP Device Name(0x2A00)，确认对端能不能拿到 "AI Passport"。

同时打印 Device Information 相关特征（若存在），用于排查 iPhone 侧显示 N/A。

用法: python read_name.py [MAC]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

TARGET = (sys.argv[1] if len(sys.argv) > 1 else "4C:11:AE:32:93:CE").upper()
DEVICE_NAME_CHR = "00002a00-0000-1000-8000-00805f9b34fb"
APPEARANCE_CHR = "00002a01-0000-1000-8000-00805f9b34fb"
BATT_LEVEL_CHR = "00002a19-0000-1000-8000-00805f9b34fb"


async def main() -> None:
    device = await BleakScanner.find_device_by_address(TARGET, timeout=15.0)
    if device is None:
        print("扫描不到设备")
        return
    print(f"广播里的名字: {device.name!r}")

    async with BleakClient(device, timeout=15.0) as client:
        print(f"已连接 mtu={client.mtu_size}")
        for svc in client.services:
            print(f"  {svc.uuid}  {svc.description}")
            for ch in svc.characteristics:
                print(f"      {ch.uuid}  {ch.description}  props={ch.properties}")
        for label, uuid in (("Device Name", DEVICE_NAME_CHR),
                            ("Appearance", APPEARANCE_CHR),
                            ("Battery Level", BATT_LEVEL_CHR)):
            try:
                raw = await client.read_gatt_char(uuid)
                if uuid == APPEARANCE_CHR and len(raw) >= 2:
                    print(f"  {label}: 0x{int.from_bytes(raw[:2], 'little'):04X}")
                else:
                    print(f"  {label}: {raw!r}")
            except Exception as exc:  # noqa: BLE001
                print(f"  {label}: 读取失败 {type(exc).__name__}: {exc}")


asyncio.run(main())
