#include "ble_remote.hpp"

#include <cstring>

namespace {

constexpr char kName[] = "M5 Media Native";
// Byte-for-byte port of the working Consumer Control + Keyboard report map.
uint8_t report_map[] = {
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01,
  0x09, 0x02, 0xA1, 0x02,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x0A,
  0x15, 0x01, 0x25, 0x0A, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
  0xC0,
  0x05, 0x0C, 0x09, 0x86, 0x15, 0xFF, 0x25, 0x01,
  0x75, 0x02, 0x95, 0x01, 0x81, 0x46,
  0x09, 0xE9, 0x09, 0xEA,
  0x15, 0x00, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x09, 0xE2, 0x09, 0x30, 0x09, 0x83, 0x09, 0x81,
  0x09, 0xB0, 0x09, 0xB1, 0x09, 0xB2, 0x09, 0xB3,
  0x09, 0xB4, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,
  0x09, 0xCD,
  0x15, 0x01, 0x25, 0x0D, 0x75, 0x04, 0x95, 0x01, 0x81, 0x00,
  0x09, 0x80, 0xA1, 0x02,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
  0x15, 0x01, 0x25, 0x03, 0x75, 0x02, 0x81, 0x00,
  0xC0,
  0x81, 0x03,
  0xC0,
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
  0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
  0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
  0xC0,
};

}  // namespace

String BleRemote::staticIdentity() {
  // Stable random-static identity distinct from UiFlow's public address. The
  // chip's factory MAC is mixed with a project constant so it also survives
  // an NVS reset; NimBLE keeps the actual peer bonds in its own NVS store.
  uint64_t value = ESP.getEfuseMac() ^ 0x6a09e667f3bcc909ULL;
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  uint8_t bytes[6];
  for (int i = 0; i < 6; ++i) bytes[i] = static_cast<uint8_t>(value >> (40 - 8 * i));
  bytes[0] = (bytes[0] & 0x3f) | 0xc0;
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return String(buffer);
}

void BleRemote::emitEvent(const Event& event) {
  if (events_) xQueueSend(events_, &event, 0);
}

void BleRemote::ServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo& info) {
  remote_.emitEvent({EventType::connect, info.getConnHandle()});
}

void BleRemote::ServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) {
  remote_.emitEvent({EventType::disconnect, info.getConnHandle(), reason});
}

void BleRemote::ServerCallbacks::onAuthenticationComplete(NimBLEConnInfo& info) {
  const uint8_t flags = (info.isEncrypted() ? 1 : 0) | (info.isBonded() ? 2 : 0);
  remote_.emitEvent({EventType::auth, info.getConnHandle(), 0, flags, info.getSecKeySize()});
}

void BleRemote::ServerCallbacks::onIdentity(NimBLEConnInfo& info) {
  remote_.emitEvent({EventType::identity, info.getConnHandle()});
}

void BleRemote::CharacteristicCallbacks::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
  const auto value = characteristic->getValue();
  if (value.size() != 1) return;
  const auto type = characteristic == remote_.control_char_ ? EventType::control : EventType::protocol;
  remote_.emitEvent({type, info.getConnHandle(), value.data()[0]});
}

void BleRemote::CharacteristicCallbacks::onSubscribe(NimBLECharacteristic* characteristic,
                                                       NimBLEConnInfo& info, uint16_t value) {
  int report_id = characteristic == remote_.consumer_ ? 1 :
                  characteristic == remote_.keyboard_ ? 2 :
                  characteristic == remote_.boot_input_ ? 3 : 0;
  remote_.emitEvent({EventType::subscribe, info.getConnHandle(), (report_id << 16) | value});
}

