\# Serial2WiFi v1 — Prototype-First Implementation Plan



\## 1. Product and Hardware Contract



Serial2WiFi is a configurable, transparent, full-duplex serial↔TCP gateway over Wi-Fi.



```text

SER→NET:

Windows COM → CP2102 → UART RX → ESP32 → TCP server



NET→SER:

TCP server → ESP32 → UART TX → CP2102 → Windows COM

```



The ESP32 is always the outbound TCP client. It forwards bytes exactly as received and does not parse the machine protocol, generate ACK/NAK responses, frame messages, or translate line endings.



The product principle is:



> The fastest proof is local and requires no setup: plug it in and see the serial data. The phone/browser is then used for real configuration and deployment.



No Wi-Fi, phone, browser, or TCP server is required for local proof-of-capture. If the current baud is wrong, PRG short-taps allow immediate baud discovery at the machine.



\### Hardware definition



```text

Board:               TTGO LoRa32 V1

PlatformIO board:    ttgo-lora32-v1



CP2102 / UART0:

RX                   GPIO3

TX                   GPIO1

flow control         None



PRG:

GPIO0                active low



OLED:

controller           SSD1306

size                 128×64

SDA                  GPIO4

SCL                  GPIO15

RST                  GPIO16

address              0x3C



LoRa/SX1276:         unused

```



OLED initialization must pulse reset low for approximately 20 ms, return it high, call `Wire.begin(4, 15)`, and initialize the SSD1306 at `0x3C`.



\### PlatformIO build



```ini

\[env:ttgo-lora32-v1]

platform = espressif32

board = ttgo-lora32-v1

framework = arduino

board\_build.filesystem = littlefs



build\_flags =

&#x20;   -DCORE\_DEBUG\_LEVEL=0



lib\_deps =

&#x20;   adafruit/Adafruit SSD1306

&#x20;   adafruit/Adafruit GFX Library

&#x20;   adafruit/Adafruit BusIO

```



HTML, CSS, and JavaScript assets live under `data/`, are uploaded to LittleFS, and require no external assets, framework, package manager, or build step.



\### Factory defaults



```text

baud          9600

framing       8N1

display       text

Wi-Fi SSID    empty

Wi-Fi pass    empty

TCP host      empty

TCP port      0

```



The all-empty network configuration is valid and represents an unconfigured device.



Supported baud rates, in PRG cycle order:



```text

2400

4800

9600

19200

38400

57600

115200

230400

460800

921600

1000000

→ 2400

```



Supported framing values:



```text

8N1

8N2

8E1

8O1

7E1

7O1

```



Any other baud or framing value fails validation.



\## 2. Runtime Architecture and Local Display



\### Execution contexts



\- The HardwareSerial receive callback continuously drains UART RX into the SER→NET queue and offers tagged bytes to display history. It performs no Wi-Fi, TCP, HTTP, I²C, OLED, or NVS work.

\- `networkTask` exclusively owns the TCP client and services SER→NET and NET→SER in bounded alternating chunks.

\- `serialTxTask` exclusively drains NET→SER into `Serial.write()`.

\- The non-blocking Arduino loop services PRG, Wi-Fi/AP state, HTTP, and throttled OLED rendering.



This isolation is a correctness requirement at every supported baud, not a special architecture for 230400. UART RX must continue draining while Wi-Fi scans, TCP reconnects, HTTP requests execute, and the OLED refreshes.



\### UART initialization



```cpp

Serial.setRxBufferSize(8192);

Serial.begin(baud, framing, GPIO\_NUM\_3, GPIO\_NUM\_1, false);

Serial.setHwFlowCtrlMode(UART\_HW\_FLOWCTRL\_DISABLE);

Serial.onReceive(handleSerialReceive, false);

Serial.onReceiveError(handleSerialError);

```



UART0 must never carry firmware logging or diagnostics. `Serial.write()` is used only by `serialTxTask` for forwarded NET→SER bytes.



\### Queues and overflow policy



