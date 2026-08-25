#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HomeSpan.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ir_Kelvinator.h>
#include <ping/ping_sock.h>
#include <lwip/ip_addr.h>
#include <sys/time.h>
#include <time.h>

#include "web_ui.h"

namespace {

constexpr uint16_t kReceiverPin = 4;
constexpr uint16_t kTransmitterPin = 5;
constexpr uint16_t kCaptureBufferSize = 1024;
constexpr uint8_t kCaptureTimeoutMs = 50;
constexpr uint8_t kMaxSchedules = 16;
constexpr uint16_t kTransmitRepeatCount = 2;
constexpr uint16_t kHomeKitPort = 1201;
constexpr uint8_t kAppMinTemp = 16;
constexpr uint8_t kAppMaxTemp = 28;
constexpr uint32_t kConfigMagic = 0x4143524D;
constexpr uint8_t kConfigVersion = 2;
constexpr uint32_t kPresenceMagic = 0x5052534E;
constexpr uint8_t kPresenceVersion = 2;
constexpr uint8_t kMaxPresenceDevices = 4;
constexpr uint16_t kPresenceProbeIntervalSeconds = 15;
constexpr uint16_t kPresenceMinAwaySeconds = 60;
constexpr uint16_t kPresenceMaxAwaySeconds = 3600;
constexpr uint8_t kPresenceArrivalSuccesses = 2;
constexpr char kSetupSsid[] = "AC-Remote-Setup";
constexpr char kSetupPassword[] = "acremote123";
constexpr char kHostname[] = "ac-remote";

struct AcState {
  uint8_t power = 0;
  uint8_t mode = kKelvinatorCool;
  uint8_t temp = 26;
  uint8_t fan = kKelvinatorFanAuto;
  uint8_t swingVertical = 1;
  uint8_t swingHorizontal = 0;
  uint8_t quiet = 0;
  uint8_t turbo = 0;
  uint8_t light = 1;
  uint8_t xfan = 0;
  uint8_t sleepMode = 0;
};

struct Schedule {
  uint32_t id = 0;
  uint8_t enabled = 1;
  uint8_t days = 0x7F;
  uint8_t hour = 0;
  uint8_t minute = 0;
  AcState state;
};

struct PersistedConfig {
  uint32_t magic = kConfigMagic;
  uint8_t version = kConfigVersion;
  uint8_t scheduleCount = 0;
  uint16_t reserved = 0;
  uint32_t nextScheduleId = 1;
  AcState state;
  Schedule schedules[kMaxSchedules];
};

struct PresenceConfigV1 {
  uint32_t magic = kPresenceMagic;
  uint8_t version = 1;
  uint8_t enabled = 0;
  uint8_t autoOn = 1;
  uint8_t autoOff = 1;
  uint16_t awaySeconds = 600;
  char targetIp[16] = {};
};

struct PresenceDeviceConfig {
  char name[16] = {};
  char targetIp[16] = {};
};

struct PresenceConfig {
  uint32_t magic = kPresenceMagic;
  uint8_t version = kPresenceVersion;
  uint8_t enabled = 0;
  uint8_t autoOn = 1;
  uint8_t autoOff = 1;
  uint16_t awaySeconds = 600;
  uint8_t deviceCount = 0;
  uint8_t reserved = 0;
  PresenceDeviceConfig devices[kMaxPresenceDevices];
};

enum class PresenceState : uint8_t { Unknown, Home, Away };

IRrecv receiver(kReceiverPin, kCaptureBufferSize, kCaptureTimeoutMs, true);
IRKelvinatorAC ac(kTransmitterPin);
decode_results irResults;
WebServer server(80);
Preferences configPreferences;
Preferences wifiPreferences;
PersistedConfig config;
PresenceConfig presenceConfig;
uint32_t lastRunMinute[kMaxSchedules] = {};
String lastSource = "boot";
String lastRemoteRawHex;
time_t lastStateEpoch = 0;
bool setupMode = false;
unsigned long lastScheduleCheckMs = 0;
unsigned long lastReconnectAttemptMs = 0;
unsigned long configDirtySinceMs = 0;
bool configDirty = false;
PresenceState presenceState = PresenceState::Unknown;
esp_ping_handle_t presencePingHandle = nullptr;
volatile bool presenceProbeRunning = false;
volatile bool presenceProbeSawReply = false;
volatile bool presenceProbeFinished = false;
volatile bool presenceProbeResult = false;
unsigned long presenceTrackingStartedMs = 0;
unsigned long presenceLastProbeCycleMs = 0;
unsigned long presenceLastSeenMs = 0;
time_t presenceLastSeenEpoch = 0;
unsigned long presenceDeviceLastSeenMs[kMaxPresenceDevices] = {};
time_t presenceDeviceLastSeenEpoch[kMaxPresenceDevices] = {};
uint8_t presenceDeviceSuccessStreak[kMaxPresenceDevices] = {};
uint8_t presenceCurrentProbeIndex = 0;
uint8_t presenceNextProbeIndex = 0;
bool presenceHasSeen = false;
bool homeKitEnabled = false;
HS_STATUS homeKitStatus = HS_WIFI_NEEDED;
SpanCharacteristic* hkCurrentState = nullptr;
SpanCharacteristic* hkTargetState = nullptr;
SpanCharacteristic* hkCurrentTemp = nullptr;
SpanCharacteristic* hkActive = nullptr;
SpanCharacteristic* hkCoolingTemp = nullptr;
SpanCharacteristic* hkHeatingTemp = nullptr;
SpanCharacteristic* hkFanOptions[5] = {};
SpanCharacteristic* hkSwingVertical = nullptr;
SpanCharacteristic* hkSwingHorizontal = nullptr;
SpanCharacteristic* hkDry = nullptr;
SpanCharacteristic* hkFanOnly = nullptr;
SpanCharacteristic* hkSleep[4] = {};

void syncHomeKitFromState(const AcState& state);

const char* modeName(uint8_t mode) {
  switch (mode) {
    case kKelvinatorAuto: return "auto";
    case kKelvinatorCool: return "cool";
    case kKelvinatorDry: return "dry";
    case kKelvinatorFan: return "fan";
    case kKelvinatorHeat: return "heat";
    default: return "cool";
  }
}

bool parseMode(const char* value, uint8_t& mode) {
  if (!value) return false;
  if (!strcmp(value, "auto")) mode = kKelvinatorAuto;
  else if (!strcmp(value, "cool")) mode = kKelvinatorCool;
  else if (!strcmp(value, "dry")) mode = kKelvinatorDry;
  else if (!strcmp(value, "fan")) mode = kKelvinatorFan;
  else if (!strcmp(value, "heat")) mode = kKelvinatorHeat;
  else return false;
  return true;
}

const char* fanName(uint8_t fan) {
  if (fan == kKelvinatorFanAuto) return "auto";
  if (fan <= 1) return "low";
  if (fan <= 3) return "medium";
  if (fan <= 4) return "high";
  return "max";
}

bool parseFan(const char* value, uint8_t& fan) {
  if (!value) return false;
  if (!strcmp(value, "auto")) fan = kKelvinatorFanAuto;
  else if (!strcmp(value, "low")) fan = 1;
  else if (!strcmp(value, "medium")) fan = 3;
  else if (!strcmp(value, "high")) fan = 4;
  else if (!strcmp(value, "max")) fan = kKelvinatorFanMax;
  else return false;
  return true;
}

void setDefaultConfig() {
  config = PersistedConfig{};
  config.state = AcState{};
}

void saveConfig() {
  configPreferences.putBytes("data", &config, sizeof(config));
  configDirty = false;
}

void loadConfig() {
  configPreferences.begin("ac-remote", false);
  if (configPreferences.getBytesLength("data") != sizeof(config)) {
    setDefaultConfig();
    saveConfig();
    return;
  }
  configPreferences.getBytes("data", &config, sizeof(config));
  if (config.magic != kConfigMagic || config.version != kConfigVersion ||
      config.scheduleCount > kMaxSchedules) {
    setDefaultConfig();
    saveConfig();
  }
}

void savePresenceConfig() {
  configPreferences.putBytes("presence", &presenceConfig,
                             sizeof(presenceConfig));
}

void loadPresenceConfig() {
  const size_t storedLength = configPreferences.getBytesLength("presence");
  if (storedLength == sizeof(presenceConfig)) {
    configPreferences.getBytes("presence", &presenceConfig,
                               sizeof(presenceConfig));
    for (uint8_t i = 0; i < kMaxPresenceDevices; ++i) {
      presenceConfig.devices[i].name[
          sizeof(presenceConfig.devices[i].name) - 1] = '\0';
      presenceConfig.devices[i].targetIp[
          sizeof(presenceConfig.devices[i].targetIp) - 1] = '\0';
    }
    if (presenceConfig.magic == kPresenceMagic &&
        presenceConfig.version == kPresenceVersion &&
        presenceConfig.deviceCount <= kMaxPresenceDevices &&
        presenceConfig.awaySeconds >= kPresenceMinAwaySeconds &&
        presenceConfig.awaySeconds <= kPresenceMaxAwaySeconds) {
      return;
    }
  } else if (storedLength == sizeof(PresenceConfigV1)) {
    PresenceConfigV1 oldConfig;
    configPreferences.getBytes("presence", &oldConfig, sizeof(oldConfig));
    oldConfig.targetIp[sizeof(oldConfig.targetIp) - 1] = '\0';
    if (oldConfig.magic == kPresenceMagic && oldConfig.version == 1 &&
        oldConfig.awaySeconds >= kPresenceMinAwaySeconds &&
        oldConfig.awaySeconds <= kPresenceMaxAwaySeconds) {
      presenceConfig = PresenceConfig{};
      presenceConfig.enabled = oldConfig.enabled;
      presenceConfig.autoOn = oldConfig.autoOn;
      presenceConfig.autoOff = oldConfig.autoOff;
      presenceConfig.awaySeconds = oldConfig.awaySeconds;
      if (oldConfig.targetIp[0]) {
        presenceConfig.deviceCount = 1;
        strlcpy(presenceConfig.devices[0].name, "手机 1",
                sizeof(presenceConfig.devices[0].name));
        strlcpy(presenceConfig.devices[0].targetIp, oldConfig.targetIp,
                sizeof(presenceConfig.devices[0].targetIp));
      }
      savePresenceConfig();
      return;
    }
  }

  {
    presenceConfig = PresenceConfig{};
    savePresenceConfig();
  }
}

void applyStateToAc(const AcState& state) {
  ac.setPower(state.power != 0);
  ac.setMode(state.mode);
  ac.setTemp(state.temp);
  ac.setFan(state.fan);
  ac.setSwingVertical(state.swingVertical != 0,
                      state.swingVertical ? kKelvinatorSwingVAuto
                                          : kKelvinatorSwingVOff);
  ac.setSwingHorizontal(state.swingHorizontal != 0);
  ac.setQuiet(state.quiet != 0);
  ac.setTurbo(state.turbo != 0);
  ac.setLight(state.light != 0);
  ac.setXFan(state.xfan != 0);
  uint8_t* raw = ac.getRaw();
  raw[0] &= 0x7F;
  raw[12] &= 0x80;  // Preserve Quiet, clear all sleep payload bits.
  raw[13] = 0;
  raw[14] &= 0xF0;  // Preserve fan bits, clear sleep payload.
  switch (state.sleepMode) {
    case 1:
      raw[0] |= 0x80;
      break;
    case 2:
      raw[12] |= 0x01;
      break;
    case 3:
      raw[0] |= 0x80;
      raw[12] |= 0x01;
      raw[13] = 0xA9;
      break;
    case 4:
      raw[0] |= 0x80;
      raw[12] |= 0x08;
      break;
  }
}

uint8_t readSleepMode(const uint8_t* raw) {
  const bool sleep13 = raw[0] & 0x80;
  const uint8_t payload12 = raw[12] & 0x7F;
  const bool payload3 = raw[13] != 0 || (raw[14] & 0x0F) != 0;
  if (sleep13) {
    if (payload12 == 0x08 && !payload3) return 4;
    if (payload12 != 0 || payload3) return 3;
    return 1;
  }
  if (payload12 & 0x01) return 2;
  return 0;
}

AcState readStateFromAc() {
  AcState state;
  state.power = ac.getPower();
  state.mode = ac.getMode();
  state.temp = ac.getTemp();
  state.fan = ac.getFan();
  state.swingVertical = ac.getSwingVerticalAuto();
  state.swingHorizontal = ac.getSwingHorizontal();
  state.quiet = ac.getQuiet();
  state.turbo = ac.getTurbo();
  state.light = ac.getLight();
  state.xfan = ac.getXFan();
  uint8_t* raw = ac.getRaw();
  state.sleepMode = readSleepMode(raw);
  return state;
}

void markStateChanged(const char* source) {
  lastSource = source;
  lastStateEpoch = time(nullptr);
  configDirty = true;
  configDirtySinceMs = millis();
}

void transmitState(const AcState& requestedState, const char* source) {
  applyStateToAc(requestedState);
  config.state = readStateFromAc();
  receiver.disableIRIn();
  ac.send(kTransmitRepeatCount);
  receiver.enableIRIn();
  markStateChanged(source);
  syncHomeKitFromState(config.state);
  Serial.printf("AC state transmitted (%s): %s\n", source,
                ac.toString().c_str());
}

const char* presenceStateName(PresenceState state) {
  switch (state) {
    case PresenceState::Home: return "home";
    case PresenceState::Away: return "away";
    default: return "unknown";
  }
}

void resetPresenceTracking() {
  presenceState = PresenceState::Unknown;
  presenceHasSeen = false;
  presenceLastSeenMs = 0;
  presenceLastSeenEpoch = 0;
  memset(presenceDeviceLastSeenMs, 0, sizeof(presenceDeviceLastSeenMs));
  memset(presenceDeviceLastSeenEpoch, 0, sizeof(presenceDeviceLastSeenEpoch));
  memset(presenceDeviceSuccessStreak, 0,
         sizeof(presenceDeviceSuccessStreak));
  presenceCurrentProbeIndex = 0;
  presenceNextProbeIndex = presenceConfig.deviceCount;
  presenceTrackingStartedMs = millis();
  presenceLastProbeCycleMs = millis() -
                             kPresenceProbeIntervalSeconds * 1000UL;
}

void onPresencePingSuccess(esp_ping_handle_t, void*) {
  presenceProbeSawReply = true;
}

void onPresencePingTimeout(esp_ping_handle_t, void*) {}

void onPresencePingEnd(esp_ping_handle_t, void*) {
  presenceProbeResult = presenceProbeSawReply;
  presenceProbeRunning = false;
  presenceProbeFinished = true;
}

void startPresenceProbe() {
  if (!presenceConfig.enabled || presenceProbeRunning || presencePingHandle ||
      WiFi.status() != WL_CONNECTED || !presenceConfig.deviceCount) {
    return;
  }

  if (presenceNextProbeIndex >= presenceConfig.deviceCount) {
    if (millis() - presenceLastProbeCycleMs <
        kPresenceProbeIntervalSeconds * 1000UL) {
      return;
    }
    presenceNextProbeIndex = 0;
    presenceLastProbeCycleMs = millis();
  }

  presenceCurrentProbeIndex = presenceNextProbeIndex++;
  const PresenceDeviceConfig& device =
      presenceConfig.devices[presenceCurrentProbeIndex];

  ip_addr_t targetAddress;
  if (!ipaddr_aton(device.targetIp, &targetAddress)) return;

  esp_ping_config_t pingConfig = ESP_PING_DEFAULT_CONFIG();
  pingConfig.target_addr = targetAddress;
  pingConfig.count = 2;
  pingConfig.interval_ms = 250;
  pingConfig.timeout_ms = 800;
  pingConfig.data_size = 16;

  esp_ping_callbacks_t callbacks = {};
  callbacks.on_ping_success = onPresencePingSuccess;
  callbacks.on_ping_timeout = onPresencePingTimeout;
  callbacks.on_ping_end = onPresencePingEnd;

  presenceProbeSawReply = false;
  presenceProbeFinished = false;
  presenceProbeRunning = true;
  if (esp_ping_new_session(&pingConfig, &callbacks, &presencePingHandle) !=
      ESP_OK) {
    presenceProbeRunning = false;
    presencePingHandle = nullptr;
    Serial.println("Presence probe: unable to create ping session");
    return;
  }
  if (esp_ping_start(presencePingHandle) != ESP_OK) {
    esp_ping_delete_session(presencePingHandle);
    presencePingHandle = nullptr;
    presenceProbeRunning = false;
    Serial.println("Presence probe: unable to start ping session");
  }
}

void applyPresenceTransition(PresenceState nextState) {
  if (nextState == presenceState) return;
  const PresenceState previousState = presenceState;
  presenceState = nextState;
  Serial.printf("Presence changed: %s -> %s\n", presenceStateName(previousState),
                presenceStateName(nextState));

  // Do not turn on an intentionally-off air conditioner merely because the
  // ESP32 restarted while someone was already home. An initial Away decision
  // is different: after the configured grace period, auto-off should still run.
  if (previousState == PresenceState::Unknown &&
      nextState == PresenceState::Home) {
    return;
  }

  AcState next = config.state;
  if (nextState == PresenceState::Home && presenceConfig.autoOn &&
      !next.power) {
    next.power = 1;
    transmitState(next, "presence-arrive");
  } else if (nextState == PresenceState::Away && presenceConfig.autoOff &&
             next.power) {
    next.power = 0;
    transmitState(next, "presence-leave");
  }
}

void processPresence() {
  if (presenceProbeFinished) {
    const bool found = presenceProbeResult;
    const uint8_t deviceIndex = presenceCurrentProbeIndex;
    presenceProbeFinished = false;
    if (presencePingHandle) {
      esp_ping_delete_session(presencePingHandle);
      presencePingHandle = nullptr;
    }
    if (presenceConfig.enabled && deviceIndex < presenceConfig.deviceCount) {
      if (found) {
        presenceHasSeen = true;
        presenceLastSeenMs = millis();
        presenceLastSeenEpoch = time(nullptr);
        presenceDeviceLastSeenMs[deviceIndex] = presenceLastSeenMs;
        presenceDeviceLastSeenEpoch[deviceIndex] = presenceLastSeenEpoch;
        if (presenceDeviceSuccessStreak[deviceIndex] <
            kPresenceArrivalSuccesses) {
          ++presenceDeviceSuccessStreak[deviceIndex];
        }
        if (presenceDeviceSuccessStreak[deviceIndex] >=
            kPresenceArrivalSuccesses) {
          applyPresenceTransition(PresenceState::Home);
        }
      } else {
        presenceDeviceSuccessStreak[deviceIndex] = 0;
      }
    }
  }

  if (!presenceConfig.enabled || WiFi.status() != WL_CONNECTED) return;

  const unsigned long awayReference =
      presenceHasSeen ? presenceLastSeenMs : presenceTrackingStartedMs;
  const bool probeCycleComplete =
      presenceNextProbeIndex >= presenceConfig.deviceCount &&
      !presenceProbeRunning && !presencePingHandle;
  if (probeCycleComplete &&
      millis() - awayReference >= presenceConfig.awaySeconds * 1000UL)
    applyPresenceTransition(PresenceState::Away);

  startPresenceProbe();
}

uint8_t homeKitTargetMode(const AcState& state) {
  if (state.mode == kKelvinatorHeat) return 1;
  if (state.mode == kKelvinatorAuto) return 0;
  return 2;
}

uint8_t homeKitCurrentMode(const AcState& state) {
  if (!state.power) return 0;
  if (state.mode == kKelvinatorHeat) return 2;
  if (state.mode == kKelvinatorCool || state.mode == kKelvinatorDry) return 3;
  return 1;
}

struct HomeKitAirConditioner : Service::HeaterCooler {
  HomeKitAirConditioner() : Service::HeaterCooler() {
    hkActive = new Characteristic::Active(0);
    hkCurrentTemp = new Characteristic::CurrentTemperature(26);
    hkCurrentState = new Characteristic::CurrentHeaterCoolerState(0);
    hkTargetState = new Characteristic::TargetHeaterCoolerState(0);
    hkCoolingTemp = new Characteristic::CoolingThresholdTemperature(26);
    hkCoolingTemp->setRange(kAppMinTemp, kAppMaxTemp, 1);
    hkHeatingTemp = new Characteristic::HeatingThresholdTemperature(26);
    hkHeatingTemp->setRange(kAppMinTemp, kAppMaxTemp, 1);
    new Characteristic::TemperatureDisplayUnits(0);
    new Characteristic::ConfiguredName("空调控制");
  }

