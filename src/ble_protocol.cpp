#include "ble_protocol.h"
#include "valve_controller.h"
#include "watchdog.h"
#include "command_history.h"   // [v2.1] Limpar histórico no disconnect
#include "command_queue.h"     // [v2.1] Enfileirar via cmdQueue_enqueue
#include <esp_gap_ble_api.h>
#include <esp_bt_main.h>
#include <esp_mac.h>
#include <string.h>
#include <stdio.h>

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: ble_protocol.cpp
// ═══════════════════════════════════════════════════════════════════════════

// ── Variáveis globais (declaradas em protocol.h) ──────────────────────────
SemaphoreHandle_t g_bleMutex  = nullptr;
QueueHandle_t     g_cmdQueue  = nullptr;

// ── Estado interno BLE ────────────────────────────────────────────────────
static BLEServer*         s_pServer    = nullptr;
static BLECharacteristic* s_pTxChar   = nullptr;
static BLECharacteristic* s_pRxChar   = nullptr;
static char               s_deviceName[32] = {0};

// ── Parâmetros de conexão para máxima estabilidade ────────────────────────
// interval=7.5ms, latency=0, timeout=5000ms
static esp_ble_conn_update_params_t s_connParams = {
    .min_int  = 0x06,   // 7.5ms
    .max_int  = 0x06,   // 7.5ms
    .latency  = 0,
    .timeout  = 500,    // 5000ms (unidade: 10ms)
};

