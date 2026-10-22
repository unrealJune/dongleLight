import argparse
import asyncio

from bleak import BleakClient


WRITE = "0000ffd9-0000-1000-8000-00805f9b34fb"
NOTIFY = "0000ffd4-0000-1000-8000-00805f9b34fb"


def channel(value):
    value = int(value)
    if not 0 <= value <= 255:
        raise argparse.ArgumentTypeError("RGB channels must be between 0 and 255")
    return value


async def control(address, rgb):
    async with BleakClient(address, timeout=20) as client:
        replies = asyncio.Queue()

        def notified(sender, data):
            if len(data) >= 9 and data[0] == 0x66:
                replies.put_nowait(bytes(data))

        await client.start_notify(NOTIFY, notified)

        async def status():
            while not replies.empty():
                replies.get_nowait()
            await client.write_gatt_char(WRITE, b"\xef\x01\x77", response=True)
            return await asyncio.wait_for(replies.get(), timeout=8)

        before = await status()
        print(f"Before: power={'on' if before[2] == 0x23 else 'off'}, "
              f"RGB={tuple(before[6:9])}")
        if rgb is None:
            return
        if before[2] != 0x23:
            raise RuntimeError("Strip is not currently on; refusing to change it")

        await client.write_gatt_char(
            WRITE, bytes([0x56, *rgb, 0x00, 0xF0, 0xAA]), response=True
        )
        await asyncio.sleep(0.5)
        after = await status()
        actual = tuple(after[6:9])
        if after[2] != 0x23 or actual != tuple(rgb):
            raise RuntimeError(f"Strip did not confirm requested color: {after.hex()}")
        print(f"Verified: power=on, RGB={actual}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Control a Triones-protocol BLE strip")
    parser.add_argument("--address", required=True)
    parser.add_argument("--rgb", nargs=3, type=channel, metavar=("R", "G", "B"))
    args = parser.parse_args()
    asyncio.run(control(args.address, args.rgb))