  boolean update() override {
    AcState next = config.state;
    if (hkActive->updated()) next.power = hkActive->getNewVal() != 0;
    if (hkTargetState->updated()) {
      const uint8_t target = hkTargetState->getNewVal();
      if (target == 0) next.mode = kKelvinatorAuto;
      else if (target == 1) next.mode = kKelvinatorHeat;
      else next.mode = kKelvinatorCool;
    }
    if (hkCoolingTemp->updated())
      next.temp = constrain(static_cast<int>(lround(hkCoolingTemp->getNewVal<float>())),
                            kAppMinTemp, kAppMaxTemp);
    if (hkHeatingTemp->updated())
      next.temp = constrain(static_cast<int>(lround(hkHeatingTemp->getNewVal<float>())),
                            kAppMinTemp, kAppMaxTemp);
    transmitState(next, "homekit");
    return true;
  }
};

struct HomeKitFanOption : Service::Switch {
  SpanCharacteristic* on;
  uint8_t fan;

  HomeKitFanOption(const char* name, uint8_t targetFan)
      : Service::Switch(), fan(targetFan) {
    on = new Characteristic::On(false);
    new Characteristic::ConfiguredName(name);
  }

  boolean update() override {
    // Fan speed is a radio-style choice: the selected option cannot be
    // switched off without selecting another speed.
    if (!on->getNewVal()) return config.state.fan != fan;
    AcState next = config.state;
    next.fan = fan;
    transmitState(next, "homekit");
    return true;
  }
};

struct HomeKitSwingSwitch : Service::Switch {
  SpanCharacteristic* on;
  bool horizontal;

