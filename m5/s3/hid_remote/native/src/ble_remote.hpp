#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdint>
#include "input.hpp"

class BleRemote {
 public:
  bool begin(uint8_t battery_level);
  void poll(uint32_t now);
  void stop();
  bool send(remote::Action action);
  void setBatteryLevel(uint8_t level);
  const char* status() const;
  uint32_t epoch() const { return epoch_; }
  bool encrypted() const { return encrypted_; }
  bool bonded() const { return bonded_; }
  uint8_t keySize() const { return key_size_; }
  bool suspended() const { return suspended_; }

 private:
  class ServerCallbacks : public NimBLEServerCallbacks {
   public:
    explicit ServerCallbacks(BleRemote& remote) : remote_(remote) {}
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override;
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override;
    void onAuthenticationComplete(NimBLEConnInfo& info) override;
    void onIdentity(NimBLEConnInfo& info) override;
   private:
    BleRemote& remote_;
  };

  class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
   public:
    explicit CharacteristicCallbacks(BleRemote& remote) : remote_(remote) {}
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override;
    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) override;
   private:
    BleRemote& remote_;
  };

  struct Report {
    Report(NimBLECharacteristic* c = nullptr, uint8_t len = 0)
        : characteristic(c), length(len) {}
    NimBLECharacteristic* characteristic = nullptr;
    uint8_t press[8] = {};
    uint8_t release[8] = {};
    uint8_t length = 0;
  };

  enum class EventType : uint8_t { connect, disconnect, auth, identity, control, protocol, subscribe };
  struct Event {
    Event(EventType t, uint16_t h = 0, int v = 0, uint8_t f = 0, uint8_t k = 0)
        : type(t), handle(h), value(v), flags(f), key_size(k) {}
    EventType type;
    uint16_t handle = 0;
    int value = 0;
    uint8_t flags = 0;
    uint8_t key_size = 0;
  };

  void emitEvent(const Event& event);
  void handleEvent(const Event& event, uint32_t now);
  void startAdvertising(bool fast, uint32_t now);
  void clearReports();
  bool notifyReport(const Report& report, bool release);
  void refreshSecurity();
  static String staticIdentity();

  ServerCallbacks server_callbacks_{*this};
  CharacteristicCallbacks characteristic_callbacks_{*this};
  NimBLEServer* server_ = nullptr;
  NimBLEHIDDevice* hid_ = nullptr;
  NimBLEAdvertising* advertising_ = nullptr;
  NimBLECharacteristic* consumer_ = nullptr;
  NimBLECharacteristic* keyboard_ = nullptr;
  NimBLECharacteristic* boot_input_ = nullptr;
  NimBLECharacteristic* boot_output_ = nullptr;
  NimBLECharacteristic* protocol_char_ = nullptr;
  NimBLECharacteristic* control_char_ = nullptr;
  Report queue_[4] = {};
  uint8_t queue_head_ = 0;
  uint8_t queue_size_ = 0;
  Report pending_ = {};
  uint32_t pressed_at_ = 0;
  uint32_t advertising_since_ = 0;
  uint32_t connected_since_ = 0;
  uint32_t last_security_request_ = 0;
  uint32_t last_bond_poll_ = 0;
  uint32_t epoch_ = 0;
  uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
  uint8_t key_size_ = 0;
  uint8_t protocol_mode_ = 1;
  uint8_t security_requests_ = 0;
  uint8_t last_bond_count_ = 0;
  bool connected_ = false;
  bool encrypted_ = false;
  bool bonded_ = false;
  bool suspended_ = false;
  bool advertising_slow_ = false;
  bool advertising_active_ = false;
  bool running_ = false;
  QueueHandle_t events_ = nullptr;
};
