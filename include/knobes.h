#ifndef KNOBES_H
#define KNOBES_H

#if !defined(LUMO_PUBLIC_RELEASE) && __has_include("secrets.h")
#include "secrets.h"
#endif

#if !defined(LUMO_PUBLIC_RELEASE) && __has_include("PilotCloud.h")
#include "PilotCloud.h"
#define LUMO_PILOT_CLOUD 1
#undef STOCK_TWELVE_DATA_API_KEY
#define STOCK_TWELVE_DATA_API_KEY ""
#endif
#ifndef LUMO_PILOT_CLOUD
#define LUMO_PILOT_CLOUD 0
#endif

#ifdef LUMO_PUBLIC_RELEASE
// Fail closed even if private values accidentally arrive through build flags.
#undef PIXYTOY_FACTORY_WIFI_SSID
#undef PIXYTOY_FACTORY_WIFI_PASSWORD
#undef STOCK_TWELVE_DATA_API_KEY
#undef LUMO_AI_BASE_URL
#undef LUMO_AI_DEVICE_TOKEN
#undef LUMO_AI_ENABLED
#define PIXYTOY_FACTORY_WIFI_SSID ""
#define PIXYTOY_FACTORY_WIFI_PASSWORD ""
#define STOCK_TWELVE_DATA_API_KEY ""
#define LUMO_AI_BASE_URL ""
#define LUMO_AI_DEVICE_TOKEN ""
#define LUMO_AI_ENABLED 0
#endif

// Set to 1 to enable audio playback, or 0 to disable it.
#ifndef LUMO_AI_ENABLED
#define LUMO_AI_ENABLED 0
#endif
#ifndef LUMO_AI_BASE_URL
#define LUMO_AI_BASE_URL ""
#endif
#ifndef LUMO_MIC_ENABLED
#define LUMO_MIC_ENABLED 1
#endif
#ifndef LUMO_CLOUD_STREAMING_INPUT
#define LUMO_CLOUD_STREAMING_INPUT 1
#endif
#ifndef LUMO_LIVE_CUBE
#define LUMO_LIVE_CUBE 0 // Private original-board GPT-Live trial only.
#endif
#ifndef LUMO_MIC_END_SILENCE_MS
#define LUMO_MIC_END_SILENCE_MS 500UL
#endif
#ifndef LUMO_AI_DEVICE_TOKEN
#define LUMO_AI_DEVICE_TOKEN ""
#endif

#define AUDIO_ENABLED 1

// Software audio level applied to MP3 and AAC output before I2S playback.
// Valid range: 0 (mute) to 100 (full decoded level).
#define AUDIO_VOLUME_PERCENT 50

// ST7789 SPI pixel format. Set to 18 for RGB666 image output, or 16 to use
// the original RGB565 driver and lower-memory video pipeline.
#define DISPLAY_COLOR_DEPTH 18

// Clock region label. The default shows the full city name.
// Change CLOCK_LABEL_MODE to CLOCK_LABEL_GMT_OFFSET to show GMT offsets.
#define CLOCK_LABEL_CITY_ABBREVIATION 0
#define CLOCK_LABEL_GMT_OFFSET 1
#define CLOCK_LABEL_MODE CLOCK_LABEL_CITY_ABBREVIATION

// Clock artwork. The retro flip clock uses face.rgb (240x240 RGB888) and
// nums.rgb (240x172 RGB888) from /Assets/Pic/RetroClock on the SD card.
// The default wave clock embeds Inter fonts and artwork in flash (no SD assets).
#define CLOCK_STYLE_ANALOG 0
#define CLOCK_STYLE_RETRO_FLIP 1
#define CLOCK_STYLE_WAVE 2
#define CLOCK_STYLE CLOCK_STYLE_WAVE

// Twelve Data provides the OHLC values used by the six stock candlestick
// displays. Put the private key in include/secrets.h (see secrets.example.h).
#ifndef STOCK_TWELVE_DATA_API_KEY
#define STOCK_TWELVE_DATA_API_KEY ""
#endif

#ifndef PIXYTOY_FACTORY_WIFI_SSID
#define PIXYTOY_FACTORY_WIFI_SSID ""
#endif

#ifndef PIXYTOY_FACTORY_WIFI_PASSWORD
#define PIXYTOY_FACTORY_WIFI_PASSWORD ""
#endif

// Refresh a cached upward-facing stock every 15 minutes while XSTOCK is active.
#define STOCK_REFRESH_INTERVAL_MS (15UL * 60UL * 1000UL)
#define STOCK_RETRY_INTERVAL_MS (60UL * 1000UL)
#define STOCK_WIFI_RETRY_INTERVAL_MS 5000UL
#define STOCK_API_CREDITS_PER_MINUTE 8
#define STOCK_ORIENTATION_POLL_MS 250UL
#define STOCK_ORIENTATION_STABLE_MS 750UL