  HomeKitSwingSwitch(const char* name, bool controlsHorizontal)
      : Service::Switch(), horizontal(controlsHorizontal) {
    on = new Characteristic::On(false);
    new Characteristic::ConfiguredName(name);
  }

  boolean update() override {
    AcState next = config.state;
    if (horizontal)
      next.swingHorizontal = on->getNewVal() != 0;
    else
      next.swingVertical = on->getNewVal() != 0;
    transmitState(next, "homekit");
    return true;
  }
};

struct HomeKitModeSwitch : Service::Switch {
  SpanCharacteristic* on;
  uint8_t mode;

  HomeKitModeSwitch(const char* name, uint8_t targetMode)
      : Service::Switch(), mode(targetMode) {
    on = new Characteristic::On(false);
    new Characteristic::ConfiguredName(name);
  }

  boolean update() override {
    AcState next = config.state;
    if (on->getNewVal()) {
      next.power = 1;
      next.mode = mode;
    } else if (next.mode == mode) {
      next.power = 0;
    }
    transmitState(next, "homekit");
    return true;
  }
};

struct HomeKitSleepSwitch : Service::Switch {
  SpanCharacteristic* on;
  uint8_t sleepMode;

  HomeKitSleepSwitch(const char* name, uint8_t targetMode)
      : Service::Switch(), sleepMode(targetMode) {
    on = new Characteristic::On(false);
    new Characteristic::ConfiguredName(name);
  }

