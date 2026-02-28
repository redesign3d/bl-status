# Bambu Status

ESP32-based status display for a Bambu printer. Runtime configuration is stored in NVS and loaded at boot. When configuration is missing, reset, or repeatedly fails at runtime, the firmware drops into provisioning mode.

## Local Config Portal

- After the device joins the local Wi-Fi network, it exposes a LAN-only configuration portal at `http://bl-status-XXXX.local/`.
- `XXXX` is the last 4 uppercase hex characters of the device MAC and also appears in the on-device UI title as `Bambu Status #XXXX`.
- The local portal is only active while STA Wi-Fi is connected and the device has a valid local IP.
- The portal requires HTTP Basic Auth:
  - username: `admin`
  - password: derived from the configured printer access code
- The password uses the access-code suffix:
  - if the configured `accessCode` contains at least 6 digits, it uses the last 6 digits
  - otherwise it uses the trailing 6 characters of the configured `accessCode`
- If the printer `accessCode` is changed in the local portal, the LAN portal login changes with it after the save+reboot.
- If the hostname does not resolve on your network, use the device IP shown on the local portal landing page or in your router DHCP table.
- The local `/config` page also includes LED settings and applies LED changes immediately after atomic save.

## Provisioning Modes

### SoftAP + Captive Portal
- The device starts an open setup access point named `BambuStatus-SETUP-XXXX`.
- Connect to that Wi-Fi from a phone or laptop.
- If the captive portal does not open automatically, browse to [http://192.168.4.1/](http://192.168.4.1/).
- Submit the required fields:
  - `wifiSsid`
  - `wifiPassword`
  - `printerHost`
  - `printerPort`
  - `printerSerial`
  - `mqttUsername`
  - `accessCode`
  - `tlsInsecure`
- On success the device stores the config atomically in NVS, shuts down provisioning services, and reboots into normal operation.
- The same setup page includes LED settings:
  - global max brightness
  - per-state color, animation mode, baseline brightness, period, and flash duty
  - copy-from helpers and reset-to-defaults without wiping Wi-Fi credentials

Notes:
- The setup AP is intentionally open for maximum compatibility on screenless devices.
- Provisioning automatically times out and cycles if left idle.
- `wifiPassword` may be left blank for open Wi-Fi networks.
- The portal disables caching and never echoes secret values back after submission.
- All provisioning pages use the per-device header `Bambu Status #XXXX`.

### Improv Wi-Fi Over Serial
- Provisioning mode also exposes the [Improv Wi-Fi](https://www.improv-wifi.com/) serial protocol.
- A serial client or future web installer can:
  - discover the device state
  - request device info
  - scan nearby Wi-Fi networks
  - send Wi-Fi credentials over serial
- If the firmware already has the non-Wi-Fi printer fields in its draft config, a successful Improv Wi-Fi exchange can complete provisioning immediately.
- If printer settings are still missing, the device connects to Wi-Fi, returns a local URL, and keeps the HTTP setup portal available there so onboarding can finish over the LAN.
- Once Wi-Fi is connected, the preferred follow-up URL is `http://bl-status-XXXX.local/`.

## LED Behavior Settings

- LED behavior is stored separately in NVS namespace `led` with its own schema version and atomic save/verify path.
- Configurable printer states are derived from the current runtime status mapping:
  - `Unknown`
  - `Idle`
  - `Prepare`
  - `Printing`
  - `Paused`
  - `Finished`
  - `Error`
- Available animation modes:
  - `Solid`
  - `Flashing`
  - `Sine`
  - `Breathing`
- Parameters:
  - `Max Brightness`: global cap applied to every LED effect
  - `Baseline Brightness`: minimum floor before the global cap is applied
  - `Period (ms)`: full animation period
  - `Flash Duty %`: on-time ratio for flashing states
  - `Color`: RGB output color for the state
- Validation rules:
  - max brightness `1..255`
  - baseline brightness `0..255`, clamped to max brightness
  - flash period `50..10000 ms`
  - sine/breathe period `200..20000 ms`
  - flash duty `1..99`
- Saving LED settings does not reboot the device; the active LED renderer reloads the new config immediately.

## OLED UX
- When an OLED is present, provisioning mode shows:
  - setup SSID
  - setup IP
  - on-device QR code for `http://192.168.4.1/`
- The device remains fully provisionable without a screen.

## Reset / Re-provision
- Provisioning automatically starts when no valid config is found.
- Repeated Wi-Fi failures in normal operation force a return to provisioning without wiping secrets automatically.
- The setup portal also exposes a reset action that clears only provisioned config keys after explicit `ERASE` confirmation.
- The local portal exposes a factory reset action as well. If the portal login is unknown, use the printer access code suffix or factory reset and reprovision.

## Troubleshooting
- Captive portal did not open: connect to the setup AP and open [http://192.168.4.1/](http://192.168.4.1/) manually.
- Improv connected Wi-Fi but setup is not complete: open the returned local URL and finish entering the printer settings.
- `.local` hostname did not resolve: open the device IP address instead. mDNS availability depends on the client OS and network.
- Device re-entered provisioning after working previously: inspect Wi-Fi reachability and printer connectivity, then re-provision if needed.
