// Copyright (C) 2024, Mark Qvist

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Boards.h"

#if PLATFORM != PLATFORM_NRF52
#if HAS_BLE

#include "BLESerial.h"

uint32_t bt_passkey_callback();
void bt_passkey_notify_callback(uint32_t passkey);
bool bt_security_request_callback();
void bt_authentication_complete_callback(esp_ble_auth_cmpl_t auth_result);
bool bt_confirm_pin_callback(uint32_t pin);
void bt_connect_callback(BLEServer *server);
void bt_disconnect_callback(BLEServer *server);
bool bt_client_authenticated();
extern bool wireless_kiss_policy_ready;
extern bool wireless_kiss_allowed;

uint32_t BLESerial::onPassKeyRequest() { return bt_passkey_callback(); }
void BLESerial::onPassKeyNotify(uint32_t passkey) { bt_passkey_notify_callback(passkey); }
bool BLESerial::onSecurityRequest() { return bt_security_request_callback(); }
void BLESerial::onAuthenticationComplete(esp_ble_auth_cmpl_t auth_result) { bt_authentication_complete_callback(auth_result); }
void BLESerial::onConnect(BLEServer *server) { bt_connect_callback(server); }
void BLESerial::onDisconnect(BLEServer *server) {
  bt_disconnect_callback(server);
#if defined(BLE_PEER_TRANSPORT)
  // The peer interface owns the advertiser in this build; restarting it here
  // would fight it after every peer disconnect.
  return;
#endif
  // SerialBT.end() can trigger this callback while secure-node policy is
  // closing BLE. Never let the library's disconnect path reopen advertising.
  if (wireless_kiss_policy_ready && wireless_kiss_allowed) {
    ble_server->startAdvertising();
  }
}
bool BLESerial::onConfirmPIN(uint32_t pin) { return bt_confirm_pin_callback(pin); };
bool BLESerial::connected() { return ble_server->getConnectedCount() > 0; }

int BLESerial::read() {
  int result = this->rx_buffer.pop();
  if (result == '\n') { this->numAvailableLines--; }
  return result;
}

size_t BLESerial::readBytes(uint8_t *buffer, size_t bufferSize) {
  int i = 0;
  while (i < bufferSize && available()) { buffer[i] = (uint8_t)this->rx_buffer.pop(); i++; }
  return i;
}

int BLESerial::peek() {
  if (this->rx_buffer.getLength() == 0) return -1;
  return this->rx_buffer.get(0);
}

int BLESerial::available() { return this->rx_buffer.getLength(); }

size_t BLESerial::print(const char *str) {
  if (ble_server->getConnectedCount() <= 0) return 0;
  size_t written = 0; for (size_t i = 0; str[i] != '\0'; i++)  { written += this->write(str[i]); }
  flush();

  return written;
}

size_t BLESerial::write(const uint8_t *buffer, size_t bufferSize) {
  if (ble_server->getConnectedCount() <= 0) { return 0; } else {
    size_t written = 0; for (int i = 0; i < bufferSize; i++) { written += this->write(buffer[i]); }
    flush();

    return written;
  }
}

size_t BLESerial::write(uint8_t byte) {
  // No serial service, nowhere to write. True for every peer build.
  if (TxCharacteristic == nullptr) { return 0; }
  if (peerMTU == 0) { checkMTU(); }
  if (bt_client_authenticated()) {
    if (ble_server->getConnectedCount() <= 0) { return 0; } else {
      this->transmitBuffer[this->transmitBufferLength] = byte;
      this->transmitBufferLength++;
      if (this->transmitBufferLength >= maxTransferSize) { flush(); }
      return 1;
    }
  } else {
    return 0;
  }
}