  boolean update() override {
    AcState next = config.state;
    if (on->getNewVal()) {
      next.sleepMode = sleepMode;
    } else if (next.sleepMode == sleepMode) {
      next.sleepMode = 0;
    }
    transmitState(next, "homekit");
    return true;
  }
};

void syncHomeKitFromState(const AcState& state) {
  if (!homeKitEnabled || !hkTargetState) return;
  hkCurrentState->setVal(homeKitCurrentMode(state));
  hkTargetState->setVal(homeKitTargetMode(state));
  // No room-temperature sensor is fitted; expose the configured setpoint.
  hkCurrentTemp->setVal(state.temp);
  hkActive->setVal(state.power != 0);
  hkCoolingTemp->setVal(state.temp);
  hkHeatingTemp->setVal(state.temp);
  const uint8_t fanValues[] = {kKelvinatorFanAuto, 1, 3, 4,
                               kKelvinatorFanMax};
  for (uint8_t i = 0; i < 5; ++i)
    hkFanOptions[i]->setVal(state.fan == fanValues[i]);
  hkSwingVertical->setVal(state.swingVertical != 0);
  hkSwingHorizontal->setVal(state.swingHorizontal != 0);
  hkDry->setVal(state.power && state.mode == kKelvinatorDry);
  hkFanOnly->setVal(state.power && state.mode == kKelvinatorFan);
  for (uint8_t i = 0; i < 4; ++i)
    hkSleep[i]->setVal(state.sleepMode == i + 1);
}

void addHomeKitAccessoryInfo(const char* name) {
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(name);
  new Characteristic::Manufacturer("DIY ESP32");
  new Characteristic::Model("Gree YAP0F21 IR Bridge");
  new Characteristic::FirmwareRevision("1.0.0");
}

void onHomeKitStatus(HS_STATUS status) {
  homeKitStatus = status;
  Serial.printf("HomeKit: %s\n", homeSpan.statusString(status));
}

void onHomeKitWifiConnected() {
  MDNS.addService("http", "tcp", 80);
  configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org",
               "time.cloudflare.com");
  Serial.printf("Web UI ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void setupHomeKit() {
  wifiPreferences.begin("wifi", true);
  String ssid = wifiPreferences.getString("ssid", "");
  String password = wifiPreferences.getString("password", "");
  wifiPreferences.end();
  if (ssid.isEmpty()) return;

  homeSpan.setLogLevel(1);
  homeSpan.setPortNum(kHomeKitPort);
  homeSpan.setHostNameSuffix("");
  homeSpan.setWifiCredentials(ssid.c_str(), password.c_str());
  homeSpan.setWifiCallback(onHomeKitWifiConnected);
  homeSpan.setStatusCallback(onHomeKitStatus);
  homeSpan.begin(Category::AirConditioners, "格力空调", kHostname,
                 "Gree YAP0F21 IR Bridge");

  new SpanAccessory();
  addHomeKitAccessoryInfo("格力空调");
  (new HomeKitAirConditioner())->setPrimary();

  const char* fanNames[] = {"风速自动", "风速低", "风速中", "风速高", "风速最大"};
  const uint8_t fanValues[] = {kKelvinatorFanAuto, 1, 3, 4,
                               kKelvinatorFanMax};
  for (uint8_t i = 0; i < 5; ++i)
    hkFanOptions[i] = (new HomeKitFanOption(fanNames[i], fanValues[i]))->on;

  hkSwingVertical = (new HomeKitSwingSwitch("上下扫风", false))->on;
  hkSwingHorizontal = (new HomeKitSwingSwitch("左右扫风", true))->on;

  hkDry = (new HomeKitModeSwitch("除湿", kKelvinatorDry))->on;
  hkFanOnly = (new HomeKitModeSwitch("送风", kKelvinatorFan))->on;

  const char* sleepNames[] = {"睡眠1", "睡眠2", "睡眠3", "睡眠4"};
  for (uint8_t i = 0; i < 4; ++i) {
    hkSleep[i] = (new HomeKitSleepSwitch(sleepNames[i], i + 1))->on;
  }

  homeKitEnabled = true;
  syncHomeKitFromState(config.state);
}

void addStateJson(JsonObject object, const AcState& state) {
  object["power"] = state.power != 0;
  object["mode"] = modeName(state.mode);
  object["temp"] = state.temp;
  object["fan"] = fanName(state.fan);
  object["swingVertical"] = state.swingVertical != 0;
  object["swingHorizontal"] = state.swingHorizontal != 0;
  object["quiet"] = state.quiet != 0;
  object["turbo"] = state.turbo != 0;
  object["light"] = state.light != 0;
  object["xfan"] = state.xfan != 0;
  object["sleepMode"] = state.sleepMode;
  object["sleep"] = state.sleepMode != 0;  // Backward-compatible API field.
}

bool updateStateFromJson(JsonObjectConst object, AcState& state, String& error) {
  if (object["power"].is<bool>()) state.power = object["power"].as<bool>();
  if (object["mode"].is<const char*>() &&
      !parseMode(object["mode"].as<const char*>(), state.mode)) {
    error = "invalid mode";
    return false;
  }
  if (object["temp"].is<int>()) {
    int temp = object["temp"].as<int>();
    if (temp < kAppMinTemp || temp > kAppMaxTemp) {
      error = "temp must be 16..28";
      return false;
    }
    state.temp = temp;
  }
  if (object["fan"].is<const char*>() &&
      !parseFan(object["fan"].as<const char*>(), state.fan)) {
    error = "invalid fan";
    return false;
  }
  if (object["swingVertical"].is<bool>())
    state.swingVertical = object["swingVertical"].as<bool>();
  if (object["swingHorizontal"].is<bool>())
    state.swingHorizontal = object["swingHorizontal"].as<bool>();
  if (object["quiet"].is<bool>()) state.quiet = object["quiet"].as<bool>();
  if (object["turbo"].is<bool>()) state.turbo = object["turbo"].as<bool>();
  if (object["light"].is<bool>()) state.light = object["light"].as<bool>();
  if (object["xfan"].is<bool>()) state.xfan = object["xfan"].as<bool>();
  if (object["sleepMode"].is<int>()) {
    int sleepMode = object["sleepMode"].as<int>();
    if (sleepMode < 0 || sleepMode > 4) {
      error = "sleepMode must be 0..4";
      return false;
    }
    state.sleepMode = sleepMode;
  } else if (object["sleep"].is<bool>()) {
    state.sleepMode = object["sleep"].as<bool>() ? 1 : 0;
  }
  return true;
}

void sendJson(JsonDocument& document, int status = 200) {
  String payload;
  serializeJson(document, payload);
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", payload);
}

void sendError(int status, const String& message) {
  JsonDocument document;
  document["ok"] = false;
  document["error"] = message;
  sendJson(document, status);
}

bool parseJsonBody(JsonDocument& document) {
  DeserializationError error = deserializeJson(document, server.arg("plain"));
  if (error) {
    sendError(400, String("invalid JSON: ") + error.c_str());
    return false;
  }
  return true;
}

bool getLocalClock(tm& value) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &value);
  return true;
}

