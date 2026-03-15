#include "valve_controller.h"
#include "flow_sensor.h"
#include "ble_protocol.h"

// ── Timeout de operação (melhoria v2.1) ──────────────────────────────────
// Se a dispensação não concluir em VALVE_OP_TIMEOUT_MS, fecha a válvula.
// Protege contra sensor travado, pulsos não chegando ou loop infinito.
#define VALVE_OP_TIMEOUT_MS     10000UL  // 10 segundos
#define RESP_ERROR_TIMEOUT      "ERROR:TIMEOUT"

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: valve_controller.cpp
// ═══════════════════════════════════════════════════════════════════════════

// ── Estado interno ────────────────────────────────────────────────────────
static volatile bool s_valveOpen = false;

// ── Semáforo de disparo da tarefa de dispensação ──────────────────────────
static SemaphoreHandle_t s_dispensacaoSem = nullptr;

// ── Inicialização ─────────────────────────────────────────────────────────
void valveController_init() {
    pinMode(PINO_RELE, OUTPUT);
    digitalWrite(PINO_RELE, !RELE_ON);  // Garante válvula fechada na inicialização
    s_valveOpen = false;

    s_dispensacaoSem = xSemaphoreCreateBinary();

    DBG_PRINTF("[VALVE] Inicializado | pino=%d | estado=FECHADA\n", PINO_RELE);
}

// ── Controle direto ───────────────────────────────────────────────────────
void valveController_open() {
    if (s_valveOpen) return;
    digitalWrite(PINO_RELE, RELE_ON);
    s_valveOpen = true;
    DBG_PRINTLN("[VALVE] ABERTA");
    bleProtocol_send(RESP_VALVE_OPEN);
}

void valveController_close() {
    if (!s_valveOpen) return;
    digitalWrite(PINO_RELE, !RELE_ON);
    s_valveOpen = false;
    DBG_PRINTLN("[VALVE] FECHADA");
    bleProtocol_send(RESP_VALVE_CLOSED);
}

bool valveController_isOpen() {
    return s_valveOpen;
}

// ── Iniciar dispensação ───────────────────────────────────────────────────
bool valveController_startDispensacao(uint32_t ml) {
    if (g_opState.state == SYS_RUNNING) {
        DBG_PRINTLN("[VALVE] BUSY — dispensação já em andamento");
        return false;
    }

    // Configura o estado global da operação
    g_opState.mlSolicitado    = ml;
    g_opState.mlLiberado      = 0;
    g_opState.pulsosAlvo      = flowSensor_calcularAlvo(ml);
    g_opState.pulsosContados  = 0;
    g_opState.ultimoComandoMs = (uint64_t)esp_timer_get_time() / 1000ULL;
    g_opState.state           = SYS_RUNNING;

    // Reseta o sensor e dispara a tarefa
    flowSensor_reset();
    flowSensor_enable();

    DBG_PRINTF("[VALVE] Iniciando dispensação: %u ml | alvo=%u pulsos\n",
               ml, g_opState.pulsosAlvo);

    // Sinaliza a taskDispensacao para iniciar
    xSemaphoreGive(s_dispensacaoSem);
    return true;
}

// ── Parada de emergência ──────────────────────────────────────────────────
void valveController_stop(const char* motivo) {
    flowSensor_disable();
    valveController_close();
    g_opState.state      = SYS_IDLE;
    g_opState.mlLiberado = flowSensor_getMl();

    DBG_PRINTF("[VALVE] STOP | motivo=%s | liberado=%u ml\n",
               motivo, g_opState.mlLiberado);
    bleProtocol_send(RESP_STOP_OK);
}

