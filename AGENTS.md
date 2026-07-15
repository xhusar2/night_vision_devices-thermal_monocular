# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Firmware uses ESP-IDF 5.5.3. Build from `source_code/` with
  `idf.py set-target esp32c3` followed by `idf.py build`.
- Run the host-side request range test with
  `cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_control_validation.c -o /tmp/test-control && /tmp/test-control`
  from `source_code/`.
- `main/index.html` is embedded into the firmware by `main/CMakeLists.txt`; keep
  its control IDs aligned with the `/get` and `/set` JSON keys in `main/main.c`.