String localTimeText() {
  tm now;
  if (!getLocalClock(now)) return "未同步";
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &now);
  return String(buffer);
}

String rawStateHex(const uint8_t* raw) {
  static const char digits[] = "0123456789ABCDEF";
  String result;
  result.reserve(kKelvinatorStateLength * 2);
  for (uint8_t i = 0; i < kKelvinatorStateLength; ++i) {
    result += digits[raw[i] >> 4];
    result += digits[raw[i] & 0x0F];
  }
  return result;
}

void handleStateGet() {
  JsonDocument document;
  document["ok"] = true;
  JsonObject state = document["state"].to<JsonObject>();
  addStateJson(state, config.state);
  document["source"] = lastSource;
  document["updatedAt"] = static_cast<int64_t>(lastStateEpoch);
  document["time"] = localTimeText();
  document["timeSynced"] = time(nullptr) >= 1700000000;
  document["ip"] = setupMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  document["hostname"] = String(kHostname) + ".local";
  document["remoteRaw"] = lastRemoteRawHex;
  sendJson(document);
}

void handleControlPost() {
  JsonDocument document;
  if (!parseJsonBody(document)) return;
  if (!document.is<JsonObject>()) {
    sendError(400, "JSON object required");
    return;
  }
  AcState next = config.state;
  String error;
  if (!updateStateFromJson(document.as<JsonObjectConst>(), next, error)) {
    sendError(400, error);
    return;
  }
  transmitState(next, "api");
  handleStateGet();
}