```text

UART RX driver buffer      8 KiB



SER→NET queue              32 KiB

full policy                discard oldest queued bytes

&#x20;                          count exact drops



NET→SER queue              32 KiB

full policy                stop reading TCP

&#x20;                          rely on TCP flow control



OLED display-history ring  approximately 1024 tagged bytes

full/contention policy     lose display history rather than

&#x20;                          delay either transport direction

```



Display history uses a fixed ring:



```cpp

struct DisplayByte {

&#x20;   uint8\_t value;

&#x20;   Direction direction;

};

```



It has two producers—UART RX and `networkTask`—and one renderer. The renderer copies a snapshot while holding a short lock and releases the lock before formatting or I²C work.



\### Network transport contract



After every TCP connection, enable `TCP\_NODELAY`.



`networkTask` must:



\- alternate between SER→NET transmission and NET→SER reception;

\- limit individual work chunks to approximately 1024 bytes;

\- handle partial TCP writes and reads;

\- retain unsent bytes after partial writes;

\- never hold queue locks during socket operations;

\- prevent either direction from starving the other.



Reconnect delays are:



```text

1 s → 2 s → 5 s → 10 s → 10 s thereafter

```



\### Directional counters



Each direction has runtime-only 64-bit counters:



```text

receivedBytes

forwardedBytes

droppedBytes

queuedBytes

```



Semantics:



```text

SER→NET received    bytes read from UART

SER→NET forwarded   bytes accepted by the local TCP stack



NET→SER received    bytes read from TCP

NET→SER forwarded   bytes accepted by the UART driver

```



Expose UART FIFO/buffer overflow, framing-error, and parity-error counters separately. Traffic totals and rate history are never persisted.



OLED transfer rates use `receivedBytes` deltas divided by the actual approximately one-second sample interval.



\### OLED modes and priority



Persisted modes are:



```text

text    Live serial text

hex     Live serial hex

stats   Operational statistics

off     Display off

```



The OLED is the local proof-of-capture and status display. Live Text is the factory default so an unconfigured device immediately demonstrates serial capture.



Priority:



```text

highest

&#x20;   factory-reset overlay

&#x20;   baud-change overlay

&#x20;   normal persisted display mode

```



Final integrated behavior:



```text

factory default display = text



unconfigured + no UART serial received yet

&#x20;   show full setup SSID/password/IP screen



after UART serial traffic has been received

&#x20;   render persisted text/hex/stats/off mode

```



`networkConfigured` never forces Live Text. If the user selects Hex, Statistics, or Off before completing networking, that selection remains authoritative.



Do not automatically change Text to Statistics after Wi-Fi or TCP configuration.



\### Live Text and Live Hex



```text

row 0       persistent status

rows 1–7    tagged serial history

```



Example:



```text

9600 8N1  AP

S> H|\\^\&|||...

<S <ACK>

S> P|1||...

```



Tags are always:



```text

S>    SER→NET

<S    NET→SER

```



Live Hex uses the same header and directional tags.



When the setup AP is active, provisioning integration may periodically alternate the header between operating status and the setup password without replacing the seven payload rows:



```text

9600 8N1 SETUP

```



and:



```text

P:7K4M29QFABCDEFG

```



Use approximately four seconds for the normal header and two seconds for the credential header.



Statistics may similarly replace only its top status row briefly.



Off means off during normal operation. Starting setup/recovery or performing a PRG action may temporarily wake the display; afterward it returns to Off.



\### Statistics display



Statistics uses unambiguous direction labels:



```text

WiFi OK   TCP OK

S>N   4.2 kB/s

&#x20;     18.4 MB

N>S   0.3 kB/s

&#x20;      1.8 MB

DROP      0/0

9600 8N1

```



It includes:



\- current SER→NET received rate;

\- current NET→SER received rate;

\- cumulative received bytes in each direction since boot;

\- dropped bytes in each direction;

\- Wi-Fi state;

\- TCP state;

\- current baud and framing.



\### PRG state machine



```text

BUTTON\_DEBOUNCE\_MS       30

SHORT\_TAP\_MAX\_MS         approximately 1000

RESET\_WARNING\_START\_MS   approximately 5000

FACTORY\_RESET\_MS         10000

BAUD\_OVERLAY\_MS          approximately 1000

```