// Clamp what we notify to what the peer negotiated.
//
// maxTransferSize was fixed at BLE_BUFFER_SIZE and never adjusted -- checkMTU()
// was declared, never defined and never called, and peerMTU was never assigned.
// A notification larger than the negotiated MTU does not fragment: the tail is
// dropped, without error, on every frame.
bool BLESerial::checkMTU() {
  if (ble_server == nullptr || ble_server->getConnectedCount() <= 0) return false;
  const uint16_t negotiated = ble_server->getPeerMTU(ble_server->getConnId());
  if (negotiated < MIN_MTU) return false;
  peerMTU = negotiated;
  // Three bytes of ATT notification header come off the usable payload.
  const uint16_t usable = (uint16_t)(negotiated - 3);
  maxTransferSize = (usable < BLE_BUFFER_SIZE) ? usable : (uint16_t)BLE_BUFFER_SIZE;
  // Deliberately does NOT log here.
  //
  // checkMTU() is reached from write() and flush(), i.e. from inside
  // serial_write() while a KISS frame is being emitted. A printf at this point
  // injects *unframed* text into the middle of that frame: the host's RNode
  // parser desynchronises, never completes its handshake, and RNS leaves the
  // interface offline with "Cannot send - interface is offline". Observed
  // exactly that on a phone -- the first thing it read off the link was
  // "[bt] mtu negotiated: 512 (payload 509)" as raw bytes, where a KISS frame
  // should have been.
  //
  // The value is published for the main loop to log instead; see update_bt().
  mtu_log_pending = true;
  return true;
}

void BLESerial::flush() {
  // Resolve the MTU lazily rather than at connect. The peer negotiates it
  // *after* the GATT connect callback fires, so asking there always returned
  // nothing and left the size at the truncating default. By the time anything
  // is actually being sent it has settled.
  if (peerMTU == 0) {
    if (checkMTU()) {
      printf("[bt] mtu negotiated: %u (payload %u)\n",
             (unsigned)peerMTU, (unsigned)maxTransferSize);
    }
  }
  if (this->transmitBufferLength > 0 && TxCharacteristic != nullptr) {
    TxCharacteristic->setValue(this->transmitBuffer, this->transmitBufferLength);
    this->transmitBufferLength = 0;
    this->lastFlushTime = millis();
    TxCharacteristic->notify(true);
  }
}

void BLESerial::disconnect() {
  if (ble_server->getConnectedCount() > 0) {
    uint16_t conn_id = ble_server->getConnId();
    // Serial.printf("Have connected: %d\n", conn_id);
    ble_server->disconnect(conn_id);
    // Serial.println("Disconnected");
  } else {
    // Serial.println("No connected");
  }
}

void BLESerial::begin(const char *name) {
  ConnectedDeviceCount = 0;
  device_name = (name != nullptr) ? name : "";
  BLEDevice::init(name);

  // Without this Bluedroid keeps its 23-byte default, whatever the client asks
  // for -- Columba requests 512 and would still have been held to a 20-byte
  // payload. Combined with the un-clamped buffer below, that silently truncated
  // every KISS frame longer than 20 bytes: the link came up, pairing worked, and
  // no Reticulum traffic survived.
  BLEDevice::setMTU(BLE_BUFFER_SIZE);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9); 
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN ,ESP_PWR_LVL_P9);

  ble_server = BLEDevice::createServer();
  ble_server->setCallbacks(this);

#if defined(BLE_PEER_TRANSPORT)
  // Peer build: the RNode/KISS service is not served at all.
  //
  // It is not merely unused here -- it is actively harmful. Its
  // characteristics demand ESP_GATT_PERM_*_ENC_MITM and it sets an encrypted
  // link requirement, so the moment a Reticulum peer connects and enumerates
  // services, Android tries to pair. bt_security_request_callback() then
  // refuses unless a pairing window happens to be open, and the peer sees
  // "device is not ready to pair" on a loop. Observed exactly that: the node
  // finally appeared as a peer, and then did nothing but emit pair requests.
  //
  // The Reticulum BLE peer protocol expects an unencrypted link and carries no
  // pairing step, so leaving the server free of MITM requirements is both the
  // fix and the correct behaviour. BLEDevice::init() and the server itself stay
  // -- BLEPeerInterface attaches its service to them.
  printf("[ble] peer mode: RNode/KISS service not served\n");
