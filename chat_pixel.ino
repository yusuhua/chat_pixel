#define XIAOZHI_BOARD CUSTOM_BOARD
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_MP34DT05_MAX98357A
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1

// HUB75 MatrixPanel
#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN 1
#define E_PIN 1

// PDM 麦克风（MP34DT05）
#define BOARD_AUDIO_PDM_I2S_PORT 0
#define BOARD_AUDIO_PDM_SAMPLE_RATE 16000
#define BOARD_AUDIO_PDM_CLOCK 14
#define BOARD_AUDIO_PDM_DATA 47
#define BOARD_AUDIO_PDM_CLOCK_INVERTED false

// MAX98357A（I2S 输出）
#define BOARD_AUDIO_OUTPUT_I2S_PORT 1
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK -1
#define BOARD_AUDIO_OUTPUT_BCLK 12
#define BOARD_AUDIO_OUTPUT_WS 11
#define BOARD_AUDIO_OUTPUT_DATA 13

#ifndef XIAOZHI_DEVICE_BOARD_TYPE
#define XIAOZHI_DEVICE_BOARD_TYPE "chat-pixel-s3"
#endif
#ifndef XIAOZHI_DEVICE_BOARD_NAME
#define XIAOZHI_DEVICE_BOARD_NAME "Chat Pixel S3"
#endif
#ifndef XIAOZHI_CHAT_BUTTON_PIN
#define XIAOZHI_CHAT_BUTTON_PIN 0
#endif
#ifndef XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL
#define XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL LOW
#endif
#define AP_PREFIX "ChatPixel"

#include <Xiaozhi.h>
#include <ArduinoWebsockets.h>
#include <EspressifOpus.h>
#include <ESP_SR.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <AnimatedGIF.h>
#include <behavior.h>
#include <icon.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <cstring>

xiaozhi::ArduinoWebSocketTransport network_transport;
xiaozhi::AsyncTransportConfig makeTransportConfig() {
  xiaozhi::AsyncTransportConfig config;
  config.control_event_queue_depth = 4;
  config.receive_text_pool_depth = 1;
  config.receive_binary_pool_depth = 1;
  config.transmit_control_pool_depth = 3;
  config.transmit_audio_pool_depth = 4;
  config.maximum_text_bytes = 4096;
  config.maximum_binary_bytes = 1275 + 16;
  config.task_stack_in_psram = true;
  // The official service closes its audio channel after goodbye/inactivity.
  // Return to local wake listening instead of reopening that finished chat.
  config.reconnect_after_disconnect = false;
  return config;
}
xiaozhi::AsyncTransport transport(network_transport, makeTransportConfig());
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);

I2sOpusAudioPort::Config makeAudioConfig() {
  I2sOpusAudioPort::Config config = xiaozhi_audio_board::makeConfig();
  config.inputTaskStackBytes = 4 * 1024;
  config.outputTaskStackBytes = 3 * 1024;
  config.wakeTaskStackBytes = 4 * 1024;
  config.captureLogIntervalPackets = 10;
  config.enableSpeechConditioning = true;
  config.speechGateRms = 250;
  config.speechTargetRms = 2200;
  config.speechMaximumGain = 4.0f;
  config.speechSilenceGain = 0.25f;
  config.speechStartPackets = 2;
  config.speechHoldMs = 420;
  config.enableWakeDetection = true;
  config.enableWakeWordInterruption = false;
  config.wakeModelPartition = "model";
  config.wakeModelKeyword = "wn9_miaomiaotongxue_tts";
  config.defaultWakeWord = "喵喵同学";
  config.wakeDetectionMode = I2sOpusAudioPort::WakeDetectionMode::Balanced;
  return config;
}
const I2sOpusAudioPort::Config audioConfig = makeAudioConfig();
I2sOpusAudioPort audioPort(audioConfig);

constexpr uint32_t kChatButtonDebounceMs = 40;
volatile bool clientStarted = false;
uint32_t lastHeartbeatMs = 0;
uint32_t measuredWakeAudioHits = 0;
uint32_t measuredWakeCaptureHits = 0;
uint32_t downlinkAudioFrames = 0;
uint32_t downlinkAudioBytes = 0;
bool chatButtonReading = HIGH;
bool chatButtonStableState = HIGH;
uint32_t chatButtonChangedMs = 0;
MatrixPanel_I2S_DMA *dma_display = nullptr;
AnimatedGIF gif;
Preferences prefs;

