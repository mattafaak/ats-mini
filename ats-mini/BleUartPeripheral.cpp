#include "BleBase.h"
#include "BleUartPeripheral.h"
#include <host/ble_hs.h>
#include "Remote.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Guards rxBuf and txBuf against concurrent access from NimBLE host task
// and main loop (dual-core ESP32-S3).
static portMUX_TYPE bufMux = portMUX_INITIALIZER_UNLOCKED;

bool BleUartPeripheral::consumeAbortPending()
{
  bool pending = BleBase::consumeAbortFlag(abortPending);
  if (pending) {
    portENTER_CRITICAL(&bufMux);
    rxBuf.flush();
    portEXIT_CRITICAL(&bufMux);
  }
  return pending;
}

bool BleUartPeripheral::canSend() const
{
  return (txCh != nullptr) &&
         (txConnHandle != BLE_HS_CONN_HANDLE_NONE) &&
         txSubscribed;
}

size_t BleUartPeripheral::txPayloadSize() const
{
  size_t mtu = txPeerMtu;
  if (mtu < BLE_MIN_MTU) mtu = BLE_MIN_MTU;
  if (mtu > BLE_MAX_MTU) mtu = BLE_MAX_MTU;
  size_t payload = mtu - 3;
  if (payload > BLE_TX_PAYLOAD_CAP)
    payload = BLE_TX_PAYLOAD_CAP;
  return payload;
}

// It is hard to achieve max throughput witout using delays...
//
// https://github.com/nkolban/esp32-snippets/issues/773
// https://github.com/espressif/arduino-esp32/issues/8413
// https://github.com/espressif/esp-idf/issues/9097
// https://github.com/espressif/esp-idf/issues/16889
// https://github.com/espressif/esp-nimble/issues/75
// https://github.com/espressif/esp-nimble/issues/106
// https://github.com/h2zero/esp-nimble-cpp/issues/347
//
// Current workaround:
// - keep one pending notification payload outside txBuf
// - defer only the start of a new burst to batch small writes
// - retry the same pending payload after ENOMEM/EAPP
// - release the pending payload only after the second SUCCESS_NOTIFY callback
//
// ATT allows up to negotiated_mtu - 3 bytes of value payload, but using the
// full payload repeatedly caused BLE_HS_ENOMEM / BLE_HS_EAPP on this
// NimBLE/ESP32 path. BLE_TX_PAYLOAD_CAP is therefore an empirical notify cap:
// it limits outbound notifications without changing the negotiated ATT MTU.
size_t BleUartPeripheral::pumpTx()
{
  if (!canSend() || (txNotifyState != BleUartTxNotifyState::Idle)) return 0;

  uint32_t now = millis();
  if (txPendingLen > 0)
  {
    if ((txRetryAfterMs != 0) && ((int32_t)(now - txRetryAfterMs) < 0)) return 0;
    txRetryAfterMs = 0;
  }
  else
  {
    if ((txDeferUntilMs != 0) && ((int32_t)(now - txDeferUntilMs) < 0)) return 0;
    txDeferUntilMs = 0;
  }

  if (txPendingLen == 0)
  {
    portENTER_CRITICAL(&bufMux);
    if (txBuf.empty()) { portEXIT_CRITICAL(&bufMux); return 0; }

    size_t chunkSize = txPayloadSize();
    size_t availableByteCount = txBuf.available();
    if (availableByteCount < chunkSize)
      chunkSize = availableByteCount;
    if (chunkSize == 0) { portEXIT_CRITICAL(&bufMux); return 0; }

    txPendingLen = txBuf.read((char*)txChunk, chunkSize);
    portEXIT_CRITICAL(&bufMux);
    if (txPendingLen == 0) return 0;
  }
  txCh->setValue(txChunk, txPendingLen);
  txNotifyState = BleUartTxNotifyState::WaitingFirstStatus;
  txCh->notify();
  return txPendingLen;
}

void BleUartPeripheral::clearPendingTx()
{
  txPendingLen = 0;
  txDeferUntilMs = 0;
  txRetryAfterMs = 0;
  txNotifyState = BleUartTxNotifyState::Idle;
}

