# Security Notes

## Threat Model
- Provisioning over the setup AP is local and low-trust.
- The setup AP is intentionally open, so any nearby client can associate while provisioning is active.
- Improv provisioning shares the same trust boundary as physical serial access.
- The local LAN portal is more convenient, but it is only enabled after the device has already joined the trusted LAN and it is protected with per-device authentication.
- Wi-Fi passwords and printer access credentials are secrets and must never be logged or echoed back in responses.

## Provisioning Controls
- Provisioning starts only when configuration is missing or invalid, after explicit reset, or after repeated runtime Wi-Fi failures.
- The setup AP is named `BambuStatus-SETUP-XXXX` and serves a captive portal at `http://192.168.4.1/`.
- Captive DNS and HTTP services run only during provisioning.
- Provisioning is time-limited and cycles cleanly instead of remaining active forever.
- HTTP handlers enforce body-size limits, field bounds, no-store headers, and per-client rate limits.

## Local Portal Security
- After STA Wi-Fi is connected, the device advertises `http://bl-status-XXXX.local/` over mDNS with `_http._tcp`.
- The local portal is disabled whenever provisioning mode is active or STA Wi-Fi is disconnected.
- The local portal requires HTTP Basic Auth by default:
  - username `admin`
  - password derived from the configured printer `accessCode`
- Password derivation:
  - if `accessCode` contains at least 6 digits, use its last 6 digits
  - otherwise use its trailing 6 characters
- This removes the separate LAN-admin secret and ties portal access to the printer credential already required during provisioning.
- State-changing POST routes require both Basic Auth and a per-process CSRF token.
- Secret config fields are never rendered back to the browser. Empty secret inputs mean “keep current value”.

## Storage
- Runtime config is stored only in NVS namespace `cfg`.
- Config writes are atomic:
  1. write all fields with `provisioned=0`
  2. commit and re-open for verification
  3. mark `provisioned=1` only after the verified readback passes validation
- Factory reset clears only provisioning-related keys in `cfg`.

## Improv Serial
- The device implements Improv Wi-Fi over serial for Wi-Fi onboarding and installer integration.
- Improv uses the same provisioning draft and the same NVS save path as the captive portal.
- When only Wi-Fi is known, the device returns a local URL and keeps the setup portal active so the remaining printer settings can be completed on the LAN.

## Reset / Recovery
- The setup portal exposes `POST /reset`, but only while provisioning is active and only after explicit `ERASE` confirmation plus a per-session token.
- The local portal exposes authenticated reset and reboot actions.
- If the local portal login is unknown, the supported recovery path is to use the printer access code suffix or factory reset and reprovision.
- Repeated Wi-Fi failures trigger provisioning fallback without automatically erasing saved config.
- If NVS initialization fails because of exhausted or mismatched pages, the store attempts erase-and-reinit before giving up.