void drawIcon(const uint16_t *icon, int16_t x, int16_t y, int16_t width, int16_t height ) {
  for (int16_t row = 0; row < height; row++) {
    for (int16_t col = 0; col < width; col++) {
      uint16_t color = pgm_read_word(&icon[row * width + col]);
      dma_display->drawPixel(x + col, y + row, color);
    }
  }
}

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Stt:
      Serial.printf("[xiaozhi] STT: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::TtsSentence:
      Serial.printf("[xiaozhi] TTS: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::Emotion:
      Serial.printf("[xiaozhi] Emotion: name=%s type=%d\n",
                event.emotion.c_str(),
                static_cast<int>(event.emotion_type));
      break;
    case xiaozhi::EventType::Alert:
      Serial.printf("[xiaozhi] alert[%s]: %s\n", event.status.c_str(),
               event.text.c_str());
      break;
    default:
      break;
  }
}

void toggleDialogue(const char* source) {
  if (!runtime.ready()) {
    Serial.printf("[xiaozhi] %s ignored: client is not ready\n", source);
    return;
  }
  if (runtime.state() == xiaozhi::State::Connecting) {
    Serial.printf("[xiaozhi] %s ignored: already connecting\n", source);
    return;
  }
  Serial.printf("[xiaozhi] dialogue toggle from %s\n", source);
  const bool accepted = runtime.state() == xiaozhi::State::Speaking
                            ? runtime.requestAbortSpeaking()
                            : runtime.requestToggleChat();
  if (!accepted) {
    Serial.printf("[xiaozhi] dialogue toggle failed in state=%s\n",
                  runtime.stateName());
  }
}

void pollDialogueTriggers() {
  const uint32_t now = millis();
  const bool reading = digitalRead(XIAOZHI_CHAT_BUTTON_PIN);
  if (reading != chatButtonReading) {
    chatButtonReading = reading;
    chatButtonChangedMs = now;
  }
  if (reading != chatButtonStableState &&
      now - chatButtonChangedMs >= kChatButtonDebounceMs) {
    chatButtonStableState = reading;
    if (chatButtonStableState == XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL) {
      toggleDialogue("KEY button");
    }
  }
}

void GIFDraw(GIFDRAW *pDraw) {
  if (!pDraw || !dma_display) {
    return;
  }

  uint8_t *s = pDraw->pPixels;
  uint16_t *usPalette = pDraw->pPalette;
  uint16_t usTemp[64];
  int y = pDraw->iY + pDraw->y;
  const int width = pDraw->iWidth;

  // 如果为 restore to background color
  if (pDraw->ucDisposalMethod == 2) {
    for (int x = 0; x < width; x++) {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // 如果有透明色
  if (pDraw->ucHasTransparency) {
    uint8_t *pEnd = s + width;
    uint8_t ucTransparent = pDraw->ucTransparent;
    int x = 0;
    int iCount = 0;

    while (x < width) {
      // 处理一段不透明像素
      uint8_t c = ucTransparent - 1;
      uint16_t *d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {
          s--;  // 回退一个像素
        } else {
          *d++ = usPalette[c];
          iCount++;
        }
      }

      if (iCount > 0) {
        for (int offset = 0; offset < iCount; offset++) {
          dma_display->drawPixel(x + offset + pDraw->iX, y, usTemp[offset]);
        }
        x += iCount;
        iCount = 0;
      }

      // 跳过透明像素
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }

      if (iCount > 0) {
        x += iCount;
        iCount = 0;
      }
    }
  } else  // 没有透明色
  {
    s = pDraw->pPixels;
    for (int x = 0; x < width; x++) {
      dma_display->drawPixel(x + pDraw->iX, y, usPalette[*s++]);
    }
  }
}