Service the state machine frequently in the non-blocking main loop so ordinary taps are not missed. Do not add a dedicated task unless later blocking loop work requires it.



Interpret actions on release:



```text

press

&#x20; |

&#x20; +-- release before \~1 second

&#x20; |      advance baud once

&#x20; |

&#x20; +-- release from \~1 second through <10 seconds

&#x20; |      no persistent action

&#x20; |

&#x20; +-- still held from \~5 seconds

&#x20; |      show factory-reset warning/countdown

&#x20; |

&#x20; +-- reaches 10 seconds

&#x20;        attempt factory reset

&#x20;        wait for GPIO0 release

&#x20;        reboot only after release and only if reset succeeded

```



A successful short tap shows, even from Off:



```text

&#x20;       BAUD

&#x20;      115200

```



It then restores the persisted display mode.



PRG has no menus or additional gestures.



\### Factory reset



The running firmware clears only the `s2w` user-configuration namespace.



Clear:



```text

Wi-Fi SSID/password/security

TCP host/port

baud/framing

display preference

```



Restore:



```text

9600

8N1

text

unconfigured network state

```



Preserve:



```text

s2id device identity

Serial2WiFi-XXXX name

unique setup-AP password

firmware and bootloader

partition table

LittleFS

CP2102/programming support

```



Result handling:



```text

clear succeeds

&#x20;   show RESET COMPLETE

&#x20;   wait for PRG release

&#x20;   reboot



clear fails

&#x20;   show RESET FAILED

&#x20;   do not claim factory state

&#x20;   do not reboot automatically

```



Never restart while GPIO0 is low. Never erase the whole flash.



Hardware recovery remains:



```text

1\. Hold PRG

2\. Press and release RST

3\. Release PRG

4\. Upload with PlatformIO

```



\## 3. Configuration, Wi-Fi, HTTP, and Browser Contract



\### Single serialized configuration authority



One configuration module owns validation, persistence, in-memory state, and coordinated runtime transitions:



```text

commitConfiguration(candidate)

&#x20;   acquire configuration serialization lock

&#x20;   validate the complete candidate

&#x20;   compute one combined transition plan

&#x20;   persist the complete candidate once

&#x20;   update in-memory configuration once

&#x20;   initiate only the required runtime differences

&#x20;   release configuration serialization lock

```



`commitConfiguration()` must never wait for Wi-Fi association or TCP connection while holding its lock. It initiates those asynchronous state-machine transitions and returns their initial state.



Invariant:



> `commitConfiguration()` is serialized. Two mutations can never validate, persist, publish, or apply configuration concurrently; PRG and HTTP cannot independently mutate configuration at the same time.



Use one FreeRTOS mutex or an equivalent single-owner configuration command path. All callers must pass through it.



Internal runtime operations are:



```text

reconfigureSerial(...)

reconfigureServer(...)

reconfigureWifi(...)

setDisplayMode(...)

```



`reconfigureSerial()` is the only authority permitted to change UART hardware, but it does not independently persist configuration.



PRG baud cycling performs, inside the serialized path:



```text

candidate = currentConfig

candidate.baud = nextSupportedBaud

commitConfiguration(candidate)

```



Browser Save submits its complete candidate through the same authority.



Persist one schema-versioned, fixed-length/range-validated configuration blob in `s2w`. Do not add an application CRC.



If persistence fails:



\- leave runtime and in-memory configuration untouched;

\- report the save failure;

\- for PRG, show `SAVE FAILED` rather than claiming a baud change.



After persistence succeeds, the saved candidate is authoritative. Failure to connect to its Wi-Fi or TCP server does not roll configuration back.



\### Valid unconfigured and configured states



The complete configuration validator must accept the factory/unconfigured network state:



```text

SSID           empty

Wi-Fi password empty

TCP host       empty

TCP port       0

```



Rules:



```text

Wi-Fi unconfigured:

&#x20;   SSID must be empty

&#x20;   password must be empty

&#x20;   security state is unset



Wi-Fi configured:

&#x20;   SSID must be non-empty

&#x20;   security is open or secured



TCP endpoint unconfigured:

&#x20;   host must be empty

&#x20;   port must be 0



TCP endpoint configured:

&#x20;   host must be non-empty

&#x20;   port must be 1..65535

```



