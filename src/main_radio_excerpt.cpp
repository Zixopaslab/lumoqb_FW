// Radio-only excerpt of lumoqb/02_FW/src/main.cpp (not a standalone build).
// Sections: RadioStation struct, RADIO_CATALOG, stream connect/stop/retry, orientation handling.
// Line ranges in the original: 236-242, 280-295, 3941-4203.

// ---- types and catalog ----
struct RadioStation {
  const char* titleLine1;
  const char* titleLine2;
  const char* host;
  const char* path;
  uint16_t port;
};

// Direct 64-128 kbps MP3 endpoints keep decoding and buffering modest on the
// no-PSRAM ESP32. Each physical side selects the station at the same index.
const RadioStation RADIO_CATALOG[] = {
  {"GROOVE", "SALAD",   "ice5.somafm.com", "/groovesalad-128-mp3", 80},
  {"DRONE",  "ZONE",    "ice5.somafm.com", "/dronezone-128-mp3", 80},
  {"SECRET", "AGENT",   "ice5.somafm.com", "/secretagent-128-mp3", 80},
  {"SPACE",  "STATION", "ice5.somafm.com", "/spacestation-128-mp3", 80},
  {"RADIO",  "PARSA",   "parsa.icdndhcp.com", "/stream", 18000},
  {"RADIO", "HAMRAH", "stream.radiohamrah.com", "/RadioHamrahMP3", 80},
  {"BEAT", "BLENDER", "ice5.somafm.com", "/beatblender-128-mp3", 80},
  {"LUSH", "", "ice5.somafm.com", "/lush-128-mp3", 80},
  {"DEF CON", "RADIO", "ice5.somafm.com", "/defcon-128-mp3", 80},
  {"DEEP SPACE", "ONE", "ice5.somafm.com", "/deepspaceone-128-mp3", 80},
  {"SUBURBS", "OF GOA", "ice5.somafm.com", "/suburbsofgoa-128-mp3", 80},
  {"SYNPHAERA", "RADIO", "ice5.somafm.com", "/synphaera-128-mp3", 80}
};

// ---- stream control ----
// Drain already-buffered headers even if an HTTP/1.0 redirect server has
// closed the socket. Unlike readStringUntil(), a timeout is never mistaken
// for the empty line that terminates the headers.
static bool readRadioHeaderLine(String& line, uint32_t started) {
  line = "";
  while (millis() - started < 8000UL) {
    if (radioStreamClient.available() > 0) {
      int c = radioStreamClient.read();
      if (c < 0) continue;
      if (c == '\n') return true;
      if (c != '\r') {
        if (line.length() >= 1024) return false;
        line += static_cast<char>(c);
      }
    } else {
      if (!radioStreamClient.connected()) return false;
      vTaskDelay(1);
    }
  }
  return false;
}

void stopRadioStream() {
  radioVizEnabled.store(false,std::memory_order_relaxed);
  Request_Audio_Stop();
  // RadioBufferedStream checks the stop flag while waiting. Do not destroy
  // WiFiClient's RX buffer while the decoder is inside available()/read().
  if (!Wait_Audio_Tasks_Stopped(1000)) {
    Serial.println("Radio: graceful audio stop timed out; forcing cleanup.");
    Stop_Audio_Tasks();
  }
  radioStreamClient.stop();
  // Self-deleting audio tasks leave their stack for the idle task to reclaim.
  // Give it time before allocating the next decoder and task on a reconnect.
  vTaskDelay(pdMS_TO_TICKS(20));
  Reset_MP3_Decoder();
  radioBufferedStream.end();
  i2s_zero_dma_buffer(I2S_NUM_0);
}

static bool parseHttpRadioUrl(const String& url, String& host,
                              uint16_t& port, String& path) {
  const String prefix = "http://";
  if (!url.startsWith(prefix)) return false;

  int authorityStart = prefix.length();
  int pathStart = url.indexOf('/', authorityStart);
  String authority = pathStart >= 0
      ? url.substring(authorityStart, pathStart)
      : url.substring(authorityStart);
  path = pathStart >= 0 ? url.substring(pathStart) : "/";

  int portSeparator = authority.lastIndexOf(':');
  if (portSeparator > 0) {
    long parsedPort = authority.substring(portSeparator + 1).toInt();
    if (parsedPort < 1 || parsedPort > 65535) return false;
    host = authority.substring(0, portSeparator);
    port = static_cast<uint16_t>(parsedPort);
  } else {
    host = authority;
    port = 80;
  }
  return host.length() > 0;
}

