# TamaPoke 1.43 firmware

Download **TamaPoke-1.43-firmware-v1.22.zip** from this folder (click it, then the download button), unzip it, and follow the steps below.

```
TamaPoke v1.22 for the Waveshare ESP32-S3-Touch-AMOLED-1.43
Port of socquique/TamaPoke (CC BY-NC-SA). Source: github.com/imalaserpewpewpew/TamaPoke, branch port/amoled-143
Tested on a real board: screen (CO5300), touch, motion sensor, deep sleep + shake to wake.

FLASH IT (in the browser, nothing to install)
  1. Open https://espressif.github.io/esptool-js/ in Chrome or Edge.
  2. Plug the board into your PC with a USB-C data cable. Click Connect and pick the port.
  3. Flash Address 0x0, file TamaPoke.ino.merged.bin. Click Program.
  4. At 100%: Disconnect, then unplug and replug the USB (or press RST).
  This wipes the board, including a saved pet.

UPDATE LATER WITHOUT LOSING YOUR PET
  Same steps, but address 0x10000 and file TamaPoke.ino.bin.

IF IT WON'T CONNECT OR KEEPS DISCONNECTING (download mode)
  Unplug the USB. Hold the BOOT button, plug the USB back in, count to 2, let go.
  The board now stays connected whatever is on it; flash merged.bin at 0x0.
  On the board edge next to the pin header: left button = RST, right button = BOOT.
  Both are reachable with a toothpick through the open dome.

SPRITES (optional, recommended)
  microSD card up to 32 GB, formatted FAT32, with the "mons" folder from the
  TamaPoke repo (tools/sdcard/mons) copied to the top level of the card.
  Without it the game uses the small built-in sprite set.

POWER
  No battery needed for flashing; USB powers the board.
  On battery: after 10 minutes untouched it saves and goes to deep sleep.
  Shake or pick it up to wake it. On USB it stays on (charging).
  Battery: 3.7 V protected LiPo, 102540 (~1100 mAh), MX1.25 2-pin plug.
  CHECK POLARITY: red wire to + on the board. These plugs are not standard.

SERIAL MONITOR (optional, 115200 baud, e.g. googlechromelabs.github.io/serial-terminal)
  BAT?      battery mV, %, usb, imu=1 means shake-to-wake works
  SLEEPNOW  deep sleep now; shake to wake
  WIPE      factory reset (new game)

Separate files, if your tool wants them:
  0x0 TamaPoke.ino.bootloader.bin   0x8000 TamaPoke.ino.partitions.bin
  0xe000 boot_app0.bin              0x10000 TamaPoke.ino.bin
```
