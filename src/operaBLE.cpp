#include "operaBLE.h"

#ifdef USAR_ESP32_UART_BLE

    BLEServer *pServer = NULL;
    BLECharacteristic *pTxCharacteristic;
    bool deviceConnected = false;

    // Controla se o dispositivo Android foi autenticado via PIN ($AUTH:259087)
    bool bleAutenticado = false;

    // =========================================================================
    // DIAGNÓSTICO DO STATUS=8 (GATT_CONN_TIMEOUT / Connection Supervision Timeout)
    //
    // O log mostra que exatamente 5 segundos após o $ML:300 ser enviado e o
    // OK ser recebido, o BLE desconecta com status=8.
    //
    // CAUSA RAIZ: O ESP32 usa os parâmetros de conexão BLE padrão:
    //   - Connection Interval: 7.5ms ~ 4000ms (negociado pelo stack)
    //   - Supervision Timeout: 20s (padrão do Bluedroid)
    //
    // Porém, quando o ESP32 entra na taskLiberaML e começa a dispensar,
    // ele fica bloqueado no loop while() com vTaskDelay(50ms). Durante esse
    // tempo, o stack BLE Bluedroid não consegue processar os LL (Link Layer)
    // keep-alive packets com prioridade suficiente, causando o supervision
    // timeout no lado do Android.
    //
    // SOLUÇÃO DEFINITIVA — 3 camadas:
    //
    // 1. ESP32: Forçar connection parameters com supervision timeout alto (6s)
    //    e connection interval curto (7.5ms) logo após conectar.
    //    Isso garante que o Android envie keep-alives frequentes.
    //
    // 2. ESP32: A taskLiberaML roda na Core 0. O BLE Bluedroid também roda
    //    na Core 0. Mover a taskLiberaML para a Core 1 (APP_CPU) para que
    //    o BLE tenha a Core 0 (PRO_CPU) exclusivamente.
    //    Isso é feito em main.cpp com xTaskCreatePinnedToCore(..., 0) → 1.
    //
    // 3. Android: Após reconectar, reenviar $ML com os ML restantes
    //    (já implementado no PagamentoConcluido.java).
    // =========================================================================

    // -------------------------------------------------------------------------
    // Callbacks de segurança BLE (bond/pairing nativo com PIN 259087)
    // -------------------------------------------------------------------------
    class MySecurityCallbacks : public BLESecurityCallbacks {

        uint32_t onPassKeyRequest() {
            DBG_PRINT(F("\n[BLE-SEC] onPassKeyRequest — PIN: "));
            DBG_PRINT(BLE_AUTH_PIN);
            return atoi(BLE_AUTH_PIN);
        }

        void onPassKeyNotify(uint32_t pass_key) {
            DBG_PRINT(F("\n[BLE-SEC] onPassKeyNotify: "));
            DBG_PRINT(pass_key);
        }

        bool onConfirmPIN(uint32_t pass_key) {
            DBG_PRINT(F("\n[BLE-SEC] onConfirmPIN: "));
            DBG_PRINT(pass_key);
            return true;
        }

        bool onSecurityRequest() {
            DBG_PRINT(F("\n[BLE-SEC] onSecurityRequest — OK"));
            return true;
        }

        void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
            if (cmpl.success) {
                DBG_PRINT(F("\n[BLE-SEC] Autenticacao SUCESSO"));
                bleAutenticado = true;
                enviaBLE("AUTH:OK");
            } else {
                DBG_PRINT(F("\n[BLE-SEC] Autenticacao FALHOU"));
                bleAutenticado = false;
                enviaBLE("AUTH:FAIL");
            }
        }
    };

    // -------------------------------------------------------------------------
    // Callbacks de conexão/desconexão
    // -------------------------------------------------------------------------
    class MyServerCallbacks : public BLEServerCallbacks {

        void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) {
            digitalWrite(PINO_STATUS, LED_STATUS_ON);
            deviceConnected = true;
            bleAutenticado  = false;

            // -----------------------------------------------------------------
            // FIX DEFINITIVO STATUS=8: Forçar connection parameters logo após
            // conectar para garantir supervision timeout alto e keep-alive
            // frequente. Isso evita que o Android declare o ESP32 como perdido.
            //
            // Parâmetros:
            //   conn_id        : ID da conexão atual
            //   min_interval   : 6 * 1.25ms = 7.5ms  (mínimo BLE)
            //   max_interval   : 6 * 1.25ms = 7.5ms  (forçar intervalo fixo)
            //   latency        : 0 (sem slave latency — ESP32 responde sempre)
            //   timeout        : 500 * 10ms = 5000ms (supervision timeout 5s)
            //
            // IMPORTANTE: slave_latency=0 é crítico. Com latency > 0, o ESP32
            // pode "pular" connection events, o que causa o status=8 quando
            // o Android não recebe ACK por muito tempo.
            // -----------------------------------------------------------------
            esp_ble_conn_update_params_t conn_params = {};
            conn_params.bda[0] = param->connect.remote_bda[0];
            conn_params.bda[1] = param->connect.remote_bda[1];
            conn_params.bda[2] = param->connect.remote_bda[2];
            conn_params.bda[3] = param->connect.remote_bda[3];
            conn_params.bda[4] = param->connect.remote_bda[4];
            conn_params.bda[5] = param->connect.remote_bda[5];
            conn_params.min_int    = 0x06;   // 7.5ms
            conn_params.max_int    = 0x06;   // 7.5ms
            conn_params.latency    = 0;      // sem slave latency
            conn_params.timeout    = 0x1F4;  // 5000ms supervision timeout

            esp_ble_gap_update_conn_params(&conn_params);

            DBG_PRINT(F("\n[BLE] Conectado — conn params atualizados (interval=7.5ms, timeout=5000ms)"));

            // Se havia operação em andamento, notifica o Android ao reconectar
            if (operacaoEmAndamento) {
                delay(1000); // aguarda autenticação completar
                String statusReconexao = COMANDO_VP + String(mlLiberadoGlobal, 3);
                enviaBLE(statusReconexao);
                DBG_PRINT(F("\n[BLE] Reconexao durante operacao — VP enviado: "));
                DBG_PRINT(mlLiberadoGlobal, 3);
            }
        }

        // Sobrecarga sem parâmetros (compatibilidade)
        void onConnect(BLEServer *pServer) {
            digitalWrite(PINO_STATUS, LED_STATUS_ON);
            deviceConnected = true;
            bleAutenticado  = false;
            DBG_PRINT(F("\n[BLE] Conectado (sem param)"));
        }

        void onDisconnect(BLEServer *pServer) {
            digitalWrite(PINO_STATUS, !LED_STATUS_ON);
            DBG_PRINT(F("\n[BLE] Desconectado — reiniciando advertising"));
            deviceConnected = false;
            bleAutenticado  = false;
            // Delay mínimo antes de reiniciar advertising
            delay(200);
            pServer->startAdvertising();
        }
    };

    // -------------------------------------------------------------------------
    // Callbacks de escrita na característica RX
    // -------------------------------------------------------------------------
    class MyCallbacks : public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *pCharacteristic) {
            String cmd = "";
            std::string rxValue = pCharacteristic->getValue();
            DBG_PRINT(F("\n[BLE] Recebido: "));
            if (rxValue.length() > 0) {
                for (int i = 0; i < rxValue.length(); i++) {
                    cmd += (char)rxValue[i];
                }
                cmd.trim();
                DBG_PRINT(cmd);

                // Autenticação por comando $AUTH:<pin>
                if (cmd.startsWith("$") && cmd.substring(1, 6) == COMANDO_AUTH) {
                    String pinRecebido = cmd.substring(6);
                    pinRecebido.trim();
                    if (pinRecebido == BLE_AUTH_PIN) {
                        bleAutenticado = true;
                        DBG_PRINT(F("\n[BLE] AUTH OK via comando"));
                        enviaBLE("AUTH:OK");
                    } else {
                        bleAutenticado = false;
                        DBG_PRINT(F("\n[BLE] AUTH FALHOU — PIN incorreto"));
                        enviaBLE("AUTH:FAIL");
                    }
                    return;
                }

                // Bloqueia comandos sem autenticação
                if (!bleAutenticado) {
                    DBG_PRINT(F("\n[BLE] Comando bloqueado — nao autenticado"));
                    enviaBLE("ERROR:NOT_AUTHENTICATED");
                    return;
                }

                // Processa o comando de operação
                executaOperacao(cmd);
            }
        }
    };

void setupBLE() {

    BLEDevice::init(BLE_NAME);

    // Segurança BLE com bond/pairing PIN 259087
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

    BLESecurity *pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);
    pSecurity->setStaticPIN(atoi(BLE_AUTH_PIN));
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    DBG_PRINT(F("\n[BLE] Seguranca configurada — PIN: "));
    DBG_PRINT(BLE_AUTH_PIN);

    // Servidor BLE
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Serviço NUS (Nordic UART Service)
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // TX: ESP32 → Android (notify)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // RX: Android → ESP32 (write)
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();

    // Configura advertising com intervalo curto para reconexão rápida
    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->setMinInterval(0x20);  // 20ms
    pAdvertising->setMaxInterval(0x40);  // 40ms
    pAdvertising->start();

    DBG_PRINT(F("\n[BLE] Aguardando conexao — Nome: "));
    DBG_PRINT(BLE_NAME);
}

void enviaBLE(String msg) {
    msg += '\n';
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
}

#endif