// ═══════════════════════════════════════════════════════════════════════════
// TAREFA FREERTOS: taskDispensacao
// Executada na Core 1 (APP_CPU) para não interferir com o BLE (Core 0 PRO_CPU).
// ═══════════════════════════════════════════════════════════════════════════
void taskDispensacao(void* param) {
    char txBuf[PROTO_TX_BUFFER_SIZE];

    for (;;) {
        // Aguarda sinal para iniciar dispensação
        xSemaphoreTake(s_dispensacaoSem, portMAX_DELAY);

        DBG_PRINTLN("[DISP] Tarefa iniciada");

        // Abre a válvula e envia confirmação
        valveController_open();

        uint64_t ultimoVP      = (uint64_t)esp_timer_get_time() / 1000ULL;
        uint64_t inicioOperacao = (uint64_t)esp_timer_get_time() / 1000ULL; // [v2.1] Timeout 10s

        // ── Loop de dispensação ───────────────────────────────────────────
        while (g_opState.state == SYS_RUNNING) {
            uint32_t pulsos = flowSensor_getPulsos();
            uint32_t mlAtual = flowSensor_getMl();
            uint64_t agora   = (uint64_t)esp_timer_get_time() / 1000ULL;

            // Atualiza estado global
            g_opState.pulsosContados = pulsos;
            g_opState.mlLiberado     = mlAtual;

            // ── Verifica se atingiu o volume alvo ─────────────────────────
            if (pulsos >= g_opState.pulsosAlvo) {
                DBG_PRINTF("[DISP] Alvo atingido: %u pulsos | %u ml\n", pulsos, mlAtual);
                break;
            }

            // ── [v2.1] Timeout de operação: 10s máximo ────────────────────
            if ((agora - inicioOperacao) >= VALVE_OP_TIMEOUT_MS) {
                DBG_PRINTF("[DISP] ERROR:TIMEOUT — operação excedeu %u ms — fechando válvula\n",
                           VALVE_OP_TIMEOUT_MS);
                bleProtocol_send(RESP_ERROR_TIMEOUT);
                g_opState.state = SYS_ERROR;
                break;
            }

            // ── Verifica timeout do sensor (sem pulsos) ───────────────────
            uint64_t ultimoPulso = flowSensor_getUltimoPulsoMs();
            if (ultimoPulso > 0 && (agora - ultimoPulso) > (uint64_t)g_opState.timeoutSensor) {
                if (!g_opState.bleConectado) {
                    // BLE desconectado: aguarda reconexão (watchdog cuidará se exceder 60s)
                    DBG_PRINTLN("[DISP] BLE desconectado — aguardando reconexão...");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                DBG_PRINTF("[DISP] TIMEOUT sensor (%u ms sem pulsos) — fechando válvula\n",
                           g_opState.timeoutSensor);
                g_opState.state = SYS_STOPPING;
                break;
            }

            // ── Envia VP: (volume parcial) a cada PROTO_HEARTBEAT_INTERVAL_MS ──
            if ((agora - ultimoVP) >= PROTO_HEARTBEAT_INTERVAL_MS) {
                snprintf(txBuf, sizeof(txBuf), "%s%u", RESP_VP_PREFIX, mlAtual);
                bleProtocol_send(txBuf);
                ultimoVP = agora;
                DBG_PRINTF("[DISP] VP:%u\n", mlAtual);
            }

            vTaskDelay(pdMS_TO_TICKS(50)); // Cede CPU sem bloquear
        }

        // ── Finalização da dispensação ────────────────────────────────────
        flowSensor_disable();
        valveController_close();

        uint32_t mlFinal   = flowSensor_getMl();
        uint32_t pulsosFinal = flowSensor_getPulsos();
        g_opState.mlLiberado     = mlFinal;
        g_opState.pulsosContados = pulsosFinal;
        g_opState.state          = SYS_IDLE;

        // Envia VP final, QP e DONE
        snprintf(txBuf, sizeof(txBuf), "%s%u", RESP_VP_PREFIX, mlFinal);
        bleProtocol_send(txBuf);

        snprintf(txBuf, sizeof(txBuf), "%s%u", RESP_QP_PREFIX, pulsosFinal);
        bleProtocol_send(txBuf);

        bleProtocol_send(RESP_DONE);

        DBG_PRINTF("[DISP] Concluído | ml=%u | pulsos=%u\n", mlFinal, pulsosFinal);
    }
}