Wi-Fi and TCP configuration are independent. A TCP endpoint may be saved before Wi-Fi is configured, and Wi-Fi may be configured before a TCP endpoint is supplied.



This ensures PRG can persist a baud change on a factory-reset device without inventing network values.



\### Coordinated transition boundaries



For a Save changing several fields, calculate all differences first and execute one coordinated boundary. Do not repeatedly close TCP or clear queues for individual fields.



```text

Same-server TCP reconnect:

&#x20;   retain SER→NET queue

&#x20;   continue draining NET→SER queue to UART



Wi-Fi-only change:

&#x20;   retain both transport queues

&#x20;   reconnect STA

&#x20;   reconnect the same TCP endpoint



Server host/port change:

&#x20;   close old TCP connection

&#x20;   count and clear both transport queues

&#x20;   connect to the new endpoint

&#x20;   retain OLED history



Baud or framing change:

&#x20;   count and clear both transport queues

&#x20;   clear UART pending input

&#x20;   clear OLED history decoded using old settings

&#x20;   close TCP connection

&#x20;   restart UART

&#x20;   reconnect TCP



Display-only change:

&#x20;   no transport effect

```



When serial, server, and Wi-Fi settings change together, perform the serial transport boundary once, then apply the new Wi-Fi and final server endpoint without duplicate queue clears or socket teardown.



A failed UART restart is a device fault: show `SERIAL ERROR`, stop forwarding through the invalid UART state, retain the saved desired configuration, and retry it on reboot. Wi-Fi and TCP connection failures remain visible retrying states.



\### Application-owned Wi-Fi lifecycle



Initialize Wi-Fi with:



```cpp

WiFi.persistent(false);

WiFi.setAutoReconnect(false);

```



The application state machine is the sole authority for STA reconnect attempts, retry timing, AP recovery, and the transition back from AP+STA to STA-only operation. Framework automatic reconnection must not run a competing lifecycle.



\### Setup identity and password



On first boot without `s2id`:



\- derive `Serial2WiFi-XXXX` from a stable MAC suffix;

\- initialize Wi-Fi/RF randomness;

\- use ESP32 hardware randomness to generate one 16-character WPA2 password;

\- use an ambiguity-free alphabet such as `23456789ABCDEFGHJKLMNPQRSTUVWXYZ`;

\- persist the password only in `s2id`.



Reuse this password for the device’s lifetime unless identity storage is deliberately erased. Factory reset preserves it.



The password is displayed locally but never returned by HTTP.



\### Setup AP lifecycle



```text

boot

&#x20;   start setup AP



unconfigured

&#x20;   keep AP active indefinitely



STA connected

&#x20;   keep AP for 60 seconds

&#x20;   then close AP



STA unavailable for 30 seconds

&#x20;   reopen AP while continuing application-controlled STA retries



STA recovers

&#x20;   keep AP for 60 seconds

&#x20;   then close AP

```



Rebooting provides a 60-second reconfiguration window on a healthy device.



```text

SSID       Serial2WiFi-XXXX

security   WPA2 with unique device password

address    http://192.168.4.1

mDNS       http://serial2wifi-xxxx.local

```



Automatic captive-page opening is best effort. `http://192.168.4.1` is the guaranteed setup path.



Use HTTP, not HTTPS, in v1.



\### AP/LAN authorization boundary



Determine request authority from the accepted socket’s local interface/address, never from `Host`.



```text

Setup-AP interface:

&#x20;   configuration page

&#x20;   Wi-Fi scan

&#x20;   GET configuration

&#x20;   POST configuration

&#x20;   status



STA/LAN interface:

&#x20;   status only

```



Configuration or scan mutations received through STA/LAN return `403`.



Generate a new CSRF token on every boot. Expose it only through the AP-side configuration response and require it in `X-CSRF-Token` for AP-side POST requests.



Do not return saved Wi-Fi passwords or the setup-AP password through any API.



\### HTTP API



Available through AP and LAN:



```text

GET  /api/status

```



AP only:



```text

GET  /api/config

POST /api/config

POST /api/wifi/scan

GET  /api/wifi/scan

```



