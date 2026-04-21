# Wakeword KWS Application

## Description
This application performs wakeword-gated keyword spotting (KWS) from a PDM microphone stream on the nRF54LM20DK.

Audio is captured from DMIC continuously. The application first waits for the wakeword "Okay Nordic". After wakeword detection, it switches to keyword spotting mode for a 7-second active window, logs detected keywords with confidence, and returns to wakeword mode when the window times out.

Supported keywords:
- Down
- Go
- Left
- No
- Off
- On
- Right
- Silence
- Stop
- Unknown
- Up
- Yes

## Hardware Configuration

### Target Board
- nRF54LM20DK

### Microphone
- PDM microphone breakout board: https://www.adafruit.com/product/3492

Use the project DTS overlay as the source of truth for active pin mapping in this app:
- `boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay`

Current overlay PDM signals:
- PDM clock: `P1.04`
- PDM data in: `P1.05`

### PDM microphone breakout board wiring
1. Wire microphone 3V to VDDIO and GND to GND on the PDK.
2. Wire SEL microphone pin to GND on the PDK so PDM data is sampled on the clock rising edge.
3. Wire microphone CLK to P1-04 and DAT to P1-05 on the PDK.

Note: The microphone is 1.8 V to 3.3 V tolerant.

## Notes
If hardware wiring does not match the current DTS overlay pin selection, update either wiring or overlay so both use the same CLK/DAT pins.