// Stock chart rendering. Continuous draws a close-price line colored by the
// selected range trend; candle draws traditional green/red OHLC candles.
#define STOCK_CHART_CANDLE 0
#define STOCK_CHART_CONTINUOUS 1
#define STOCK_CHART_TYPE STOCK_CHART_CONTINUOUS
#define STOCK_CONTINUOUS_LINE_WIDTH 4
#if STOCK_CHART_TYPE != STOCK_CHART_CANDLE && \
    STOCK_CHART_TYPE != STOCK_CHART_CONTINUOUS
#error "STOCK_CHART_TYPE must be STOCK_CHART_CANDLE or STOCK_CHART_CONTINUOUS"
#endif

// Tap behavior while the stock app is active. A valid cube face must be
// pointing up; one tap advances 1H -> 4H -> 1D -> 1W -> 1H.
#define STOCK_TAP_POLL_INTERVAL_MS 20UL
#define STOCK_TAP_COOLDOWN_MS 250UL
// LIS3DH tap force threshold at the configured +/-2g range. Lower values are
// more sensitive. 25 is tuned for an ordinary fingertip tap on the enclosure.
#define STOCK_TAP_THRESHOLD 25

// Internet-radio recovery and orientation polling intervals.
#define RADIO_RETRY_INTERVAL_MS 5000UL
#define RADIO_ORIENTATION_POLL_MS 250UL
#define RADIO_ORIENTATION_STABLE_MS 750UL

// Focus: per-face minutes (cyan, red, orange, green, pink, purple).
#define FOCUS_FACE_MINUTES {25,25,25,25,25,25}
#define FOCUS_ORIENTATION_STABLE_MS 500UL

// Lumo's 21.81-second introduction: initial cue estimates, milliseconds from
// voice start. Tune against the recording: stocks, timer, radio, weather, time.
#define CREATURE_INTRO_CUES_MS {6300UL,7600UL,8800UL,10300UL,11600UL}

// Key-free Open-Meteo current conditions for the six weather faces.
#define WEATHER_REFRESH_INTERVAL_MS (15UL * 60UL * 1000UL)
#define WEATHER_RETRY_INTERVAL_MS (60UL * 1000UL)

// Mixed dashboard refreshes its Weather and Stock faces only while Internet
// Radio is stopped, preventing TLS and MP3 decoding from competing for heap.
#define DASHBOARD_REFRESH_INTERVAL_MS (15UL * 60UL * 1000UL)
#define DASHBOARD_RETRY_INTERVAL_MS (60UL * 1000UL)

// Set to 1 only for factory/debug builds that should boot directly into BLE setup.
#define BLE_WIFI_SETUP_ONLY 0

// Keep BLE advertising during normal operation so the companion app can switch
// live between Clock, Stocks, Radio, Weather, and Dashboard without restarting.
#define BLE_RUNTIME_APP_SWITCHING 1

// Set to 1 to mirror important setup/status messages on the PixyToy displays.
#define DISPLAY_SETUP_MESSAGES 1

// Set to 1 to show setup/status JPG screens from /Assets/Pic/System when available.
#define DISPLAY_SETUP_IMAGES 1

// Set to 1 only when testing the old WiFi image download path.
#define AUTO_DOWNLOAD_WIFI_TEST_ASSETS 0

// Set this to the ESP32 GPIO connected to a charger STAT/CHG or USB VBUS
// detect signal. Leave -1 when the hardware does not expose charger status.
#define CHARGER_STATUS_PIN -1

// Most Li-ion charger STAT pins are active-low while charging.
#define CHARGER_STATUS_ACTIVE_LEVEL LOW

// Legacy immediate charging view only. Idle charging below has its own
// enable switch; disable CHARGER_IDLE_SCREEN_ENABLED to keep all faces awake.
#define CHARGER_TURN_OFF_OTHER_DISPLAYS 0
// 1: keep apps running with an always-visible battery badge on all six faces.
// The lightning mark is shown only while charging is detected.
// 0: use the original full-screen battery/charging mode above.
#define CHARGER_TOP_OVERLAY 1
// Idle charging screen: motion wakes all faces; set 0 to disable for debugging.
#define CHARGER_IDLE_SCREEN_ENABLED 1
#define CHARGER_IDLE_TIMEOUT_MS 60000UL
#define CHARGER_IDLE_MOTION_THRESHOLD_G 0.08f

// Fallback charger detection when no charger status pin is available.
// This treats a steady battery-voltage rise as "probably charging".
#define CHARGER_DETECT_BY_VOLTAGE 1
#define CHARGER_VOLTAGE_SAMPLE_MS 10000
#define CHARGER_VOLTAGE_RISE_THRESHOLD 0.10f
#define CHARGER_VOLTAGE_FALL_THRESHOLD 0.05f
#define CHARGER_MIN_DETECT_VOLTAGE 3.95f

// Show the current battery ADC voltage on the bottom display while setup mode is idle.
#define SHOW_BATTERY_VOLTAGE_IN_SETUP 1

#endif // KNOBES_H
