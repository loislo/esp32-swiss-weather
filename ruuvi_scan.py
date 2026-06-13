#!/usr/bin/env python3
"""Scan for RuuviTag BLE advertisements and decode sensor data (data format 5)."""
import asyncio
import struct
import sys
from bleak import BleakScanner

RUUVI_MANUFACTURER_ID = 0x0499


def decode_df5(data: bytes):
    """Decode RuuviTag RAWv2 (data format 5) manufacturer payload."""
    if len(data) < 24 or data[0] != 5:
        return None
    temp = struct.unpack(">h", data[1:3])[0] * 0.005
    humidity = struct.unpack(">H", data[3:5])[0] * 0.0025
    pressure = struct.unpack(">H", data[5:7])[0] + 50000
    battery_raw = (data[13] << 3) | (data[14] >> 5)
    return {
        "temperature_c": round(temp, 2),
        "humidity_pct": round(humidity, 2),
        "pressure_hpa": round(pressure / 100.0, 2),
        "battery_mv": battery_raw + 1600,
    }


async def main(timeout=12.0):
    found = {}

    def callback(device, adv):
        md = adv.manufacturer_data.get(RUUVI_MANUFACTURER_ID)
        if md:
            decoded = decode_df5(md)
            if decoded:
                found[device.address] = (device, adv.rssi, decoded)

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()

    if not found:
        print("No RuuviTag found. Make sure Bluetooth is ON and the tag is in range.")
        sys.exit(1)

    for addr, (device, rssi, d) in found.items():
        name = device.name or "RuuviTag"
        print(f"{name}  [{addr}]  RSSI {rssi} dBm")
        print(f"  Temperature: {d['temperature_c']} °C")
        print(f"  Humidity:    {d['humidity_pct']} %")
        print(f"  Pressure:    {d['pressure_hpa']} hPa")
        print(f"  Battery:     {d['battery_mv']} mV")


if __name__ == "__main__":
    asyncio.run(main())