void addScheduleJson(JsonObject object, const Schedule& schedule) {
  object["id"] = schedule.id;
  object["enabled"] = schedule.enabled != 0;
  object["days"] = schedule.days;
  object["hour"] = schedule.hour;
  object["minute"] = schedule.minute;
  JsonObject state = object["state"].to<JsonObject>();
  addStateJson(state, schedule.state);
}

void handleSchedulesGet() {
  JsonDocument document;
  document["ok"] = true;
  JsonArray schedules = document["schedules"].to<JsonArray>();
  for (uint8_t i = 0; i < config.scheduleCount; ++i) {
    JsonObject item = schedules.add<JsonObject>();
    addScheduleJson(item, config.schedules[i]);
  }
  sendJson(document);
}

int findScheduleIndex(uint32_t id) {
  for (uint8_t i = 0; i < config.scheduleCount; ++i) {
    if (config.schedules[i].id == id) return i;
  }
  return -1;
}

void handleSchedulesPost() {
  JsonDocument document;
  if (!parseJsonBody(document)) return;
  JsonObjectConst body = document.as<JsonObjectConst>();
  uint32_t id = body["id"] | 0;
  int index = id ? findScheduleIndex(id) : -1;
  bool creating = index < 0;
  if (creating && config.scheduleCount >= kMaxSchedules) {
    sendError(409, "maximum 16 schedules");
    return;
  }

  Schedule schedule;
  if (!creating) schedule = config.schedules[index];
  else {
    schedule.id = config.nextScheduleId++;
    schedule.state = config.state;
  }
  if (body["enabled"].is<bool>()) schedule.enabled = body["enabled"].as<bool>();
  int days = body["days"] | static_cast<int>(schedule.days);
  int hour = body["hour"] | static_cast<int>(schedule.hour);
  int minute = body["minute"] | static_cast<int>(schedule.minute);
  if (days < 1 || days > 0x7F || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    sendError(400, "invalid days/hour/minute");
    return;
  }
  schedule.days = days;
  schedule.hour = hour;
  schedule.minute = minute;
  if (body["state"].is<JsonObjectConst>()) {
    String error;
    if (!updateStateFromJson(body["state"].as<JsonObjectConst>(), schedule.state,
                             error)) {
      sendError(400, error);
      return;
    }
  }

  if (creating) {
    config.schedules[config.scheduleCount] = schedule;
    lastRunMinute[config.scheduleCount] = 0;
    ++config.scheduleCount;
  } else {
    config.schedules[index] = schedule;
    lastRunMinute[index] = 0;
  }
  saveConfig();
  JsonDocument response;
  response["ok"] = true;
  JsonObject result = response["schedule"].to<JsonObject>();
  addScheduleJson(result, schedule);
  sendJson(response, creating ? 201 : 200);
}

void handleSchedulesDelete() {
  uint32_t id = server.arg("id").toInt();
  int index = findScheduleIndex(id);
  if (index < 0) {
    sendError(404, "schedule not found");
    return;
  }
  for (uint8_t i = index; i + 1 < config.scheduleCount; ++i) {
    config.schedules[i] = config.schedules[i + 1];
    lastRunMinute[i] = lastRunMinute[i + 1];
  }
  --config.scheduleCount;
  saveConfig();
  JsonDocument response;
  response["ok"] = true;
  sendJson(response);
}

void handleTimeGet() {
  JsonDocument document;
  document["ok"] = true;
  document["epoch"] = static_cast<int64_t>(time(nullptr));
  document["local"] = localTimeText();
  document["synced"] = time(nullptr) >= 1700000000;
  sendJson(document);
}

void handleTimePost() {
  JsonDocument document;
  if (!parseJsonBody(document)) return;
  int64_t epoch = document["epoch"] | 0;
  if (epoch < 1700000000) {
    sendError(400, "invalid epoch");
    return;
  }
  timeval value{static_cast<time_t>(epoch), 0};
  settimeofday(&value, nullptr);
  handleTimeGet();
}

void handleInfoGet() {
  JsonDocument document;
  document["ok"] = true;
  document["device"] = "ESP32-S3 AC Remote";
  document["protocol"] = "KELVINATOR/GREE YAP0F21";
  document["receiverPin"] = kReceiverPin;
  document["transmitterPin"] = kTransmitterPin;
  document["wifiRssi"] = setupMode ? 0 : WiFi.RSSI();
  document["ip"] = setupMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  sendJson(document);
}

bool validPresenceIp(const String& text, IPAddress& address) {
  if (!address.fromString(text)) return false;
  if (address[0] == 0 || address[0] == 127 || address[0] >= 224) return false;
  if (!setupMode && address == WiFi.localIP()) return false;
  return true;
}

bool anyPresenceDeviceOnline() {
  for (uint8_t i = 0; i < presenceConfig.deviceCount; ++i) {
    if (presenceDeviceSuccessStreak[i]) return true;
  }
  return false;
}

