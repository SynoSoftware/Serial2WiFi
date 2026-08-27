# Serial2WiFi — Historical Follow-up Decisions

Date: 2026-08-26

Status: historical reference only.

This document follows `InitialPlan.md`. The initial plan remains the record of
the original product contract. This file records decisions that were active when
it was written, but it is not current implementation authority.

## 1. Historical decision: preserve the AP/LAN boundary

Authenticated LAN management is intentional. The setup AP remains the
bootstrap path and the only interface for Wi-Fi scans and the browser
terminal.

Available through the setup AP and authenticated station/LAN sessions:

- runtime status;
- authentication state needed to present the management UI;
- configuration reads and writes;
- CSRF-token access used by management operations.

Setup AP only:

- Wi-Fi scans;
- the browser terminal;
- initial administrator-password creation while the management password is
  unset.

Unauthenticated station/LAN requests still cannot mutate configuration.

The implementation must enforce this at the HTTP authorization boundary, not
only by hiding controls in the frontend. Host headers and captive hostnames
must not bypass the accepted socket’s local-interface check.

## 2. Historical decision: adopt PRG interaction

The OLED is a small navigable carousel rather than a menu-driven configuration
surface.

- A short PRG tap advances to the next available OLED page.
- The timed boot branding is the only startup exception: while it is still
  visible, the first short tap lands on the preferred runtime page instead of
  advancing the carousel.
- Setup-only pages are skipped when the setup AP is unavailable.
- A hold on the Serial page advances the baud rate and persists it through the
  shared configuration authority.
- Repeated baud changes while holding stop after one complete baud cycle or on
  the first failed save.
- Holding for the factory-reset threshold retains the existing reset warning,
  reset result, release requirement, and reboot safety rules.
- PRG has no additional menu gesture.

The prior short-tap-to-change-baud description in `InitialPlan.md` is
superseded by this section.

## 3. Historical decision: adopt factory-reset semantics

Factory reset means recovery to a newly initialized device, including when the
operator no longer knows the setup Wi-Fi password or has disabled the setup
AP.

- It clears every application-owned NVS namespace: `s2w` configuration,
  `s2id` setup identity, and `auth` management credentials.
- On the following reboot, the device generates and persists a new setup AP
  password, has no station configuration, and has no management password.
- It does not erase the NVS partition wholesale or erase flash. Firmware,
  bootloader, partition data, and non-application data remain intact.
- The existing ten-second hold, result indication, release-before-reboot rule,
  and failure behavior remain unchanged.

This supersedes the `InitialPlan.md` statements that factory reset preserves
`s2id` or setup credentials.

### Exact-schema configuration storage

The configuration blob is accepted only when its length, schema version, and
validation all match the current firmware. There are no configuration
migrations. Any missing, malformed, or earlier-version `s2w` configuration is
cleared and replaced with the current factory-default configuration.

## 4. Adopted startup OLED flow

Startup branding is a timed boot indication, not a carousel page. After the
branding interval, the carousel begins on the preferred runtime page. The
Brand page remains an ordinary page in the cycle, and the first short tap
while the boot indication is still visible lands on the preferred runtime page
instead of stepping through the remaining carousel pages one by one.

When the setup AP is available, the setup QR and credential pages are reached
through the carousel. They are not forced immediately after boot. The normal
live text, live hex, statistics, and serial pages remain available according
to the current navigation and persisted display preference.

The persisted display preference still controls the preferred runtime page.
Changing network configuration must not silently force Live Text, Hex,
Statistics, or Off.

## 5. Accepted QR limitation

The TTGO LoRa32 V1 has a 128×64 OLED. The current compact QR layout is retained
because increasing the QR version would make the code materially harder to
scan on this display.

This is an accepted hardware/display limitation for the next step. Do not
expand the QR code or redesign the setup page as part of the AP/LAN fix. Any
future QR change requires a display-size or credential-format decision first.

## 6. Deferred TCP naming and validation changes

No TCP naming or validation changes are included in the next step.

The current `listen`/`connect` API names and the existing endpoint validation
remain in place for now. Revisit whether the public names should be
`server`/`client`, and whether inactive-mode endpoint fields should be cleared
or rejected, in a separate TCP-focused change.

## 7. Verification for the next step

Before committing the AP/LAN change:

- build the firmware with the pinned PlatformIO platform;
- verify JavaScript syntax;
- verify that authenticated LAN configuration GET/POST works;
- verify that scan requests from station/LAN receive `403`;
- verify that status remains available through station/LAN;
- verify that setup-AP configuration and scan requests continue to work;
- verify that a factory reset removes the saved station configuration, setup
  credentials, and management password, then creates new setup credentials at
  the next boot;
- keep captive portal, UART, OLED, and TCP hardware checks pending until a
  physical device is available.