`GET /api/status` includes non-secret runtime state and:



```json

{

&#x20; "configurationAllowed": true

}

```



It may include Wi-Fi/TCP state, IP address, baud/framing, display mode, directional counters, queue sizes, drops, UART errors, and retry state.



`GET /api/config` returns non-secret editable settings, `wifiSecurity`, whether a station password is already stored, and the per-boot CSRF token. It never returns either password.



`POST /api/config` validates the complete candidate and returns field-specific `400` errors.



Minimum validation:



```text

SSID          empty or maximum 32 bytes

Wi-Fi pass    empty or maximum 64 bytes

security      unset | open | secured

host          empty or maximum 253 bytes

port          0 when host empty; otherwise 1..65535

baud          supported enum

framing       supported enum

display       text | hex | stats | off

```



Password rules:



```text

unconfigured Wi-Fi

&#x20;   SSID empty

&#x20;   security unset

&#x20;   stored password empty



same secured SSID + blank submitted password

&#x20;   retain stored password



different secured SSID + blank submitted password

&#x20;   validation error



open SSID selected

&#x20;   explicitly clear stored password

```



\### Browser behavior



Use the same semantic HTML/CSS/vanilla-JavaScript assets on both interfaces:



```text

Setup AP:

&#x20;   compact device-status header

&#x20;   editable Wi-Fi and TCP configuration

&#x20;   remaining settings and operational status



Normal LAN:

&#x20;   status-only presentation

```



Do not render disabled editable controls on LAN. Select the presentation from `configurationAllowed`.



On the setup AP, the Wi-Fi section must appear immediately after the compact device-status header. Do not place a dashboard, traffic counters, or a large status block before the first-time user’s primary task.



The editable page includes:



```text

Wi-Fi scan/network/password/security

TCP host/port

baud

framing

display:

&#x20;   Live text

&#x20;   Live hex

&#x20;   Statistics

&#x20;   Off

```



Wi-Fi scan states must be explicit:



```text

Scanning…

network rows

No networks found

Scan failed + retry action

```



Wi-Fi selection behavior:



\- scan asynchronously when the page opens;

\- show touchable network rows ordered strongest-first;

\- indicate selection using more than color;

\- selecting a secured row immediately reveals and enables the password field;

\- selecting an open row hides and disables the password field;

\- provide a simple `Show`/`Hide` password affordance;

\- provide `Other network` for hidden SSIDs;

\- when `Other network` is selected, require an explicit Open or Password-protected choice;

\- apply the corresponding password-field behavior immediately;

\- provide `Scan again` as a secondary action;

\- do not hide the network list in a dropdown.



Save flow:



```text

Save and connect

&#x20;   ↓

validate

&#x20;   ↓

Saving…

&#x20;   ↓

Connecting to <SSID>…

&#x20;   ↓

Wi-Fi connected

&#x20;   ↓

Connecting to server…

&#x20;   ↓

Ready

```



Requirements:



\- no navigation or modal dialogs;

\- one primary action: `Save and connect` or `Save changes`;

\- disable/show busy state while the save request itself is processed;

\- preserve all entered values on validation or connection failure;

\- focus the first invalid field;

\- place errors beside their field or associated connection state;

\- keep the setup AP available during the attempt;

\- distinguish Wi-Fi failure from TCP failure;

\- never undo successful Wi-Fi because TCP failed;

\- show `Retrying automatically` for TCP failures.



\### Fluent and accessibility contract



```text

font                     Segoe UI, system-ui, sans-serif

spacing grid             4 px

page padding             16 px

between sections         24 px

within field groups      8–12 px

layout                   mobile-first single column

minimum supported width  320 px

content maximum          approximately 480 px

minimum touch target     44×44 px

phone controls           full width

horizontal scrolling     forbidden

```



The password Show/Hide affordance may sit inside or beside the password control but must retain a 44×44 px target.



Also require:



\- persistent top-aligned labels;

\- polished Fluent-style surfaces, controls, states, and typography;

\- native input, select, and button semantics;

\- visible `:focus-visible` treatment;

\- full keyboard operability;

\- WCAG-appropriate contrast;

\- `aria-live` scan/save/connection results;

