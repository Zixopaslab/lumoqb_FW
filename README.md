# lumoqb_FW

Internet radio portion of the Lumoqb firmware, extracted from `lumoqb/02_FW`.
This is a reference extraction, not a standalone build: the radio code still depends on `main.cpp`
globals (Wi-Fi, display, orientation) that live in the main firmware repo.

Branch `01_internet_radio` layout:

| Path | Contents |
|---|---|
| `src/radio_buffer.h` | 8 KB jitter buffer between socket and decoder |
| `src/esp32_audio_task.h` | MP3 task, Helix decoder, I2S output, volume |
| `src/radio_visualizer.h`, `include/RadioVizMotion.h` | 8-band Goertzel spectrum and bar easing |
| `src/ferrofluid_radio.h` | Ferrofluid visual driven by radio audio |
| `src/face_settings.h`, `src/world_faces.h` | Per-side station choice, BLE validation, NVS |
| `src/main_radio_excerpt.cpp` | Radio-only excerpt of `main.cpp`: catalog, connect, stop, retry |
| `include/knobes.h` | Timing constants (`RADIO_*`) and build config |
| `app/world-directory.js` | Phone-side station search and validation |
| `docs/internet-radio-architecture.html` | Architecture diagram and notes |
