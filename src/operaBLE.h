#include "config.h"
#if !defined(_OPERA_BLE_) && defined(USAR_ESP32_UART_BLE)
    #define _OPERA_BLE_
    #include <BLEDevice.h>
    #include <BLEServer.h>
    #include <BLEUtils.h>
    #include <BLE2902.h>
    #include <BLESecurity.h>

    #include "operacional.h"

    #define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"  // UART service UUID
    #define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    #define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

    void setupBLE();
    void enviaBLE( String msg );

#endif