uint32_t presenceAwayRemainingSeconds() {
  if (!presenceConfig.enabled || !presenceConfig.deviceCount ||
      presenceState == PresenceState::Away) {
    return 0;
  }
  const unsigned long awayReference =
      presenceHasSeen ? presenceLastSeenMs : presenceTrackingStartedMs;
  const uint32_t elapsedSeconds = (millis() - awayReference) / 1000UL;
  if (elapsedSeconds >= presenceConfig.awaySeconds) return 0;
  return presenceConfig.awaySeconds - elapsedSeconds;
}

void handlePresenceGet() {
  JsonDocument document;
  document["ok"] = true;
  document["enabled"] = presenceConfig.enabled != 0;
  document["targetIp"] = presenceConfig.deviceCount
                             ? presenceConfig.devices[0].targetIp
                             : "";
  document["requestIp"] = server.client().remoteIP().toString();
  document["autoOn"] = presenceConfig.autoOn != 0;
  document["autoOff"] = presenceConfig.autoOff != 0;
  document["awaySeconds"] = presenceConfig.awaySeconds;
  document["probeIntervalSeconds"] = kPresenceProbeIntervalSeconds;
  document["status"] = presenceStateName(presenceState);
  document["airConditionerPower"] = config.state.power != 0;
  const bool anyOnline = anyPresenceDeviceOnline();
  document["anyDeviceOnline"] = anyOnline;
  const uint32_t awayRemainingSeconds = presenceAwayRemainingSeconds();
  const bool autoOffPending = presenceConfig.enabled &&
                              presenceConfig.autoOff && config.state.power &&
                              !anyOnline &&
                              presenceState != PresenceState::Away;
  document["autoOffPending"] = autoOffPending;
  if (autoOffPending) {
    document["awayRemainingSeconds"] = awayRemainingSeconds;
    document["expectedCloseAt"] =
        static_cast<int64_t>(time(nullptr) + awayRemainingSeconds);
  } else {
    document["awayRemainingSeconds"] = nullptr;
    document["expectedCloseAt"] = nullptr;
  }
  document["lastSeenAt"] = static_cast<int64_t>(presenceLastSeenEpoch);
  if (presenceHasSeen)
    document["lastSeenAgoSeconds"] = (millis() - presenceLastSeenMs) / 1000UL;
  else
    document["lastSeenAgoSeconds"] = nullptr;
  JsonArray devices = document["devices"].to<JsonArray>();
  for (uint8_t i = 0; i < presenceConfig.deviceCount; ++i) {
    JsonObject item = devices.add<JsonObject>();
    item["name"] = presenceConfig.devices[i].name;
    item["targetIp"] = presenceConfig.devices[i].targetIp;
    item["successStreak"] = presenceDeviceSuccessStreak[i];
    item["lastSeenAt"] = static_cast<int64_t>(presenceDeviceLastSeenEpoch[i]);
    if (presenceDeviceLastSeenMs[i]) {
      const unsigned long ageSeconds =
          (millis() - presenceDeviceLastSeenMs[i]) / 1000UL;
      item["lastSeenAgoSeconds"] = ageSeconds;
      item["online"] = presenceDeviceSuccessStreak[i] != 0;
    } else {
      item["lastSeenAgoSeconds"] = nullptr;
      item["online"] = false;
    }
  }
  sendJson(document);
}

void handlePresencePost() {
  JsonDocument document;
  if (!parseJsonBody(document)) return;
  if (!document.is<JsonObject>()) {
    sendError(400, "JSON object required");
    return;
  }

  JsonObjectConst body = document.as<JsonObjectConst>();
  PresenceConfig next = presenceConfig;
  if (body["enabled"].is<bool>()) next.enabled = body["enabled"].as<bool>();
  if (body["autoOn"].is<bool>()) next.autoOn = body["autoOn"].as<bool>();
  if (body["autoOff"].is<bool>()) next.autoOff = body["autoOff"].as<bool>();
  if (body["awaySeconds"].is<int>()) {
    const int awaySeconds = body["awaySeconds"].as<int>();
    if (awaySeconds < kPresenceMinAwaySeconds ||
        awaySeconds > kPresenceMaxAwaySeconds) {
      sendError(400, "awaySeconds must be 60..3600");
      return;
    }
    next.awaySeconds = awaySeconds;
  }

  if (body["devices"].is<JsonArrayConst>()) {
    JsonArrayConst devices = body["devices"].as<JsonArrayConst>();
    if (devices.size() > kMaxPresenceDevices) {
      sendError(400, "maximum 4 presence devices");
      return;
    }
    next.deviceCount = 0;
    memset(next.devices, 0, sizeof(next.devices));
    for (JsonObjectConst item : devices) {
      String name = item["name"] | "";
      String targetIp = item["targetIp"] | "";
      name.trim();
      targetIp.trim();
      IPAddress parsed;
      if (!validPresenceIp(targetIp, parsed)) {
        sendError(400, "invalid device targetIp");
        return;
      }
      if (name.length() >= sizeof(next.devices[0].name)) {
        sendError(400, "device name too long");
        return;
      }
      for (uint8_t i = 0; i < next.deviceCount; ++i) {
        if (targetIp == next.devices[i].targetIp) {
          sendError(400, "duplicate device targetIp");
          return;
        }
      }
      PresenceDeviceConfig& device = next.devices[next.deviceCount];
      if (name.isEmpty()) name = "手机 " + String(next.deviceCount + 1);
      strlcpy(device.name, name.c_str(), sizeof(device.name));
      strlcpy(device.targetIp, targetIp.c_str(), sizeof(device.targetIp));
      ++next.deviceCount;
    }
  } else if (body["targetIp"].is<const char*>()) {
    // Backward compatibility for the single-device API.
    String targetIp = body["targetIp"].as<const char*>();
    targetIp.trim();
    IPAddress parsed;
    if (!targetIp.isEmpty() && !validPresenceIp(targetIp, parsed)) {
      sendError(400, "invalid targetIp");
      return;
    }
    next.deviceCount = targetIp.isEmpty() ? 0 : 1;
    memset(next.devices, 0, sizeof(next.devices));
    if (!targetIp.isEmpty()) {
      strlcpy(next.devices[0].name, "手机 1",
              sizeof(next.devices[0].name));
      strlcpy(next.devices[0].targetIp, targetIp.c_str(),
              sizeof(next.devices[0].targetIp));
    }
  }
  if (next.enabled && !next.deviceCount) {
    sendError(400, "at least one device required when enabled");
    return;
  }

  presenceConfig = next;
  savePresenceConfig();
  resetPresenceTracking();
  handlePresenceGet();
}