bool startRadioStation(uint8_t stationIndex) {
  if (stationIndex >= 6) return false;
  // Live STT may still own RX/TLS while a dashboard face switches to radio.
  // The regular radio retry starts playback after its speaker handoff completes.
  if (!stopContinuousLumoCaptureForPlayback()) {radioLastStartAttemptMs=millis();return false;}

  radioLastStartAttemptMs = millis();
  stopRadioStream();
  if (dashboardModeActive) {
    drawRadioFace(stationIndex, stationIndex, "CONNECTING");
  } else {
    showRadioScreens(stationIndex, "CONNECTING");
  }

  WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED && !autoConnectSavedWifi()) {
    Serial.println("Radio: no saved WiFi connection is available.");
    if (dashboardModeActive) drawRadioFace(stationIndex, stationIndex, "WIFI FAILED");
    else showRadioScreens(stationIndex, "WIFI FAILED");
    radioLastStartAttemptMs = millis();
    return false;
  }

  // Leave modem sleep enabled: this ESP32 WiFi/Bluetooth coexistence build
  // aborts if it is disabled while BLE is active (even just advertising).

  const RadioStation& station = RADIO_STATIONS[stationIndex];
  String streamHost = station.host;
  String streamPath = station.path;
  uint16_t streamPort = station.port;
  // WiFiClient's overload takes seconds (unlike Stream's milliseconds).
  radioStreamClient.setTimeout(5);

  bool streamReady = false;
  for (uint8_t redirectCount = 0; redirectCount <= 3; ++redirectCount) {
    Serial.printf("Radio: connecting to http://%s:%u%s\n",
                  streamHost.c_str(), streamPort, streamPath.c_str());
    radioStreamClient.stop();
    if (!radioStreamClient.connect(streamHost.c_str(), streamPort)) {
      Serial.println("Radio: stream server connection failed.");
      break;
    }

    String hostHeader = streamHost;
    if (streamPort != 80) hostHeader += ":" + String(streamPort);
    radioStreamClient.printf(
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: PixyToy/0.1\r\n"
        "Accept: audio/mpeg\r\n"
        "Icy-MetaData: 0\r\n"
        "Connection: close\r\n\r\n",
        streamPath.c_str(), hostHeader.c_str());

    uint32_t headerStarted = millis();
    String statusLine;
    if (!readRadioHeaderLine(statusLine, headerStarted)) {
      Serial.println("Radio: incomplete or oversized HTTP status line.");
      break;
    }
    statusLine.trim();
    Serial.printf("Radio: server response: %s\n", statusLine.c_str());
    bool responseOk = statusLine.indexOf(" 200 ") >= 0;
    bool responseRedirect = statusLine.indexOf(" 301 ") >= 0 ||
                            statusLine.indexOf(" 302 ") >= 0 ||
                            statusLine.indexOf(" 307 ") >= 0 ||
                            statusLine.indexOf(" 308 ") >= 0;
    String redirectLocation;
    bool headersComplete = false;
    bool unsupportedFraming = false;
    for (unsigned headerCount = 0; headerCount < 64; ++headerCount) {
      String header;
      if (!readRadioHeaderLine(header, headerStarted)) break;
      header.trim();
      String lowerHeader=header;lowerHeader.toLowerCase();
      if ((lowerHeader.startsWith("transfer-encoding:") && lowerHeader.indexOf("chunked")>=0) ||
          (lowerHeader.startsWith("icy-metaint:") && lowerHeader.substring(12).toInt()>0))
        unsupportedFraming=true;
      if (header.length() == 0) {
        headersComplete = true;
        break;
      }
      if (lowerHeader.startsWith("location:")) {
        redirectLocation = header.substring(header.indexOf(':') + 1);
        redirectLocation.trim();
      }
    }

    if (!headersComplete) {
      Serial.println("Radio: incomplete, timed-out or oversized stream headers.");
      break;
    }
    if (responseOk) {
      if(unsupportedFraming) {
        Serial.println("Radio: unsupported chunked/ICY metadata response; refusing non-audio bytes.");
        break;
      }
      streamReady = true;
      break;
    }
    if (!responseRedirect || redirectLocation.length() == 0 ||
        redirectCount == 3) {
      Serial.printf("Radio: server rejected stream: %s\n", statusLine.c_str());
      break;
    }

    if (redirectLocation.startsWith("/")) {
      streamPath = redirectLocation;
    } else if (!parseHttpRadioUrl(redirectLocation, streamHost,
                                  streamPort, streamPath)) {
      Serial.printf("Radio: unsupported redirect: %s\n",
                    redirectLocation.c_str());
      break;
    }
    Serial.println("Radio: following stream redirect.");
  }

  if (!streamReady) {
    radioStreamClient.stop();
    if (dashboardModeActive) drawRadioFace(stationIndex, stationIndex, "RETRYING");
    else showRadioScreens(stationIndex, "RETRYING");
    radioLastStartAttemptMs = millis();
    return false;
  }

  last_audio_output_ms = 0;
  if(!radioBufferedStream.begin(radioStreamClient)) {
    Serial.println("Radio: cannot allocate 8192-byte prebuffer; retrying later.");
    radioStreamClient.stop();radioLastStartAttemptMs=millis();return false;
  }
  BaseType_t taskResult = mp3_player_task_start(&radioBufferedStream, RADIO_AUDIO_CORE);
  if (taskResult != pdPASS) {
    Serial.printf("Radio: MP3 task start failed: %d\n", taskResult);
    radioStreamClient.stop();
    Reset_MP3_Decoder();
    radioBufferedStream.end();
    if (dashboardModeActive) drawRadioFace(stationIndex, stationIndex, "AUDIO ERROR");
    else showRadioScreens(stationIndex, "AUDIO ERROR");
    radioLastStartAttemptMs = millis();
    return false;
  }

  for(auto &level:radioVizLevels)level.store(0,std::memory_order_relaxed);
  radioVizUpdated.store(0,std::memory_order_release);
  radioVizEnabled.store(true,std::memory_order_relaxed);
  Serial.printf("Radio: playback heap: %u free, %u largest block.\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.printf("Radio: playing side %u, %s %s.\n",
                stationIndex + 1, station.titleLine1, station.titleLine2);
  if (dashboardModeActive) drawRadioFace(stationIndex, stationIndex, "PLAYING",false);
  else showRadioScreens(stationIndex, "PLAYING");
  radioLastStartAttemptMs = millis();
  return true;
}

