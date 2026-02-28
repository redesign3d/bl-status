import os
from pathlib import Path
from SCons.Script import Import

Import("env")

# Default certificate paths (Mac)
DEFAULT_PATHS = [
    Path("/Applications/BambuStudio.app/Contents/Resources/cert/printer.cer"),
    Path.home() / "Applications/BambuStudio.app/Contents/Resources/cert/printer.cer",
    Path.home() / "Documents/printer.cer",
]
CERT_ENV_KEY = "BAMBU_CERT_PATH"
OUTPUT_HEADER = Path(env.subst("$PROJECT_INCLUDE_DIR")) / "generated_printer_cert.h"

cert_path_value = os.environ.get(CERT_ENV_KEY)
if cert_path_value:
    cert_path = Path(cert_path_value).expanduser()
else:
    cert_path = next((p for p in DEFAULT_PATHS if p.exists()), DEFAULT_PATHS[0])

if cert_path.exists():
    pem = cert_path.read_text().strip() + "\n"
    OUTPUT_HEADER.write_text(
        "// Auto-generated from {}\n"
        "#pragma once\n\n"
        "// NOLINTBEGIN\n"
        "static const char BAMBU_PRINTER_ROOT_CA[] PROGMEM = R\"pem(\n{}\n)pem\";\n"
        "// NOLINTEND\n".format(cert_path, pem)
    )
    print("Embed cert: using {} -> {}".format(cert_path, OUTPUT_HEADER))
else:
    # Ensure header is removed so __has_include fails and we fall back to insecure
    if OUTPUT_HEADER.exists():
        OUTPUT_HEADER.unlink()
    print("Embed cert: certificate not found at {} (set {} to override); using insecure TLS".format(cert_path, CERT_ENV_KEY))
