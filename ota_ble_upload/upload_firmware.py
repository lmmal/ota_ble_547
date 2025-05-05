import requests
from bleak import BleakClient, BleakScanner
import asyncio
import json
import os

# --- Config ---
FLASK_SERVER_URL = "http://localhost:5000"
SAVE_PATH = "downloaded_firmware.bin"
SIG_PATH = "../signed/firmware.sig.json"
OTA_CHARACTERISTIC_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
DEVICE_NAME = "nimble"
CHUNK_SIZE = 180
FIRMWARE_PATH = SAVE_PATH

# OTA protocol message type definitions
MSG_TYPE_INIT = 0x01
MSG_TYPE_CHUNK = 0x02
MSG_TYPE_END = 0x03

def fetch_firmware_metadata():
    try:
        response = requests.get(f"{FLASK_SERVER_URL}/api/firmware")
        response.raise_for_status()
        return response.json()
    except Exception as e:
        print(f"[!] Failed to fetch firmware metadata: {e}")
        return None

def download_firmware(filename):
    try:
        url = f"{FLASK_SERVER_URL}/firmware/{filename}"
        response = requests.get(url)
        response.raise_for_status()
        with open(SAVE_PATH, "wb") as f:
            f.write(response.content)
        print(f"[✓] Firmware downloaded and saved as '{SAVE_PATH}'")
        return True
    except Exception as e:
        print(f"[!] Firmware download failed: {e}")
        return False

def transmit_to_esp32():
    asyncio.run(_transmit_firmware_ble())

async def _transmit_firmware_ble():
    print("[*] Scanning for ESP32 devices...")
    devices = await BleakScanner.discover()
    target_device = None

    for device in devices:
        if device.name and DEVICE_NAME in device.name:
            target_device = device
            break

    if not target_device:
        print(f"[!] No device found with name '{DEVICE_NAME}'")
        return

    print(f"[*] Found device: {target_device.name} ({target_device.address})")
    async with BleakClient(target_device) as client:
        if not client.is_connected:
            print("[!] Failed to connect to the device")
            return

        print("[*] Connected to the device, starting OTA upload...")

        # Load firmware binary
        with open(FIRMWARE_PATH, "rb") as f:
            firmware_data = f.read()
        total_size = len(firmware_data)
        print(f"[*] Total firmware size: {total_size} bytes")

        # Load SEMECS signature
        if not os.path.exists(SIG_PATH):
            print(f"[!] Signature file not found: {SIG_PATH}")
            return

        with open(SIG_PATH, "r") as f:
            sig = json.load(f)

        index = int(sig["index"])
        sj_bytes = bytes.fromhex(sig["sj"].lstrip("0x"))
        cj_bytes = bytes.fromhex(sig["cj"])
        eMj_bytes = bytes.fromhex(sig["eMj"])
        eMj_len = len(eMj_bytes)

        # Send INIT packet (metadata only)
        init_payload = (
            total_size.to_bytes(4, "big") +
            index.to_bytes(1, "big") +
            sj_bytes +
            cj_bytes +
            eMj_len.to_bytes(4, "big")
        )
        init_packet = bytes([MSG_TYPE_INIT]) + init_payload
        await client.write_gatt_char(OTA_CHARACTERISTIC_UUID, init_packet)
        print(f"[*] INIT packet sent ({len(init_packet)} bytes)")

        # Send SEMECS eMj in CHUNK packets
        print(f"[*] Sending eMj ({eMj_len} bytes)...")
        for i in range(0, eMj_len, CHUNK_SIZE):
            chunk = eMj_bytes[i:i+CHUNK_SIZE]
            packet = bytes([MSG_TYPE_CHUNK]) + chunk
            await client.write_gatt_char(OTA_CHARACTERISTIC_UUID, packet)
            print(f"[eMj-{i // CHUNK_SIZE}] Sent {len(chunk)} bytes")

        # Send firmware in CHUNK packets
        print(f"[*] Sending firmware ({total_size} bytes)...")
        for i in range(0, total_size, CHUNK_SIZE):
            chunk = firmware_data[i:i+CHUNK_SIZE]
            packet = bytes([MSG_TYPE_CHUNK]) + chunk
            await client.write_gatt_char(OTA_CHARACTERISTIC_UUID, packet)
            print(f"[fw-{i // CHUNK_SIZE}] Sent {len(chunk)} bytes")

        # Send END packet
        await client.write_gatt_char(OTA_CHARACTERISTIC_UUID, bytes([MSG_TYPE_END]))
        print("[✓] Firmware upload complete. Waiting for ESP32 to reboot...")

# --- Main ---
if __name__ == "__main__":
    meta = fetch_firmware_metadata()
    if not meta:
        exit(1)

    filename = meta.get("filename")
    if not filename:
        print("[!] Firmware metadata missing 'filename'")
        exit(1)

    print(f"[*] Found firmware: {filename} (version {meta.get('version')})")

    if download_firmware(filename):
        transmit_to_esp32()