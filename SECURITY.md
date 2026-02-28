# Security Notes

## Threat Model
- Provisioning over the setup AP is local and low-trust.
- The setup AP is intentionally open, so any nearby client can associate while provisioning is active.
- Improv provisioning shares the same trust boundary as physical serial access.
- Wi-Fi passwords and printer access credentials are secrets and must never be logged or echoed back in responses.

## Provisioning Controls
- Provisioning starts only when configuration is missing or invalid, after explicit reset, or after repeated runtime Wi-Fi failures.
- The setup AP is named `BambuStatus-SETUP-XXXX` and serves a captive portal at `http://192.168.4.1/`.
- Captive DNS and HTTP services run only during provisioning.
- Provisioning is time-limited and cycles cleanly instead of remaining active forever.
- HTTP handlers enforce body-size limits, field bounds, no-store headers, and per-client rate limits.

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
- Repeated Wi-Fi failures trigger provisioning fallback without automatically erasing saved config.
- If NVS initialization fails because of exhausted or mismatched pages, the store attempts erase-and-reinit before giving up.
