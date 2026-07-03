# Hardware validation: Application lifecycle

## Environment

- Date: 2026-06-17
- Board: JC4880P443C_I_W
- Target: ESP32-P4
- ESP-IDF: 6.0.1
- Port: /dev/tty.usbmodem101

## Command

```sh
idf.py -p /dev/tty.usbmodem101 flash monitor
```

## Expected result
- Startup diagnostics complete successfully.
- Application lifecycle starts.
- Application lifecycle started log is visible.
- No reset loop occurs.

## Observed logs

```text
I (1033) startup_diag: Startup diagnostics completed.
I (1033) app_lifecycle: Starting application lifecycle...
I (1043) app_lifecycle: Application lifecycle started.
I (1043) main_task: Returned from app_main()
```

## Result
Passed.