# Hardware Validation

This directory stores manual hardware validation reports.

Use these reports to keep evidence for checks that cannot be covered by
host-side unit tests, such as flashing, boot logs, board-specific configuration,
and runtime behavior on the real ESP32-P4 board.

## Reports

- [ESP32-P4 startup diagnostics](esp32-p4-startup-diagnostics.md)
- [Application lifecycle](app-lifecycle-hardware-test.md)
- [Display and touch bring-up](display-touch-bring-up.md)
- [ESP-Hosted Wi-Fi link bring-up](wifi-hosted-link-bring-up.md)

## Report Template

````md
# Hardware validation: <feature>

## Environment

- Date:
- Board:
- Target:
- ESP-IDF:
- Port:

## Command

```sh
idf.py -p <port> flash monitor
```

## Expected result

-

## Observed logs

```text

```

## Result

Passed / Failed
````
