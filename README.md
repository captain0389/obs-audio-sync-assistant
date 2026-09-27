# OBS Audio Sync Assistant — prototype

A Windows-first OBS Studio plugin prototype that measures the relative timing of two OBS audio sources using GCC-PHAT cross-correlation and can apply the measured correction to the target source.

## What the prototype does

1. Adds an **Audio Sync Assistant** dock to OBS.
2. Enumerates audio-capable OBS sources.
3. Lets the user select a **Reference** and **Target** source.
4. Captures 3/5/8 seconds of source audio through OBS's source audio-capture callbacks.
5. Converts each source to mono and runs GCC-PHAT correlation.
6. Reports the measured offset and a conservative confidence score.
7. Applies the signed correction to the target using `obs_source_set_sync_offset()`.

### Offset convention

A positive measured offset means **the target leads the reference**, so the plugin adds a positive delay to the target. A negative offset means the target lags the reference and the plugin applies a negative correction.

## Important prototype limitations

- This first version assumes OBS's normal 48 kHz audio rate.
- It captures only while the selected sources are producing audio.
- It uses a simple in-memory capture window; it does not yet compensate for clock drift.
- It does not yet synchronize video.
- It does not yet provide a calibration tone.
- Confidence is intentionally conservative and should not be treated as a statistical probability.
- The project has not been compiled in this environment because the OBS/Qt Windows development SDK is not installed here.

## Build prerequisites on Windows

- Windows 10/11 x64
- Visual Studio 2022 or newer with C++ desktop workload
- CMake 3.28+
- Qt 6 development libraries compatible with your OBS build
- OBS Studio development libraries / CMake package

The official OBS plugin template supports Windows with Visual Studio and CMake and is the recommended starting point for production packaging.

## Suggested next development steps

1. Build against the user's exact OBS version.
2. Verify callback sample format and source activation behavior on Windows.
3. Add a continuously updated waveform/correlation display.
4. Add selectable analysis band-pass/high-pass filtering to reject voice/noise.
5. Add drift measurement over longer recordings.
6. Add **Calibration Mode** that emits a known click sequence.
7. Add video/audio synchronization using OBS raw video timestamps.
8. Add undo/restore of the previous source sync offset.
9. Add installer/package generation.


## v0.3.0 calibration mode

Adds a temporary known chirp calibration source and speaker-to-microphone calibration workflow. See README.txt in the patch for details.


## v0.3.0

Waveform Alignment mode replaces the experimental microphone/speaker calibration feature. The plugin records two OBS audio sources simultaneously, displays their waveforms, provides an automatic initial alignment estimate, and lets the user fine-tune the correction with a millisecond slider before applying it to the target source. The dock is scrollable so the UI no longer forces OBS to stretch.