#else
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(this);

  SetupSerialService();
  this->startAdvertising();
#endif
}

void BLESerial::startAdvertising() {
  ble_adv = BLEDevice::getAdvertising();

  // Put the name in the advertisement and the service UUID in the scan
  // response, rather than the other way round.
  //
  // This is why a phone's own Bluetooth settings could not see the node while
  // nRF Connect could. The library's default path sets
  // `include_name = !m_scanResp`, so asking for a scan response moves the name
  // *out* of the advertisement entirely -- and the 31-byte advertisement was
  // then filled by flags, TX power, the connection interval hint and a 128-bit
  // service UUID, which alone costs 18 bytes. What went out was an unnamed
  // advertisement. A scanner that shows everything it hears, like nRF Connect,
  // displays such a device happily; a phone's pairing screen, which lists
  // devices by name, has nothing to list.
  //
  // The service UUID is not needed to be discovered -- it is what a client
  // matches on *after* connecting -- so it moves to the scan response, where
  // there is room for it and where an interested client will still ask for it.
  BLEAdvertisementData advertisement;
  advertisement.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advertisement.setName(device_name);
  // Generic "Computer" appearance. Costs 4 bytes and gives a phone something
  // to render besides a bare name.
  advertisement.setAppearance(0x0080);

  BLEAdvertisementData scan_response;
  scan_response.setCompleteServices(BLEUUID(BLE_SERIAL_SERVICE_UUID));

  ble_adv->setAdvertisementData(advertisement);
  ble_adv->setScanResponseData(scan_response);
  ble_adv->setMinPreferred(0x20);
  ble_adv->setMaxPreferred(0x40);
  ble_adv->setScanResponse(true);
  ble_adv->start();
}

void BLESerial::stopAdvertising() {
  ble_adv = BLEDevice::getAdvertising();
  ble_adv->stop();
}

void BLESerial::end() { BLEDevice::deinit(); }

void BLESerial::onWrite(BLECharacteristic *characteristic) {
  if (characteristic->getUUID().toString() == BLE_RX_UUID) {
    auto value = characteristic->getValue();
    // Does the host's data reach us at all? The board sends nothing to the
    // phone, which means it never has anything to answer -- and that is either
    // because the commands never arrive or because they arrive and are not
    // consumed. This separates the two, and it is the only place that can.
    // Counted, not printed. Anything written to the console while a host is
    // attached goes to that host, and unframed bytes in a KISS stream are what
    // broke the RNode handshake.
    rx_bytes_total += value.length();
    for (int i = 0; i < value.length(); i++) { rx_buffer.push(value[i]); }
  }
}

#if !defined(BLE_PEER_TRANSPORT)
void BLESerial::SetupSerialService() {
  SerialService = ble_server->createService(BLE_SERIAL_SERVICE_UUID);

  RxCharacteristic = SerialService->createCharacteristic(BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  RxCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  RxCharacteristic->addDescriptor(new BLE2902());
  RxCharacteristic->setWriteProperty(true);
  RxCharacteristic->setCallbacks(this);

  TxCharacteristic = SerialService->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  TxCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

  // The CCCD needs write permission of its own, and it was never given any.
  //
  // A client enables notifications by *writing* this descriptor. The
  // characteristic was declared read-only-under-MITM and the descriptor
  // inherited nothing, so the subscribe could be refused -- and a client that
  // cannot subscribe has no reason to go on and send commands. Observed
  // exactly: connect, a full bond (auth_mode 0x0d: bond + MITM + SC), and then
  // not one byte ever written to the RX characteristic, over and over.
  //
  // Granting it the same authenticated access as the characteristic keeps the
  // security requirement identical while making the subscribe possible.
  BLE2902 *tx_cccd = new BLE2902();
  tx_cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  TxCharacteristic->addDescriptor(tx_cccd);
  TxCharacteristic->setNotifyProperty(true);
  TxCharacteristic->setReadProperty(true);

  SerialService->start();
}
#endif // !BLE_PEER_TRANSPORT

BLESerial::BLESerial() { }

#endif
#endif
