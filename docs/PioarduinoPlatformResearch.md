# pioarduino 55.03.311 with VS Code PlatformIO

Research date: 2026-08-26

## Conclusion

The requested release and URL are valid. Release `55.03.311` is the pioarduino platform for Arduino-ESP32 3.3.11 on ESP-IDF 5.5.5, and its manifest requires PlatformIO Core 6.1.19 or newer. The normal PlatformIO IDE extension for VS Code can use it through a URL override; installing the pioarduino IDE fork is optional.

Use the URL as a plain INI value:

```ini
[env:ttgo-lora32-v1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = ttgo-lora32-v1
framework = arduino
```

Do not paste Markdown link syntax such as `[URL](URL)` into `platformio.ini`. PlatformIO defines `platform` as one package-specification value and accepts a remote HTTP(S) ZIP archive directly. It installs the platform and the dependencies declared by that platform, so a separate `platform_packages` override is not needed for an ordinary Arduino build. ([PlatformIO `platform` option](https://docs.platformio.org/en/latest/projectconf/sections/env/options/platform/platform.html), [remote ZIP package specification](https://docs.platformio.org/en/latest/core/userguide/pkg/cmd_install.html#remote-tar-or-zip-archive))

## Verified release facts

- The [55.03.311 release](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.311) is titled “Arduino Release v3.3.11 based on ESP-IDF v5.5.5”.
- The exact [platform-espressif32.zip asset](https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip) exists under that release.
- The tagged [`platform.json`](https://raw.githubusercontent.com/pioarduino/platform-espressif32/55.03.311/platform.json) declares platform version `55.03.311`, PlatformIO Core `>=6.1.19`, Arduino framework 3.3.11, and ESP-IDF 5.5.5.
- The pioarduino maintainers explicitly state that the standard PlatformIO IDE extension can use the community platform by putting its ZIP URL in `platformio.ini`. Their own extension mainly adds updated defaults and tooling convenience. ([pioarduino extension comparison](https://github.com/pioarduino/pioarduino-vscode-ide/blob/pioarduino/WHY_THIS_FORK.md))

## VS Code prerequisites and checks

1. In VS Code, open the PlatformIO Core CLI from the PlatformIO toolbar or Activity Bar and run:

   ```text
   pio --version
   ```

   The result must be `6.1.19` or newer. PlatformIO documents that the VS Code extension uses its built-in Core and exposes a dedicated Core CLI terminal. ([PlatformIO IDE for VS Code](https://docs.platformio.org/en/latest/integration/ide/vscode.html#platformio-core-cli))

2. If it is older, run the VS Code command `PlatformIO: Upgrade PlatformIO Core`, or run this from that PlatformIO Core CLI terminal:

   ```text
   pio upgrade
   ```

   Then restart VS Code and recheck the version. The command upgrades PlatformIO Core to the latest stable version. ([PlatformIO `pio upgrade`](https://docs.platformio.org/en/latest/core/userguide/cmd_upgrade.html), [official extension command declaration](https://github.com/platformio/platformio-vscode-ide/blob/develop/package.json))

3. Confirm this works in a normal system terminal:

   ```text
   git --version
   ```

   The pioarduino repository lists Git as a VS Code prerequisite. ([pioarduino installation README](https://github.com/pioarduino/platform-espressif32/tree/55.03.311#ide-preparation))

4. From the PlatformIO Core CLI in this project, install/resolve dependencies and build with:

   ```text
   pio pkg install
   pio run -e ttgo-lora32-v1
   ```

   `pio pkg install` reads `platform`, `platform_packages`, and `lib_deps` from `platformio.ini` and installs the declared platform plus its framework/toolchain dependencies. ([PlatformIO package install behavior](https://docs.platformio.org/en/latest/core/userguide/pkg/cmd_install.html#description))

## Migration and build caveats

- This platform moves projects from the official PlatformIO Espressif platform’s Arduino 2.x generation to Arduino-ESP32 3.3.11. Arduino-ESP32 3.x contains intentional breaking API and build-system changes; compile errors in application code or libraries must be migrated rather than solved by downgrading this platform. ([Espressif 2.x-to-3.0 migration guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html))
- Common changes include redesigned I2S, LEDC, RMT, SigmaDelta, and Timer APIs; changed BLE types; removed Hall-sensor APIs; and changed UART behavior/default pins. `WiFiClient`/`WiFiUDP::flush()` no longer clears received data (`clear()` does), and `WiFiServer::available()` is deprecated in favor of `accept()`. Check the same Espressif migration guide for the exact affected API.
- If a project replaces the core C/C++ compiler flags rather than merely adding normal PlatformIO `build_flags`, Espressif requires retaining `-MMD -c`.
- The release platform manifest already pins the matching Arduino framework, ESP-IDF package, toolchains, uploader, and build tools. Adding independent framework/toolchain overrides can create an unsupported mixture and is unnecessary unless a project has a concrete hybrid-framework requirement. ([55.03.311 `platform.json`](https://raw.githubusercontent.com/pioarduino/platform-espressif32/55.03.311/platform.json))
