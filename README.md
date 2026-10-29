# QCARomePatchRAM

macOS x86_64 kext. It loads Qualcomm Rome 3.2 firmware into the Lite-On QCA6174A-5 Bluetooth USB device `04ca:3016` (from the Acer Swift 3 SF314-51), then releases the device to the system Bluetooth stack. It plays the same role for this chip that BrcmPatchRAM3 plays for Broadcom chips.

**Status: not tested on real hardware.** The host tests and the macOS CI build pass. Real loading and pairing have not been verified.

## How it works
1. Match `IOUSBHostDevice` `idVendor=0x04ca idProduct=0x3016` (IOProbeScore 4000).
2. Read the version over EP0 with vendor-IN `0x09`. Only `rom_version 0x302` is accepted. Any other version is rejected before anything is written.
3. Read the status over EP0 with vendor-IN `0x05`. If the rampatch bit (`0x80`) is missing, download `rampatch_usb_00000302.bin`. If the NVM bit (`0x40`) is missing, download `nvm_usb_00000302.bin`. Each download sends its header over EP0 vendor-OUT `0x01` and its body over bulk `0x02` in chunks of at most 4096 bytes.
4. Finish only if the final status has both bits set (`(s & 0xc0) == 0xc0`). Write the result to ioreg (`QCARomePatchRAM-*` properties), close the device, call `registerService`, and return `false` from `start` so the device is released.

The protocol follows Linux `drivers/bluetooth/btusb_main.c` (`btusb_setup_qca*`).

## Firmware
The firmware comes from linux-firmware commit `d0039fd797d7776d89f00c22c45754914a96706e`. `scripts/gen_firmware_header.py` checks each file's size and SHA-256 before generating the header in `build/gen/`. The firmware files themselves are not committed. The licences are in `firmware/`.

| file | bytes | sha256 |
|---|---|---|
| rampatch_usb_00000302.bin | 68644 | f0d15f6d…691e |
| nvm_usb_00000302.bin | 1998 | 9ee2cff5…88a5 |

## Installing with OpenCore (Sequoia)
- Kext load order: `Lilu.kext` → `BlueToolFixup.kext` → `QCARomePatchRAM.kext`.
- BlueToolFixup is required: since macOS 12, the system Bluetooth stack needs it to accept third-party USB HCI devices.
- Remove `IntelBluetoothFirmware` and `IntelBTPatcher`. They are for Intel chips.
- Boot args:
  - `-qcarpoff` disables the kext.
  - `-qcarpprobe` only reads the version and status; nothing is written.
- To check the result: `ioreg -l | grep QCARomePatchRAM`

## Building
```sh
make test                         # Linux/macOS host tests (ASan/UBSan)
./scripts/build_kext_macos.sh     # macOS + Xcode + MacKernelSDK (pinned 3f75008)
```
