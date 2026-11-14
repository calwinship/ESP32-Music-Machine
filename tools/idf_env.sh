# Source this file (do not execute it) to get an ESP-IDF shell:  . tools/idf_env.sh
# Pins the IDF version and the Python that ESP-IDF's export.sh will detect.
# export.sh picks whatever `python3` is first on PATH; Homebrew's default is 3.14, but the IDF
# Python environment was created with 3.12, so put 3.12 first.
IDF_VERSION_DIR="${IDF_VERSION_DIR:-$HOME/esp/v5.3.2/esp-idf}"
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
. "$IDF_VERSION_DIR/export.sh" >/dev/null || { echo "idf_env.sh: export.sh failed (IDF at $IDF_VERSION_DIR)" >&2; return 1; }
