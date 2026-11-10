"""用 Windows WinRT 直接抓 BLE 广播的原始 AD 段，验证 AI Passport 的广播合规性。

区别对待 ADV_IND 与 SCAN_RSP，两者都要看：
Apple 要求 flags / 服务招揽 / TX Power 在 ADV_IND，名字通常在 SCAN_RSP。

用法: python dump_adv_raw.py [秒数] [名字片段]
"""
import sys
import time

from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementWatcher,
)

AD_NAMES = {
    0x01: "Flags",
    0x02: "16-bit Svc UUID (more)",
    0x03: "16-bit Svc UUID (all)",
    0x06: "128-bit Svc UUID (more)",
    0x07: "128-bit Svc UUID (all)",
    0x08: "Shortened Name",
    0x09: "Complete Name",
    0x0A: "TX Power",
    0x14: "16-bit Solicitation",
    0x15: "128-bit Solicitation",
    0x16: "Svc Data 16-bit",
    0x19: "Appearance",
    0xFF: "Manufacturer",
}
TYPE_NAMES = {0: "ADV_IND", 1: "ADV_DIRECT", 2: "ADV_SCAN_IND", 3: "ADV_NONCONN", 4: "SCAN_RSP"}

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
NEEDLE = (sys.argv[2] if len(sys.argv) > 2 else "passport").lower()

seen = {}


def on_received(watcher, event):
    adv = event.advertisement
    seen[(event.bluetooth_address, event.advertisement_type)] = (
        adv.local_name,
        event.raw_signal_strength_in_dbm,
        adv,
    )


def main() -> None:
    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = 1  # Active：主动索取 scan response
    watcher.add_received(on_received)
    watcher.start()
    print(f"主动扫描 {SECONDS:.0f} 秒…")
    time.sleep(SECONDS)
    watcher.stop()

    addrs = {
        addr
        for (addr, _t), (name, _r, _a) in seen.items()
        if name and NEEDLE in name.lower()
    }
    if not addrs:
        print(f"没听到名字含 {NEEDLE!r} 的设备；共 {len(seen)} 条广播")
        return

    for addr in sorted(addrs):
        print(f"\n########## {addr:012X}")
        for (a, atype), (name, rssi, adv) in sorted(seen.items()):
            if a != addr:
                continue
            print(f"--- {TYPE_NAMES.get(atype, atype)}  rssi={rssi} dBm  local_name={name!r}")
            if not adv.data_sections:
                print("    (无数据段)")
            for section in adv.data_sections:
                data = bytes(section.data) if section.data else b""
                label = AD_NAMES.get(section.data_type, "?")
                print(f"    AD 0x{section.data_type:02X} {label:<22}: {data.hex(' ')}")


main()
