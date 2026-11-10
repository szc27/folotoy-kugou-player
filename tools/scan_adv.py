"""扫一遍附近的 BLE 广播，把 AI Passport 的原始广告内容打出来。

用法: python scan_adv.py [秒数] [目标名字片段]
"""
import asyncio
import sys

from bleak import BleakScanner

ANCS = "7905f431-b5ce-4e99-a40f-4b1e122d00d0"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
NEEDLE = sys.argv[2] if len(sys.argv) > 2 else "passport"


async def main() -> None:
    found = {}

    def on_adv(device, adv):
        key = device.address
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        name = adv.local_name or device.name or ""
        interesting = NEEDLE in name.lower() or ANCS in uuids
        if key not in found or interesting:
            found[key] = (device, adv, interesting)

    scanner = BleakScanner(detection_callback=on_adv)
    await scanner.start()
    await asyncio.sleep(SECONDS)
    await scanner.stop()

    hits = [v for v in found.values() if v[2]]
    print(f"共听到 {len(found)} 个设备，其中匹配 ANCS/名字的 {len(hits)} 个\n")

    for device, adv, interesting in found.values():
        if not interesting:
            continue
        print(f"=== {device.address}  RSSI={adv.rssi} dBm")
        print(f"    name          : {adv.local_name!r}")
        print(f"    service_uuids : {adv.service_uuids}")
        print(f"    service_data  : { {k: v.hex() for k, v in (adv.service_data or {}).items()} }")
        print(f"    manufacturer  : { {k: v.hex() for k, v in (adv.manufacturer_data or {}).items()} }")
        print(f"    tx_power      : {adv.tx_power}")
        for i, payload in enumerate(adv.platform_data or []):
            raw = getattr(payload, "data", None)
            if raw:
                print(f"    raw[{i}]       : {raw.hex()}")

    if not hits:
        print("没有匹配的设备。下面列出全部名字，便于人工确认：")
        for device, adv, _ in found.values():
            print(f"  - {device.address}  name={adv.local_name!r}  uuids={adv.service_uuids}")


asyncio.run(main())
