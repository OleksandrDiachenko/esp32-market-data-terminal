# Setup

## Requirements
- ESP-IDF 6.0.1
- JC4880P443C_I_W board
- USB connection to the board

## Build
```sh
. /Users/siswood/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p <PORT> flash monitor
```