// ═══════════════════════════════════════════════════════════════════════════
// CALLBACKS DE SEGURANÇA BLE
// ═══════════════════════════════════════════════════════════════════════════
class MySecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override {
        DBG_PRINTLN("[BLE-SEC] onPassKeyRequest → PIN: " BLE_AUTH_PIN);
        return (uint32_t)atol(BLE_AUTH_PIN);
    }

    void onPassKeyNotify(uint32_t pass_key) override {
        DBG_PRINTF("[BLE-SEC] onPassKeyNotify: %06u\n", pass_key);
    }

    bool onConfirmPIN(uint32_t pass_key) override {
        DBG_PRINTF("[BLE-SEC] onConfirmPIN: %06u\n", pass_key);
        return (pass_key == (uint32_t)atol(BLE_AUTH_PIN));
    }

    bool onSecurityRequest() override {
        DBG_PRINTLN("[BLE-SEC] onSecurityRequest → true");
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        if (cmpl.success) {
            DBG_PRINTLN("[BLE-SEC] Bond/Pairing concluído com sucesso");
            g_opState.bleAutenticado = true;
            // Envia AUTH:OK para o Android saber que pode enviar $ML
            if (s_pTxChar) {
                bleProtocol_send(RESP_AUTH_OK);
            }
        } else {
            DBG_PRINTF("[BLE-SEC] Falha no bond: reason=0x%02X\n", cmpl.fail_reason);
            g_opState.bleAutenticado = false;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// CALLBACKS DE SERVIDOR BLE (conexão / desconexão)
// ═══════════════════════════════════════════════════════════════════════════
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        g_opState.bleConectado   = true;
        g_opState.bleAutenticado = false; // Requer re-autenticação a cada conexão

        DBG_PRINTF("[BLE] Conectado | addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   param->connect.remote_bda[0], param->connect.remote_bda[1],
                   param->connect.remote_bda[2], param->connect.remote_bda[3],
                   param->connect.remote_bda[4], param->connect.remote_bda[5]);

        // Solicita parâmetros de conexão de alta performance
        memcpy(s_connParams.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gap_update_conn_params(&s_connParams);

        // Reseta o watchdog de comando
        watchdog_kick();

        // Se havia operação em andamento, informa o status ao Android
        if (g_opState.state == SYS_RUNNING) {
            char buf[PROTO_TX_BUFFER_SIZE];
            snprintf(buf, sizeof(buf), "RESUME:ML=%u:LIBERADO=%u",
                     g_opState.mlSolicitado, g_opState.mlLiberado);
            // Aguarda autenticação antes de enviar (será enviado após AUTH:OK)
        }
    }

    void onDisconnect(BLEServer* pServer) override {
        g_opState.bleConectado   = false;
        g_opState.bleAutenticado = false;

        DBG_PRINTLN("[BLE] Desconectado");

        // [v2.1] PROTEÇÃO BLE DISCONNECT:
        // Fecha a válvula IMEDIATAMENTE se não há operação em andamento.
        // Se há operação (SYS_RUNNING), mantém aberta e aguarda reconexão.
        if (valveController_isOpen() && g_opState.state != SYS_RUNNING) {
            DBG_PRINTLN("[BLE] Disconnect sem operação ativa — fechando válvula imediatamente");
            valveController_stop("BLE_DISCONNECT");
        } else if (valveController_isOpen()) {
            DBG_PRINTLN("[BLE] Disconnect durante operação — válvula permanece aberta (watchdog 60s)");
        }

        // [v2.1] Limpa histórico de comandos e fila ao desconectar
        // Evita falsos positivos de duplicação após reconexão
        cmdHistory_clear();
        cmdQueue_clear();

        // Reinicia advertising para permitir reconexão
        vTaskDelay(pdMS_TO_TICKS(200));
        bleProtocol_startAdvertising();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// CALLBACKS DA CARACTERÍSTICA RX (dados recebidos do Android)
// ═══════════════════════════════════════════════════════════════════════════
class MyRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        std::string value = pChar->getValue();
        if (value.empty()) return;

        // Copia para buffer com terminador nulo
        char buf[PROTO_RX_BUFFER_SIZE];
        size_t len = value.length();
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, value.c_str(), len);
        buf[len] = '\0';

        // Remove \r\n se presentes
        for (int i = len - 1; i >= 0; i--) {
            if (buf[i] == '\r' || buf[i] == '\n') buf[i] = '\0';
            else break;
        }

        DBG_PRINTF("[PROTO] RX raw: [%s]\n", buf);

        // [v2.1] Enfileira via command_queue (FIFO 5, QUEUE:FULL automático)
        // cmdQueue_enqueue já responde QUEUE:FULL se necessário
        if (!cmdQueue_enqueue(buf)) {
            DBG_PRINTF("[BLE] cmdQueue_enqueue falhou para: [%s]\n", buf);
        }

        // Reseta o watchdog a cada dado recebido
        watchdog_kick();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// INICIALIZAÇÃO DO BLE
// ═══════════════════════════════════════════════════════════════════════════
void bleProtocol_init() {
    // ── Cria mutex e fila ─────────────────────────────────────────────────
    g_bleMutex = xSemaphoreCreateMutex();
    g_cmdQueue = xQueueCreate(PROTO_CMD_QUEUE_SIZE, PROTO_RX_BUFFER_SIZE);

    // ── Gera nome dinâmico CHOPP_XXXX (4 últimos bytes do MAC BLE) ────────
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_deviceName, sizeof(s_deviceName), "CHOPP_%02X%02X", mac[4], mac[5]);

    DBG_PRINTF("[BLE] Nome do dispositivo: %s\n", s_deviceName);
    DBG_PRINTF("[BLE] MAC BLE: %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // ── Inicializa BLEDevice ──────────────────────────────────────────────
    BLEDevice::init(s_deviceName);
    BLEDevice::setMTU(512);

    // ── Configura segurança BLE ───────────────────────────────────────────
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

    BLESecurity* pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    pSecurity->setCapability(ESP_IO_CAP_OUT);       // ESP32 exibe o PIN
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setStaticPIN((uint32_t)atol(BLE_AUTH_PIN));

    // ── Cria servidor BLE ─────────────────────────────────────────────────
    s_pServer = BLEDevice::createServer();
    s_pServer->setCallbacks(new MyServerCallbacks());

    // ── Cria serviço NUS ──────────────────────────────────────────────────
    BLEService* pService = s_pServer->createService(NUS_SERVICE_UUID);

    // ── Característica TX (ESP32 → Android) — Notify ─────────────────────
    s_pTxChar = pService->createCharacteristic(
        NUS_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    s_pTxChar->addDescriptor(new BLE2902());

    // ── Característica RX (Android → ESP32) — Write ──────────────────────
    s_pRxChar = pService->createCharacteristic(
        NUS_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    s_pRxChar->setCallbacks(new MyRxCallbacks());

    // ── Inicia o serviço ──────────────────────────────────────────────────
    pService->start();

    // ── Configura e inicia advertising ───────────────────────────────────
    bleProtocol_startAdvertising();

    DBG_PRINTF("[BLE] Pronto — aguardando conexão | Nome: %s | PIN: %s\n",
               s_deviceName, BLE_AUTH_PIN);
}

// ── Envio thread-safe ao Android ─────────────────────────────────────────
void bleProtocol_send(const char* data) {
    if (!s_pTxChar || !g_opState.bleConectado) {
        DBG_PRINTF("[BLE] TX ignorado (desconectado): %s\n", data);
        return;
    }

    // Adquire mutex com timeout de 50ms para não bloquear indefinidamente
    if (xSemaphoreTake(g_bleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_pTxChar->setValue((uint8_t*)data, strlen(data));
        s_pTxChar->notify();
        DBG_PRINTF("[PROTO] TX: %s\n", data);
        xSemaphoreGive(g_bleMutex);
    } else {
        DBG_PRINTF("[BLE] MUTEX timeout ao enviar: %s\n", data);
    }
}

// ── Reinicia advertising ──────────────────────────────────────────────────
void bleProtocol_startAdvertising() {
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(NUS_SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMaxPreferred(0x06);
    BLEDevice::startAdvertising();
    DBG_PRINTF("[BLE] Advertising iniciado — %s\n", s_deviceName);
}

// ── Nome do dispositivo ───────────────────────────────────────────────────
const char* bleProtocol_getDeviceName() {
    return s_deviceName;
}