bool validHomeKitCode(const String& code) {
  if (code.length() != 8) return false;
  for (char c : code)
    if (!isDigit(c)) return false;
  const char* blocked[] = {"00000000", "11111111", "22222222", "33333333",
                           "44444444", "55555555", "66666666", "77777777",
                           "88888888", "99999999", "12345678", "87654321"};
  for (const char* value : blocked)
    if (code == value) return false;
  return true;
}

void handleHomeKitGet() {
  JsonDocument document;
  document["ok"] = true;
  document["enabled"] = homeKitEnabled;
  document["paired"] = homeKitStatus == HS_PAIRED;
  document["status"] = homeKitEnabled ? homeSpan.statusString(homeKitStatus)
                                      : "disabled";
  document["codeConfigured"] = configPreferences.getBool("hk-code", false);
  sendJson(document);
}

void handleHomeKitPost() {
  if (!homeKitEnabled) {
    sendError(503, "HomeKit unavailable");
    return;
  }
  if (homeKitStatus == HS_PAIRED) {
    sendError(409, "unpair HomeKit before changing setup code");
    return;
  }
  JsonDocument document;
  if (!parseJsonBody(document)) return;
  String code = document["code"] | "";
  code.trim();
  if (!validHomeKitCode(code)) {
    sendError(400, "code must be 8 non-trivial digits");
    return;
  }
  homeSpan.setPairingCode(code.c_str(), false);
  configPreferences.putBool("hk-code", true);
  JsonDocument response;
  response["ok"] = true;
  response["message"] = "HomeKit setup code saved";
  sendJson(response);
}

void handleWifiSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64) {
    server.send(400, "text/plain; charset=utf-8", "Wi-Fi 信息无效");
    return;
  }
  wifiPreferences.begin("wifi", false);
  wifiPreferences.putString("ssid", ssid);
  wifiPreferences.putString("password", password);
  wifiPreferences.end();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><h2>已保存，设备正在重启…</h2>");
  delay(700);
  ESP.restart();
}

void handleNotFound() {
  if (setupMode) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  sendError(404, "not found");
}

void registerRoutes() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", setupMode ? kSetupPage : kWebUi);
  });
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/state", HTTP_GET, handleStateGet);
  server.on("/api/control", HTTP_POST, handleControlPost);
  server.on("/api/schedules", HTTP_GET, handleSchedulesGet);
  server.on("/api/schedules", HTTP_POST, handleSchedulesPost);
  server.on("/api/schedules", HTTP_DELETE, handleSchedulesDelete);
  server.on("/api/time", HTTP_GET, handleTimeGet);
  server.on("/api/time", HTTP_POST, handleTimePost);
  server.on("/api/info", HTTP_GET, handleInfoGet);
  server.on("/api/presence", HTTP_GET, handlePresenceGet);
  server.on("/api/presence", HTTP_POST, handlePresencePost);
  server.on("/api/homekit", HTTP_GET, handleHomeKitGet);
  server.on("/api/homekit", HTTP_POST, handleHomeKitPost);
  server.onNotFound(handleNotFound);
}

void startNetwork() {
  wifiPreferences.begin("wifi", true);
  String ssid = wifiPreferences.getString("ssid", "");
  String password = wifiPreferences.getString("password", "");
  wifiPreferences.end();
  if (!ssid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kHostname);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.printf("Connecting to Wi-Fi: %s", ssid.c_str());
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    setupMode = false;
    Serial.printf("Wi-Fi connected: http://%s/\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(kHostname)) MDNS.addService("http", "tcp", 80);
    configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
  } else {
    setupMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kSetupSsid, kSetupPassword);
    Serial.printf("Setup AP ready: %s, http://%s/\n", kSetupSsid,
                  WiFi.softAPIP().toString().c_str());
  }
}

void handleIrReceive() {
  if (!receiver.decode(&irResults)) return;
  if (irResults.decode_type == KELVINATOR && irResults.bits == kKelvinatorBits &&
      IRKelvinatorAC::validChecksum(irResults.state)) {
    lastRemoteRawHex = rawStateHex(irResults.state);
    ac.setRaw(irResults.state);
    config.state = readStateFromAc();
    markStateChanged("remote");
    syncHomeKitFromState(config.state);
    Serial.printf("Original remote synchronized: %s, Sleep mode: %u, RAW: %s\n",
                  ac.toString().c_str(), config.state.sleepMode,
                  lastRemoteRawHex.c_str());
  }
  receiver.resume();
}

void runSchedules() {
  if (millis() - lastScheduleCheckMs < 1000) return;
  lastScheduleCheckMs = millis();
  tm now;
  if (!getLocalClock(now)) return;
  uint32_t minuteToken = static_cast<uint32_t>(time(nullptr) / 60);
  for (uint8_t i = 0; i < config.scheduleCount; ++i) {
    Schedule& schedule = config.schedules[i];
    bool dayMatches = schedule.days & (1U << now.tm_wday);
    bool timeMatches = schedule.hour == now.tm_hour && schedule.minute == now.tm_min;
    if (schedule.enabled && dayMatches && timeMatches &&
        lastRunMinute[i] != minuteToken) {
      lastRunMinute[i] = minuteToken;
      transmitState(schedule.state, "schedule");
    }
  }
}

void keepWifiConnected() {
  if (homeKitEnabled || setupMode || WiFi.status() == WL_CONNECTED ||
      millis() - lastReconnectAttemptMs < 30000) return;
  lastReconnectAttemptMs = millis();
  WiFi.reconnect();
}

void flushConfigIfNeeded() {
  if (configDirty && millis() - configDirtySinceMs >= 2000) saveConfig();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  loadConfig();
  loadPresenceConfig();
  resetPresenceTracking();
  ac.begin();
  applyStateToAc(config.state);
  receiver.enableIRIn();
  startNetwork();
  if (!setupMode) setupHomeKit();
  registerRoutes();
  server.begin();
  Serial.println("AC Remote web/API server ready");
}

void loop() {
  if (homeKitEnabled) homeSpan.poll();
  server.handleClient();
  handleIrReceive();
  runSchedules();
  processPresence();
  keepWifiConnected();
  flushConfigIfNeeded();
  delay(2);
}