\- field errors programmatically associated with their inputs;

\- selected networks indicated by more than color;

\- no icon-only essential actions;

\- `prefers-color-scheme` support;

\- `prefers-reduced-motion` support.



No Fluent library, external font, CDN, framework, Node, Bun, React, or build step is introduced.



\## 4. Prototype-First Milestones



Each milestone remains independently demonstrable. Implement one milestone at a time and run only its focused acceptance checks.



\### P0 — Tiny hardware gate



Before implementing the customer demo:



1\. identify the CP2102 COM port;

2\. confirm UART RX GPIO3 and TX GPIO1;

3\. confirm the real Windows application uses flow control `None`;

4\. open and close the COM port with the real application;

5\. confirm DTR/RTS does not cause an unresolved ESP32 reset that loses the start of a transmission;

6\. confirm PRG+RST enters the ROM flashing bootloader.



Resolve any DTR/RTS reset problem before Milestone 1A. This is the only mandatory pre-demo hardware gate.



\### Milestone 1A — Prove serial capture locally



Implement:



```text

UART RX

→ tagged non-critical display history

→ Live Text OLED

→ serial-only configuration persistence

→ PRG baud cycling

```



At this stage, the shared configuration authority handles serial-only reconfiguration and persistence. Do not create fake TCP coordination hooks.



Before provisioning exists, the pre-traffic screen may show `WAITING FOR SERIAL` with current baud/framing. Do not invent setup credentials before Milestone 3.



Acceptance:



1\. start from factory-reset/unconfigured state;

2\. do not configure Wi-Fi or a server;

3\. connect to the machine/controller PC;

4\. cause the machine to transmit;

5\. show received serial content on OLED;

6\. short-tap PRG if the baud is wrong;

7\. show the baud overlay after every successful tap;

8\. clear history decoded under the previous baud;

9\. obtain readable output at the correct baud;

10\. preserve the selected baud across power cycle despite empty SSID, host, and port zero.



This is the first demo-ready milestone.



\### Milestone 1B — Full-duplex TCP round trip



Add:



```text

UART RX

→ SER→NET queue

→ TCP echo server

→ NET→SER queue

→ UART TX

```



Extend the same configuration boundary with queue and TCP coordination.



Verify:



\- exact binary preservation in both directions;

\- simultaneous full-duplex traffic;

\- partial TCP and UART writes;

\- bounded fair servicing;

\- `TCP\_NODELAY`;

\- reconnect with retained SER→NET queue;

\- NET→SER continues draining during reconnect;

\- queue overflow/drop accounting.



\### Milestone 2 — Complete transport-independent OLED subsystem



Implement and harden only:



```text

Live Text

Live Hex

Statistics

Off

persistent status row

tagged bidirectional history

baud/reset overlays

renderer snapshot isolation

throttled rendering

```



Do not implement setup credentials, credential alternation, or the provisioning-specific full setup screen in this milestone.



\### Milestone 3 — Provisioning, browser, and setup OLED integration



Implement:



\- permanent `s2id` identity and setup-password generation;

\- full setup SSID/password/IP screen before first UART traffic;

\- setup credential header alternation after serial traffic begins;

\- AP lifecycle and captive-portal fallback;

\- application-owned Wi-Fi retry lifecycle;

\- LittleFS-hosted responsive frontend;

\- AP/LAN interface authorization;

\- per-boot CSRF;

\- status, configuration, and asynchronous scan APIs;

\- serialized complete-candidate Save;

\- first-time phone flow, scan states, Fluent interaction, and accessibility.



\### Milestone 4 — Integration and hardening



Exercise:



\- simultaneous UART, TCP, HTTP, Wi-Fi scan, OLED, and PRG activity;

\- concurrent PRG/HTTP mutation attempts;

\- all configuration transition boundaries;

\- AP loss/recovery lifecycle;

\- LAN read-only enforcement;

\- factory-reset success and failure;

\- sustained traffic at every supported baud, including 1000000;

\- counter accuracy and transport/display isolation.



\## 5. Product Acceptance and Non-Goals



\### Zero-configuration proof