void displayTask(void *pvParameters) {
  while (clientStarted) {
    if (runtime.stateName() == xiaozhi::stateName(xiaozhi::State::Speaking) && gif.open((uint8_t *)speak, sizeof(speak), GIFDraw)) {
      while (gif.playFrame(true, NULL)) {}
      gif.close();
    } else if (gif.open((uint8_t *)listen, sizeof(listen), GIFDraw)) {
      while (gif.playFrame(true, NULL)) {}
      gif.close();
    }
    vTaskDelay(1);
  }
  vTaskDelete(NULL);
}

void registerSetBrightnessMcp() {
  xiaozhi::McpTool tool;
  tool.name = "screen.set_brightness";
  tool.description = "Set screen brightness from 0 to 100 percent.";
  tool.properties = {
    xiaozhi::McpProperty::Integer("brightness", 0, 100)
  };
  tool.handler = [](const xiaozhi::McpArguments& arguments) {
    int32_t brightness = 0;
    arguments.getInt("brightness", brightness);
    if (brightness < 0) {
        brightness = 0;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    prefs.begin("screen", false);
    prefs.putInt("brightness", brightness);
    prefs.end();

    // 0~100 转换成 HUB75 的 0~255
    uint8_t brightness8 = static_cast<uint8_t>((brightness * 255) / 100);
    if (dma_display != nullptr) {
        dma_display->setBrightness8(brightness8);
    }
    Serial.printf( "[mcp] set brightness: %ld%% -> %d/255\n", static_cast<long>(brightness), brightness8);
    return xiaozhi::McpResult::Boolean(true);
  };

  std::string error;
  if (!client.mcp().addTool(std::move(tool), &error)) {
    Serial.printf("[mcp] set brightness: %s\n", error.c_str());
  }
}

void registerGetBrightnessMcp() {
  xiaozhi::McpTool tool;
  tool.name = "screen.get_brightness";
  tool.description = "Get the current screen brightness from 0 to 100 percent.";
  tool.handler = [](const xiaozhi::McpArguments& arguments) {
    prefs.begin("screen", false);
    int32_t screenBrightness = prefs.getInt("brightness");
    prefs.end();
    Serial.printf("[mcp] get brightness: %ld%%\n", static_cast<long>(screenBrightness));
    return xiaozhi::McpResult::Integer(screenBrightness);
  };

  std::string error;
  if (!client.mcp().addTool(std::move(tool), &error)) {
    Serial.printf("[mcp] get brightness: %s\n", error.c_str());
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxBufferSize(1024);
  Serial.setTxTimeoutMs(0);
#endif
  delay(250);

  // 优先尝试从外部PSRAM分配
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(0);
  }
  Serial.printf("[board] %s psram=%lu free_psram=%lu\n",
                XIAOZHI_DEVICE_BOARD_NAME,
                static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(ESP.getFreePsram()));

  // 硬件检查
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  mxconfig.gpio.e = E_PIN;
  mxconfig.clkphase = false;
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  prefs.begin("screen", false);
  int32_t screenBrightness = prefs.getInt("brightness", 40);
  prefs.end();
  uint8_t brightness8 = static_cast<uint8_t>((screenBrightness * 255) / 100);
  dma_display->setBrightness8(brightness8);
  dma_display->clearScreen();
  drawIcon(logo_64, 0, 0, LOGO_64_WIDTH, LOGO_64_HEIGHT);
  delay(1000);
  Serial.printf("[display] width=%d, height=%d\n", dma_display->width(), dma_display->height());

  // Start going through GIFS
  gif.begin(LITTLE_ENDIAN_PIXELS);

  const auto& audioOutput = audioConfig.hardware.output;
  Serial.printf("[audio] profile=%s OUT port=%d MCLK=%d BCLK=%d WS=%d DATA=%d\n",
                I2sOpusAudioPort::Config::compiledProfileName(),
                audioOutput.port, audioOutput.mclk, audioOutput.bclk,
                audioOutput.ws, audioOutput.data);
  const auto& pdmInput = audioConfig.hardware.pdmInput;
  Serial.printf("[audio] IN PDM port=%d CLK=%d DATA=%d\n", pdmInput.port, pdmInput.clock, pdmInput.data);
  if (!xiaozhi_audio_board::probe(audioConfig)) {
    Serial.println("[audio] selected audio hardware probe failed");
    dma_display->clearScreen();
    drawIcon(error_48, 8, 0, ERROR_48_WIDTH, ERROR_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("AHW Probe Failed");
    return;
  }

  // 聊天按键
  pinMode(XIAOZHI_CHAT_BUTTON_PIN, INPUT_PULLUP);

  // WIFI配网
  dma_display->clearScreen();
  drawIcon(wifi_48, 8, 0, WIFI_48_WIDTH, WIFI_48_HEIGHT);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[32];
  sprintf(ssid, "%s-%02X%02X", AP_PREFIX, mac[4], mac[5]);
  Serial.printf("[wifi] ssid=%s\n", ssid);
  dma_display->setCursor(0, 48);
  dma_display->print("SSID: ");
  dma_display->print(ssid);
  WiFi.mode(WIFI_AP_STA);
  WiFiManager wifiManager;
  if (!wifiManager.autoConnect(ssid)) {
    Serial.println("[wifi] connected failed, restart");
    dma_display->clearScreen();
    drawIcon(wifi_f_48, 8, 0, WIFI_F_48_WIDTH, WIFI_F_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("WIFI Connected Failed");
    return;
  }
  Serial.println("[wifi] connected successfully");
  dma_display->clearScreen();
  drawIcon(wifi_s_48, 8, 0, WIFI_S_48_WIDTH, WIFI_S_48_HEIGHT);
  dma_display->setCursor(0, 48);
  dma_display->print("WIFI Connected");
  
  // 小智AI云服务认证
  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  std::string provisioningError;
  xiaozhi::OfficialServiceOptions serviceOptions;
  serviceOptions.board_type = XIAOZHI_DEVICE_BOARD_TYPE;
  serviceOptions.board_name = XIAOZHI_DEVICE_BOARD_NAME;
  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, provisioningError, serviceOptions)) {
    Serial.printf("[xiaozhi] provisioning failed: %s\n", provisioningError.c_str());
    dma_display->clearScreen();
    drawIcon(error_48, 8, 0, ERROR_48_WIDTH, ERROR_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("Provisioning Failed");
    return;
  }

  config.max_json_bytes = 4096;
  config.max_audio_payload_bytes = 1275;
  config.enable_server_aec = false;
  config.enable_voice_barge_in = false;
  Serial.printf("[xiaozhi] official WebSocket protocol v%u, token present=%s\n",
                config.protocol_version,
                config.authorization.empty() ? "no" : "yes");
  Serial.printf("[audio] server AEC=%s voice barge-in=%s\n",
                config.enable_server_aec ? "on" : "off",
                config.enable_voice_barge_in ? "on" : "off");
  Serial.printf("[wake] word interruption=%s\n",
                audioConfig.enableWakeWordInterruption ? "on" : "off");

  // 设备激活验证
  if (provisioning.activation.present && !provisioning.activation.code.empty()) {
    Serial.printf("[xiaozhi] activation code=%s message=%s\n",
                  provisioning.activation.code.c_str(),
                  provisioning.activation.message.c_str());
    dma_display->clearScreen();
    drawIcon(connection_48, 8, 0, CONNECTION_48_WIDTH, CONNECTION_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("Startup Failed");
    return;
  }

  network_transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  // 配置回调函数
  xiaozhi::Callbacks callbacks;
  callbacks.on_event = onEvent;
  callbacks.on_wake_word = [](const std::string& wakeWord) {
    const auto wake = audioPort.wakeStats();
    Serial.printf("[wake] detected: %s at=%lu dispatch_ms=%lu hits=%lu\n",
                  wakeWord.c_str(), static_cast<unsigned long>(wake.lastDetectionMs),
                  static_cast<unsigned long>(millis() - wake.lastDetectionMs),
                  static_cast<unsigned long>(wake.detections));
  };
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("[xiaozhi] state=%s\n", xiaozhi::stateName(next));
  };
  callbacks.on_capture = [](bool enabled, const xiaozhi::AudioFormat& format) {
    const auto wake = audioPort.wakeStats();
    if (enabled && wake.detections != measuredWakeCaptureHits) {
      measuredWakeCaptureHits = wake.detections;
      Serial.printf("[wake-latency] capture_ms=%lu\n",
                    static_cast<unsigned long>(millis() - wake.lastDetectionMs));
    }
    Serial.printf("[xiaozhi] capture=%s format=%lu Hz/%u ch/%u ms\n",
                  enabled ? "on" : "off",
                  static_cast<unsigned long>(format.sample_rate),
                  format.channels, format.frame_duration_ms);
  };
  callbacks.on_audio_meta = [](const xiaozhi::AudioFrameMeta& meta) {
    const auto wake = audioPort.wakeStats();
    if (wake.detections != measuredWakeAudioHits) {
      measuredWakeAudioHits = wake.detections;
      Serial.printf("[wake-latency] first_downlink_ms=%lu\n",
                    static_cast<unsigned long>(millis() - wake.lastDetectionMs));
    }
    ++downlinkAudioFrames;
    downlinkAudioBytes += static_cast<uint32_t>(meta.opus_bytes);
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    if (code != xiaozhi::ErrorCode::TransportDisconnected) {
      Serial.println("[xiaozhi] transport disconnected");
    }
    Serial.printf("[xiaozhi] error[%s]: %s\n", xiaozhi::errorName(code),
                  message.c_str());
  };

  if (!client.attachAudioPort(&audioPort)) {
    Serial.println("[audio] failed to attach I2S/Opus audio port");
    dma_display->clearScreen();
    drawIcon(error_48, 8, 0, ERROR_48_WIDTH, ERROR_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("Audio Port Failed");
    return;
  }

  // MCP注册
  registerSetBrightnessMcp();
  registerGetBrightnessMcp();

  xiaozhi::ClientRuntimeConfig runtimeConfig;
  if (!runtime.begin(config, callbacks, runtimeConfig)) {
    Serial.println("CHECK SERIAL LOG");
    dma_display->clearScreen();
    drawIcon(error_48, 8, 0, ERROR_48_WIDTH, ERROR_48_HEIGHT);
    dma_display->setCursor(0, 48);
    dma_display->print("Startup Failed");
    return;
  }
  
  clientStarted = true;
  dma_display->clearScreen();
  Serial.println("[xiaozhi] ready, say 喵喵同学 to wakeup your device");

  xTaskCreatePinnedToCore(displayTask, "displayTask", 2048, NULL, 1, NULL, 0);
}

void loop() {
  if (clientStarted) {
    runtime.loop();
    pollDialogueTriggers();
  }

  const uint32_t now = millis();
  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf(
      "[heartbeat] wifi=%d client=%s audio=%lu/%lu internal_free=%lu, "
      "largest_internal_block=%lu, heap=%lu free_psram=%lu\n",
      static_cast<int>(WiFi.status()), runtime.stateName(), 
      static_cast<unsigned long>(downlinkAudioFrames),
      static_cast<unsigned long>(downlinkAudioBytes),
      esp_get_free_internal_heap_size(),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getFreePsram()));
    const auto wake = audioPort.wakeStats();
    Serial.printf("[wake-status] on=%u capture=%u reads=%lu partial=%lu frames=%lu "
                  "hits=%lu peak=%lu rms=%lu ch=%c ref=%lu clean=%lu aec=%lu aec_us=%lu aec_on=%u clip=%lu mic_db=%.0f\n",
                  wake.enabled, wake.capturing,
                  static_cast<unsigned long>(wake.reads),
                  static_cast<unsigned long>(wake.partialReads),
                  static_cast<unsigned long>(wake.frames),
                  static_cast<unsigned long>(wake.detections),
                  static_cast<unsigned long>(wake.peak),
                  static_cast<unsigned long>(wake.rms), wake.channel,
                  static_cast<unsigned long>(wake.referenceRms),
                  static_cast<unsigned long>(wake.cleanRms),
                  static_cast<unsigned long>(wake.aecFrames),
                  static_cast<unsigned long>(wake.maximumAecUs), wake.aecActive,
                  static_cast<unsigned long>(wake.clippedReads), wake.microphoneGainDb);
  }

  delay(1);
}