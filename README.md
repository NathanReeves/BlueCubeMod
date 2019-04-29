# BlueCubeMod

ESP32 based GameCube Controller Bluetooth conversion using BTstack

Mac/PC/PS4 supported (tested using Dolphin Emulator on Mac)

For Switch/RaspberryPi, use an 8Bitdo USB adapter

## Wiring:

- Connect pins 23 and 18 to GameCube controller's data pin (Red)

- Connect GND to controller's ground pin (Black)

## Build instructions:

- Get esp-idf	https://docs.espressif.com/projects/esp-idf/en/latest/get-started/

- Get BTstack - Dev  Branch (as of 4/29) https://github.com/bluekitchen/btstack/tree/master/port/esp32

- Clone BlueCubeMod

- If you haven’t flashed an ESP32 project before, you need the port name of ESP32 for the config file. If using unix system, to get the port name of a USB device run:

`ls /dev`

- Find your device on the list and copy it. It should look something like: /dev/cu.usbserial-DO01EXOV or /dev/cu.SLAB_USBtoUART

- cd into project folder and run:

`make menuconfig`

- Paste your port name into Serial Flasher Config -> Default Serial Port

- Compile and flash the program, run:

`make flash monitor`