void BleUartPeripheral::resetTxSession()
{
  abortPending = false;
  clearPendingTx();
  portENTER_CRITICAL(&bufMux);
  txBuf.flush();
  portEXIT_CRITICAL(&bufMux);
  txConnHandle = BLE_HS_CONN_HANDLE_NONE;
  txPeerMtu = BLE_MIN_MTU;
  txSubscribed = false;
}

void BleUartPeripheral::configureDefaults()
{
  BLEDevice::setMTU(BLE_MAX_MTU);
  ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_ANY_MASK);
}

void BleUartPeripheral::createServices()
{
  if (service != nullptr) return;

  BLEServer* currentServer = server();
  if (currentServer == nullptr) return;

  service = currentServer->createService(UART_SERVICE_UUID);
  txCh = service->createCharacteristic(UART_CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  txCh->setCallbacks(this);
  rxCh = service->createCharacteristic(UART_CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE_NR);
  rxCh->setCallbacks(this);
  service->start();
}

void BleUartPeripheral::destroyServices()
{
  resetTxSession();
  abortPending = false;
  portENTER_CRITICAL(&bufMux);
  rxBuf.flush();
  portEXIT_CRITICAL(&bufMux);
}

void BleUartPeripheral::configureAdvertising(BLEAdvertising& advertising)
{
  advertising.removeServiceUUID(UART_SERVICE_UUID);
  advertising.addServiceUUID(UART_SERVICE_UUID);
}

void BleUartPeripheral::onConnect(BLEServer* server, ble_gap_conn_desc* desc)
{
  txConnHandle = desc->conn_handle;
  txPeerMtu = BLE_MIN_MTU;
  txSubscribed = false;
  clearPendingTx();
  ble_gap_set_prefered_le_phy(desc->conn_handle, BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_ANY_MASK, BLE_GAP_LE_PHY_CODED_ANY);
}

void BleUartPeripheral::onDisconnect(BLEServer* server, ble_gap_conn_desc* desc)
{
  if (desc->conn_handle == txConnHandle)
    resetTxSession();

  portENTER_CRITICAL(&bufMux);
  rxBuf.flush();
  portEXIT_CRITICAL(&bufMux);
  BlePeripheral::onDisconnect(server, desc);
}

void BleUartPeripheral::onWrite(BLECharacteristic* characteristic, ble_gap_conn_desc* desc)
{
  if (characteristic != rxCh) return;

  uint8_t* data = characteristic->getData();
  size_t byteCount = characteristic->getLength();
  abortPending |= byteCount > 0;
  portENTER_CRITICAL(&bufMux);
  size_t room = rxBuf.room();
  if (byteCount > room)
    byteCount = room;
  if ((data != nullptr) && (byteCount > 0))
    rxBuf.write((const char*)data, byteCount);
  portEXIT_CRITICAL(&bufMux);
}

void BleUartPeripheral::onSubscribe(BLECharacteristic* characteristic, ble_gap_conn_desc* desc, uint16_t subValue)
{
  if ((characteristic != txCh) || (desc->conn_handle != txConnHandle)) return;

  txSubscribed = !!(subValue & 0x0001);
  if (txSubscribed)
  {
    BLEServer* currentServer = server();
    if (currentServer != nullptr)
    {
      uint16_t mtu = currentServer->getPeerMTU(desc->conn_handle);
      if (mtu >= BLE_MIN_MTU)
        txPeerMtu = mtu;
    }
    pumpTx();
  }
  else
  {
    clearPendingTx();
    portENTER_CRITICAL(&bufMux);
    txBuf.flush();
    portEXIT_CRITICAL(&bufMux);
  }
}

void BleUartPeripheral::onStatus(BLECharacteristic* characteristic, Status status, uint32_t code)
{
  if (characteristic != txCh) return;

  switch (status)
  {
    case SUCCESS_NOTIFY:
    case SUCCESS_INDICATE:
      // On this wrapper the first SUCCESS_NOTIFY is not enough to retire the
      // pending chunk. Wait for the second success callback before clearing it.
      if ((status == SUCCESS_NOTIFY) && (txNotifyState == BleUartTxNotifyState::WaitingFirstStatus))
      {
        txNotifyState = BleUartTxNotifyState::WaitingSecondStatus;
        break;
      }
      clearPendingTx();
      break;

    case ERROR_NO_CLIENT:
      resetTxSession();
      break;

    case ERROR_NO_SUBSCRIBER:
      clearPendingTx();
      portENTER_CRITICAL(&bufMux);
      txBuf.flush();
      portEXIT_CRITICAL(&bufMux);
      txSubscribed = false;
      break;

    case ERROR_GATT:
      if ((code == BLE_HS_ENOMEM) || (code == BLE_HS_EAPP))
      {
        // Keep txPendingLen and retry the same payload later. Only the
        // in-flight state is cleared here.
        txNotifyState = BleUartTxNotifyState::Idle;
        txRetryAfterMs = millis() + BLE_NOTIFY_RETRY_DELAY_MS;
        break;
      }
      clearPendingTx();
      portENTER_CRITICAL(&bufMux);
      txBuf.flush();
      portEXIT_CRITICAL(&bufMux);
      break;

    default:
      break;
  }
}

int BleUartPeripheral::available()
{
  pumpTx();
  portENTER_CRITICAL(&bufMux);
  int result = rxBuf.available();
  portEXIT_CRITICAL(&bufMux);
  return result;
}

int BleUartPeripheral::peek()
{
  pumpTx();
  portENTER_CRITICAL(&bufMux);
  int result = rxBuf.peek();
  portEXIT_CRITICAL(&bufMux);
  return result;
}

int BleUartPeripheral::read()
{
  pumpTx();
  portENTER_CRITICAL(&bufMux);
  int value = rxBuf.read();
  portEXIT_CRITICAL(&bufMux);
  if (value >= 0)
    abortPending = false;
  return value;
}

void BleUartPeripheral::flush()
{
  uint32_t start = millis();
  while (true)
  {
    portENTER_CRITICAL(&bufMux);
    bool empty = txBuf.empty() && (txPendingLen == 0);
    portEXIT_CRITICAL(&bufMux);
    if (empty) break;
    if (!canSend()) break;
    if (pumpTx() == 0) {
      if ((millis() - start) > 500) break;
      delay(1);
    }
  }
}

size_t BleUartPeripheral::write(const uint8_t* data, size_t size)
{
  if ((txCh == nullptr) || !canSend()) return 0;

  size_t writtenByteCount = 0;
  while (writtenByteCount < size)
  {
    pumpTx();

    portENTER_CRITICAL(&bufMux);
    bool wasEmpty = txBuf.empty();
    size_t queuedByteCount = txBuf.write((const char*)data + writtenByteCount, size - writtenByteCount);
    portEXIT_CRITICAL(&bufMux);
    writtenByteCount += queuedByteCount;
    if ((queuedByteCount > 0) && wasEmpty && (txPendingLen == 0) && (txNotifyState == BleUartTxNotifyState::Idle))
    {
      uint32_t deferUntilMs = millis() + BLE_TX_DEFER_MS;
      if ((txDeferUntilMs == 0) || ((int32_t)(deferUntilMs - txDeferUntilMs) > 0))
        txDeferUntilMs = deferUntilMs;
    }
    if (writtenByteCount >= size)
      break;

    if (!canSend())
      return writtenByteCount;

    delay(1);
  }

  if (!canSend())
    return writtenByteCount;

  pumpTx();
  return writtenByteCount;
}

size_t BleUartPeripheral::write(uint8_t byte)
{
  return write(&byte, 1);
}

size_t BleUartPeripheral::print(std::string str)
{
  return write((const uint8_t*)str.data(), str.length());
}

size_t BleUartPeripheral::printf(const char* format, ...)
{
  char dummy;
  va_list args;
  va_start(args, format);
  int requiredSize = vsnprintf(&dummy, 1, format, args);
  va_end(args);
  if (requiredSize == 0)
  {
    return 0;
  }
  else if (requiredSize > 0)
  {
    char* buffer = (char*)malloc(requiredSize + 1);
    if (!buffer) return 0;
    va_start(args, format);
    int result = vsnprintf(buffer, requiredSize + 1, format, args);
    va_end(args);
    if ((result >= 0) && (result <= requiredSize))
    {
      size_t writtenBytesCount = write((uint8_t*)buffer, result);
      free(buffer);
      return writtenBytesCount;
    }
    free(buffer);
  }
  return 0;
}
