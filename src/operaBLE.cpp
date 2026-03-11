#include "operaBLE.h"

#ifdef USAR_ESP32_UART_BLE

    BLEServer *pServer = NULL;
    BLECharacteristic *pTxCharacteristic;
    bool deviceConnected = false;

    // Controla se o dispositivo Android foi autenticado via PIN ($AUTH:259087)
    // Resetado para false a cada nova conexao BLE
    bool bleAutenticado = false;

    // -------------------------------------------------------------------------
    // Callbacks de segurança BLE (bond/pairing nativo com PIN 259087)
    // Ativado quando o Android chama device.createBond() antes do connectGatt()
    // -------------------------------------------------------------------------
    class MySecurityCallbacks : public BLESecurityCallbacks {

        // Retorna o PIN quando o Android solicita pareamento
        uint32_t onPassKeyRequest() {
            DBG_PRINT(F("\n[BLE-SEC] onPassKeyRequest — retornando PIN: "));
            DBG_PRINT(BLE_AUTH_PIN);
            return atoi(BLE_AUTH_PIN);
        }

        // Notifica que o PIN foi exibido (ESP32 com display)
        void onPassKeyNotify(uint32_t pass_key) {
            DBG_PRINT(F("\n[BLE-SEC] onPassKeyNotify — PIN exibido: "));
            DBG_PRINT(pass_key);
        }

        // Retorna true para confirmar o pareamento
        bool onConfirmPIN(uint32_t pass_key) {
            DBG_PRINT(F("\n[BLE-SEC] onConfirmPIN — confirmando PIN: "));
            DBG_PRINT(pass_key);
            return true;
        }

        // Informa se o dispositivo já foi autenticado anteriormente
        bool onSecurityRequest() {
            DBG_PRINT(F("\n[BLE-SEC] onSecurityRequest — permitindo"));
            return true;
        }

        // Chamado quando a autenticação é concluída
        // Se autenticado com sucesso, envia AUTH:OK automaticamente
        void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
            if (cmpl.success) {
                DBG_PRINT(F("\n[BLE-SEC] Autenticacao BLE nativa concluida com SUCESSO"));
                bleAutenticado = true;
                enviaBLE("AUTH:OK");
            } else {
                DBG_PRINT(F("\n[BLE-SEC] Autenticacao BLE nativa FALHOU"));
                bleAutenticado = false;
                enviaBLE("AUTH:FAIL");
            }
        }
    };

    // -------------------------------------------------------------------------
    // Callbacks de conexão/desconexão
    // -------------------------------------------------------------------------
    class MyServerCallbacks : public BLEServerCallbacks {
        void onConnect(BLEServer *pServer) {
            digitalWrite(PINO_STATUS, LED_STATUS_ON);
            DBG_PRINT(F("\n[BLE] Conectado"));
            deviceConnected = true;
            // Reseta autenticacao a cada nova conexao
            bleAutenticado = false;
            DBG_PRINT(F("\n[BLE] Aguardando autenticacao (bond nativo ou $AUTH:PIN)"));
        };

        void onDisconnect(BLEServer *pServer) {
            digitalWrite(PINO_STATUS, !LED_STATUS_ON);
            DBG_PRINT(F("\n[BLE] Desconectado"));
            deviceConnected = false;
            bleAutenticado = false;
            delay(500);
            pServer->startAdvertising();
        }
    };

    // -------------------------------------------------------------------------
    // Callbacks de escrita na característica RX
    // Aceita dois fluxos de autenticação:
    //   1. Bond nativo: Android faz createBond() → PIN 259087 → AUTH:OK automático
    //   2. Comando manual: Android envia $AUTH:259087 → ESP32 valida e responde AUTH:OK
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

                // ---------------------------------------------------------
                // Fluxo 2: Autenticacao por comando $AUTH:<pin>
                // Formato esperado do Android: $AUTH:259087
                // Compativel com BluetoothService.java (envio automatico apos
                // onServicesDiscovered) e com Bluetooth2.java (legado)
                // ---------------------------------------------------------
                if (cmd.startsWith("$") && cmd.substring(1, 6) == COMANDO_AUTH) {
                    String pinRecebido = cmd.substring(6);
                    pinRecebido.trim();
                    DBG_PRINT(F("\n[BLE] PIN recebido via $AUTH: "));
                    DBG_PRINT(pinRecebido);
                    if (pinRecebido == BLE_AUTH_PIN) {
                        bleAutenticado = true;
                        DBG_PRINT(F("\n[BLE] Autenticacao OK via comando"));
                        enviaBLE("AUTH:OK");
                    } else {
                        bleAutenticado = false;
                        DBG_PRINT(F("\n[BLE] Autenticacao FALHOU - PIN incorreto"));
                        enviaBLE("AUTH:FAIL");
                    }
                    return;
                }

                // ---------------------------------------------------------
                // Bloqueia qualquer comando de operacao se nao autenticado
                // ---------------------------------------------------------
                if (!bleAutenticado) {
                    DBG_PRINT(F("\n[BLE] Comando bloqueado - dispositivo nao autenticado"));
                    enviaBLE("ERROR:NOT_AUTHENTICATED");
                    return;
                }

                // Dispositivo autenticado: processa o comando normalmente
                executaOperacao(cmd);
            }
        }
    };

void setupBLE() {

    // Inicializa o dispositivo BLE com o nome configurado (ex: CHOPP_0001)
    BLEDevice::init(BLE_NAME);

    // -------------------------------------------------------------------------
    // Configura segurança BLE nativa para suportar bond/pairing com PIN 259087
    // Isso permite que o Android use device.createBond() antes do connectGatt()
    // garantindo que o PIN seja injetado automaticamente via ACTION_PAIRING_REQUEST
    // -------------------------------------------------------------------------
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

    BLESecurity *pSecurity = new BLESecurity();
    // Modo de autenticação: Secure Connections + MITM + Bond
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    // Capacidade de I/O: ESP32 pode exibir o PIN (display only)
    pSecurity->setCapability(ESP_IO_CAP_OUT);
    // PIN estático 259087
    pSecurity->setStaticPIN(atoi(BLE_AUTH_PIN));
    // Tamanho da chave de criptografia
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    DBG_PRINT(F("\n[BLE] Seguranca BLE configurada — PIN: "));
    DBG_PRINT(BLE_AUTH_PIN);

    // Cria o servidor BLE
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Cria o serviço BLE (Nordic UART Service - NUS)
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Característica TX (notificação → ESP32 envia dados ao Android)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // Característica RX (escrita → Android envia comandos ao ESP32)
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    // Inicia o serviço e o advertising
    pService->start();
    pServer->getAdvertising()->start();

    DBG_PRINT(F("\n[BLE] Aguardando conexao — Nome: "));
    DBG_PRINT(BLE_NAME);
}

void enviaBLE(String msg) {
    msg += '\n';
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
}

#endif