```text

Given a factory-reset Serial2WiFi

And no Wi-Fi or TCP server has been configured

When it is connected to a serial source and bytes are transmitted

Then those bytes become visible on the OLED in Live Text mode

without requiring a phone, browser, server, or initial button interaction.



If the factory baud is wrong,

PRG short-taps allow immediate baud discovery at the machine.

```



\### Unknown-baud discovery



```text

Given serial traffic that is unreadable at the current baud

When the user short-taps PRG

Then Serial2WiFi advances to the next supported baud,

shows BAUD <rate> temporarily,

clears history decoded using the old baud,

establishes the required transport boundary,

persists the new baud,

and resumes the persisted display mode.



Repeating this allows the user to reach the correct baud

without entering the web UI.

```



\### Unconfigured configuration validity



```text

Given factory configuration with empty SSID/password,

empty TCP host, and TCP port zero

When PRG changes only the baud

Then complete configuration validation succeeds

and the new baud is persisted without inventing network values.

```



Also verify that host-empty/port-nonzero and host-nonempty/port-zero candidates are rejected.



\### Serialized configuration



```text

Given PRG and HTTP attempt configuration mutations concurrently

Then only one commit executes at a time

and validation, persistence, in-memory publication,

and runtime application never overlap between candidates.

```



Verify the resulting configuration is one complete committed candidate, not a mixture of both.



\### Configured statistics



```text

Given a configured working gateway

When display mode is Statistics

Then the OLED shows both directional current received rates,

both cumulative received-byte totals, drops, Wi-Fi/TCP state,

and current serial configuration.

```



\### Off-mode physical feedback



```text

Given display mode Off

When PRG is short-tapped successfully

Then the OLED temporarily shows the new baud

and returns to Off.



When PRG is held for factory reset

Then the reset overlay remains visible as specified.

```



\### Configuration atomicity



Verify that:



\- PRG and browser changes enter through serialized `commitConfiguration`;

\- persistence failure leaves runtime and in-memory state unchanged;

\- a combined Save performs one coordinated transport boundary;

\- Wi-Fi or TCP connection failure does not roll back saved settings;

\- the AP remains reachable and connection state explains the failure.



\### Wi-Fi lifecycle ownership



Verify that:



\- framework persistence and automatic reconnect are disabled;

\- only the application reconnect state machine calls STA reconnection;

\- the defined 30-second AP recovery rule is deterministic;

\- framework reconnect behavior cannot race AP lifecycle transitions.



\### Setup recovery and authorization



Verify that:



\- the AP follows the exact boot/60-second/30-second lifecycle;

\- reboot provides a healthy-device reconfiguration window;

\- AP requests can scan, read configuration, and save;

\- LAN presents status only;

\- LAN configuration POST returns `403`;

\- spoofing `Host` cannot bypass the local-interface check;

\- missing or incorrect CSRF is rejected;

\- neither station nor setup password is returned.



\### First-time mobile flow



At 320 px width, verify:



\- no horizontal scrolling;

\- Wi-Fi appears directly after the compact header;

\- controls are full width and touch targets are at least 44×44 px;

\- Scanning, empty-result, and failed-scan states are distinguishable;

\- secure/open selections update the password UI immediately;

\- Other network requires an explicit security choice;

\- Show/Hide is keyboard and touch accessible;

\- Save preserves input and announces progress/errors.



\### Factory-reset safety



Verify that:



\- releases before ten seconds never reset;

\- a short tap changes baud only once;

\- ten-second reset success waits for PRG release before reboot;

\- NVS clear failure displays `RESET FAILED` and does not reboot;

\- `s2id`, setup credentials, firmware, and programming recovery survive.



\### Explicit non-goals



\- HTTPS, certificates, or CA provisioning in v1.

\- Configuration writes over normal LAN.

\- Button-driven menus or general configuration; PRG is limited to short-tap baud cycling and ten-second emergency factory reset.

\- Machine-protocol parsing, validation, emulation, or gateway-generated responses.

\- Whole-flash factory erase.

\- Persisted traffic totals or rate history.

\- LoRa/SX1276 functionality.

\- External frontend assets, frameworks, or build tooling.

\- Treating OLED history as a reliable transport buffer.



