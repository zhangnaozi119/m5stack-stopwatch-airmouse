# Third-party notices

This project includes the following third-party software.

## HijelHID_BLEMouse

- Location: `lib/HijelHID_BLEMouse/`
- Upstream: https://github.com/HijelHub/HijelHID_BLEMouse
- Author: Hijel
- License: Apache License 2.0

Vendored and locally modified to:

- explicitly start the NimBLE GATT server (`_pServer->start()`), which is
  required on NimBLE-Arduino 2.5.1 because `NimBLEHIDDevice::startServices()`
  is a no-op there;
- create the input-report characteristic without the `READ_ENC` flag so hosts
  can subscribe without a completed pairing;
- gate the send task on connection rather than pairing;
- continue unencrypted instead of disconnecting when authentication is
  incomplete.

The original license text is retained in `lib/HijelHID_BLEMouse/LICENSE`.

## M5Unified / M5GFX / M5PM1 / M5IOE1

- Upstream: https://github.com/m5stack
- License: MIT

Pulled as PlatformIO dependencies; not vendored in this repository.

## NimBLE-Arduino

- Upstream: https://github.com/h2zero/NimBLE-Arduino
- License: Apache License 2.0

Pulled as a PlatformIO dependency; not vendored in this repository.
