# Security Notes

## Threat Model
- Provisioning happens on a local SoftAP and is treated as low-trust shared media.
- Attackers on the same local RF network can attempt unauthorized provisioning or DoS.
- Device secrets (Wi-Fi password, printer access code) must never be logged or hardcoded.

## Provisioning Controls
- Device enters provisioning only when config is missing/invalid or after explicit reset/recovery conditions.
- Provisioning uses a dedicated SoftAP with:
  - Unique SSID derived from MAC suffix.
  - Random on-device generated WPA2 passphrase.
  - Short-lived pairing code required to unlock configuration routes.
- Pairing code and session state are RAM-only and expire automatically.
- Captive DNS + HTTP services are only active in provisioning mode.
- HTTP responses use no-store headers and avoid returning secret material.

## Storage
- Provisioned config is stored in NVS namespace `cfg`.
- Config writes use a two-step commit:
  1. Write all keys with `provisioned=0` and commit.
  2. Re-open and verify stored fields pass validation.
  3. Set `provisioned=1` only after verification succeeds.
- Factory reset clears only provisioning keys in `cfg`.

## Reset / Reprovision
- Authenticated provisioning endpoint `POST /reset` requires explicit confirmation token plus `ERASE`.
- Repeated Wi-Fi connectivity failures trigger fallback into provisioning without auto-erasing saved secrets.