bool BleRemote::begin(uint8_t battery_level) {
  events_ = xQueueCreate(16, sizeof(Event));
  if (!events_) return false;
  const String identity = staticIdentity();
  if (identity.length() != 17 || !NimBLEDevice::init(kName)) return false;
  if (!NimBLEDevice::setOwnAddr(NimBLEAddress(identity.c_str(), BLE_ADDR_RANDOM)) ||
      !NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)) return false;

  NimBLEDevice::setSecurityAuth(true, false, true);  // Bonded LE Secure Connections, Just Works.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  server_ = NimBLEDevice::createServer();
  if (!server_) return false;
  server_->setCallbacks(&server_callbacks_, false);
  server_->advertiseOnDisconnect(false);

  hid_ = new NimBLEHIDDevice(server_);
  hid_->setManufacturer("DIY");
  hid_->setPnp(1, 0xffff, 1, 0x0101);
  hid_->setHidInfo(0, 0x02);
  hid_->setReportMap(report_map, sizeof(report_map));
  hid_->setBatteryLevel(battery_level, false);

  consumer_ = hid_->getInputReport(1);
  keyboard_ = hid_->getInputReport(2);
  protocol_char_ = hid_->getProtocolMode();
  control_char_ = hid_->getHidControl();
  // The helper's boot characteristic lacks READ. Create both with the same
  // encrypted read permissions as the Python firmware.
  auto* service = hid_->getHidService();
  boot_input_ = service->createCharacteristic("2A22", NIMBLE_PROPERTY::READ |
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  boot_output_ = service->createCharacteristic("2A32", NIMBLE_PROPERTY::READ |
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE |
      NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  if (!consumer_ || !keyboard_ || !protocol_char_ || !control_char_ ||
      !boot_input_ || !boot_output_) return false;

  const uint8_t consumer_release[2] = {0, 0};
  const uint8_t keyboard_release[8] = {};
  consumer_->setValue(consumer_release, sizeof(consumer_release));
  keyboard_->setValue(keyboard_release, sizeof(keyboard_release));
  boot_input_->setValue(keyboard_release, sizeof(keyboard_release));
  boot_output_->setValue(static_cast<uint8_t>(0));
  protocol_char_->setValue(static_cast<uint8_t>(1));
  consumer_->setCallbacks(&characteristic_callbacks_);
  keyboard_->setCallbacks(&characteristic_callbacks_);
  boot_input_->setCallbacks(&characteristic_callbacks_);
  protocol_char_->setCallbacks(&characteristic_callbacks_);
  control_char_->setCallbacks(&characteristic_callbacks_);

  auto* dis = hid_->getDeviceInfoService();
  dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("StickS3 Media");
  dis->createCharacteristic("2A26", NIMBLE_PROPERTY::READ)->setValue("native-0.1");
  dis->createCharacteristic("2A27", NIMBLE_PROPERTY::READ)->setValue("StickS3");

  if (!server_->start()) return false;
  advertising_ = NimBLEDevice::getAdvertising();
  advertising_->addServiceUUID("1812");
  advertising_->setAppearance(0x03C1);  // HID keyboard.
  advertising_->setName(kName);
  running_ = true;
  last_bond_count_ = NimBLEDevice::getNumBonds();
  Serial.printf("BLE boot bonds %u\n", static_cast<unsigned>(last_bond_count_));
  startAdvertising(true, millis());
  return true;
}

void BleRemote::startAdvertising(bool fast, uint32_t now) {
  if (!running_ || connected_ || !advertising_) return;
  if (advertising_active_) advertising_->stop();
  const uint16_t interval = fast ? 160 : 800;  // 0.625 ms units.
  advertising_->setMinInterval(interval);
  advertising_->setMaxInterval(interval);
  advertising_active_ = advertising_->start();
  advertising_slow_ = !fast;
  advertising_since_ = now;
  Serial.printf("BLE advertising %s ok=%d\n", fast ? "100ms" : "500ms", advertising_active_);
}

void BleRemote::clearReports() {
  queue_head_ = queue_size_ = 0;
  pending_ = {};
}

void BleRemote::handleEvent(const Event& event, uint32_t now) {
  switch (event.type) {
    case EventType::connect:
      if (connected_ && event.handle != conn_handle_) {
        server_->disconnect(event.handle);
        break;
      }
      connected_ = true;
      conn_handle_ = event.handle;
      connected_since_ = now;
      last_security_request_ = 0;
      security_requests_ = 0;
      encrypted_ = bonded_ = suspended_ = false;
      key_size_ = 0;
      protocol_mode_ = 1;
      protocol_char_->setValue(static_cast<uint8_t>(1));
      advertising_active_ = false;
      clearReports();
      ++epoch_;
      Serial.printf("BLE connect bonds %u\n", static_cast<unsigned>(NimBLEDevice::getNumBonds()));
      break;
    case EventType::disconnect:
      if (event.handle != conn_handle_) break;
      Serial.printf("BLE disconnect reason %d bonds %u\n", event.value,
                    static_cast<unsigned>(NimBLEDevice::getNumBonds()));
      connected_ = encrypted_ = bonded_ = suspended_ = false;
      conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
      key_size_ = 0;
      protocol_mode_ = 1;
      clearReports();
      ++epoch_;
      if (running_) startAdvertising(true, now);
      break;
    case EventType::auth:
      Serial.printf("BLE auth encrypted=%u bonded=%u key_size=%u bonds=%u\n",
                    event.flags & 1, (event.flags >> 1) & 1, event.key_size,
                    static_cast<unsigned>(NimBLEDevice::getNumBonds()));
      break;
    case EventType::identity:
      Serial.println("BLE peer identity resolved");
      break;
    case EventType::control:
      if (event.handle == conn_handle_ && (event.value == 0 || event.value == 1)) {
        suspended_ = event.value == 0;
        clearReports();
        Serial.printf("BLE %s\n", suspended_ ? "suspend" : "resume");
      }
      break;
    case EventType::protocol:
      if (event.handle == conn_handle_ && (event.value == 0 || event.value == 1)) {
        protocol_mode_ = event.value;
        clearReports();
        Serial.printf("BLE protocol %u\n", protocol_mode_);
      }
      break;
    case EventType::subscribe:
      Serial.printf("BLE report subscription id=%d value=%d\n", event.value >> 16,
                    event.value & 0xffff);
      break;
  }
}

void BleRemote::refreshSecurity() {
  if (!connected_ || !server_) return;
  auto info = server_->getPeerInfoByHandle(conn_handle_);
  const bool encrypted = info.isEncrypted();
  const bool bonded = info.isBonded();
  const uint8_t size = info.getSecKeySize();
  if (encrypted != encrypted_ || bonded != bonded_ || size != key_size_) {
    encrypted_ = encrypted;
    bonded_ = bonded;
    key_size_ = size;
    Serial.printf("BLE link encrypted=%u bonded=%u key_size=%u bonds=%u\n",
                  encrypted_, bonded_, key_size_, static_cast<unsigned>(NimBLEDevice::getNumBonds()));
  }
}

bool BleRemote::notifyReport(const Report& report, bool release) {
  const uint8_t* bytes = release ? report.release : report.press;
  report.characteristic->setValue(bytes, report.length);
  const bool sent = report.characteristic->notify(bytes, report.length, conn_handle_);
  if (!sent) Serial.println("BLE HID notify failed");
  return sent;
}

void BleRemote::poll(uint32_t now) {
  if (!running_) return;
  Event event(EventType::identity);
  while (xQueueReceive(events_, &event, 0) == pdTRUE) handleEvent(event, now);
  if (now - last_bond_poll_ >= 1000) {
    last_bond_poll_ = now;
    const uint8_t count = NimBLEDevice::getNumBonds();
    if (count != last_bond_count_) {
      last_bond_count_ = count;
      Serial.printf("BLE bonds changed %u\n", static_cast<unsigned>(count));
    }
  }
  refreshSecurity();

  if (!connected_) {
    if (!advertising_active_) startAdvertising(true, now);
    else if (!advertising_slow_ && now - advertising_since_ >= 30000) startAdvertising(false, now);
    return;
  }
  if (!encrypted_) {
    // Let Windows resume link encryption from its existing bond first.
    const uint32_t first_delay = NimBLEDevice::getNumBonds() ? 4000 : 1200;
    const bool due = security_requests_ == 0 ? now - connected_since_ >= first_delay :
                     security_requests_ == 1 && now - last_security_request_ >= 10000;
    if (due) {
      ++security_requests_;
      last_security_request_ = now;
      const bool sent = NimBLEDevice::startSecurity(conn_handle_);
      Serial.printf("BLE security request %u sent=%u\n", security_requests_, sent);
    }
    clearReports();
    return;
  }
  if (suspended_) {
    clearReports();
    return;
  }
  if (pending_.characteristic && now - pressed_at_ >= 40) {
    if (notifyReport(pending_, true)) pending_ = {};
    else pressed_at_ = now;
  } else if (!pending_.characteristic && queue_size_) {
    Report next = queue_[queue_head_];
    queue_head_ = (queue_head_ + 1) % 4;
    --queue_size_;
    if (notifyReport(next, false)) {
      pending_ = next;
      pressed_at_ = now;
    }
  }
}

bool BleRemote::send(remote::Action action) {
  if (!running_ || !connected_ || !encrypted_ || suspended_ || queue_size_ >= 4) return false;
  if (protocol_mode_ == 0 && (action == remote::Action::volume_up ||
      action == remote::Action::volume_down || action == remote::Action::play_pause)) return false;
  Report report;
  switch (action) {
    case remote::Action::volume_up: report.characteristic = consumer_; report.press[0] = 0x40; report.length = 2; break;
    case remote::Action::volume_down: report.characteristic = consumer_; report.press[0] = 0x80; report.length = 2; break;
    case remote::Action::play_pause:
      if (protocol_mode_ == 0) return false;
      report.characteristic = consumer_; report.press[1] = 0x0D; report.length = 2; break;
    case remote::Action::arrow_left:
      report.characteristic = protocol_mode_ == 0 ? boot_input_ : keyboard_;
      report.press[2] = 0x50; report.length = 8; break;
    case remote::Action::arrow_right:
      report.characteristic = protocol_mode_ == 0 ? boot_input_ : keyboard_;
      report.press[2] = 0x4F; report.length = 8; break;
    case remote::Action::num_lock:
      report.characteristic = protocol_mode_ == 0 ? boot_input_ : keyboard_;
      report.press[2] = 0x53; report.length = 8; break;
    default: return false;
  }
  queue_[(queue_head_ + queue_size_) % 4] = report;
  ++queue_size_;
  return true;
}

void BleRemote::setBatteryLevel(uint8_t level) {
  if (hid_) hid_->setBatteryLevel(level, connected_ && encrypted_);
}

const char* BleRemote::status() const {
  if (!connected_) return "Pair via BT";
  if (!encrypted_) return "Pairing...";
  if (suspended_) return "Suspended";
  if (!bonded_ || NimBLEDevice::getNumBonds() == 0) return "No bond";
  return "Connected";
}

void BleRemote::stop() {
  if (!running_) return;
  clearReports();
  if (connected_ && encrypted_) {
    const Report consumer_release(consumer_, 2);
    const Report keyboard_release(keyboard_, 8);
    const Report boot_release(boot_input_, 8);
    notifyReport(consumer_release, true);
    notifyReport(keyboard_release, true);
    notifyReport(boot_release, true);
  }
  running_ = false;
  if (advertising_) advertising_->stop();
  if (server_ && connected_) server_->disconnect(conn_handle_);
  NimBLEDevice::deinit(false);
  Serial.println("BLE stopped");
}