void updateRadioMode() {
  updateRadioVisualizer();
  unsigned long nowMs = millis();
  if (!radioScreensDrawn) showRadioScreens(-1, "SELECT SIDE");

  if (nowMs - radioLastOrientationPollMs >= RADIO_ORIENTATION_POLL_MS) {
    radioLastOrientationPollMs = nowMs;
    setFastPeripheralSelection(true);
    _gyro.Tilt_Detection(&ctx.cubeOrientation, false);
    setFastPeripheralSelection(false);
    uint8_t orientation = ctx.cubeOrientation;

    if (orientation < 1 || orientation > 6) {
      radioCandidateOrientation = 0;
      radioCandidateSinceMs = 0;
    } else if (orientation != radioCandidateOrientation) {
      radioCandidateOrientation = orientation;
      radioCandidateSinceMs = nowMs;
    } else if (orientation != radioLastOrientation &&
               nowMs - radioCandidateSinceMs >= RADIO_ORIENTATION_STABLE_MS) {
        radioLastOrientation = orientation;
        radioSelectedStation = orientation - 1;
        startRadioStation(radioSelectedStation);
        return;
    }
  }

  if (radioSelectedStation < 0) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (mp3TaskHandle != nullptr) stopRadioStream();
  }

  // readBytes() returns when its five-second socket timeout expires. The MP3
  // task then exits, and the block below reconnects it. Do not infer a stalled
  // stream from the I2S callback timestamp: that produced false reconnects in
  // normal mode even while the same audio played continuously in charge mode.
  if (mp3TaskHandle == nullptr &&
      nowMs - radioLastStartAttemptMs >= RADIO_RETRY_INTERVAL_MS) {
    Serial.println("Radio: stream ended; reconnecting.");
    startRadioStation(radioSelectedStation);
  }
}
