#include "watchdog.h"
#include "valve_controller.h"
#include "ble_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: watchdog.cpp
// ═══════════════════════════════════════════════════════════════════════════

// Tempo máximo de espera por reconexão BLE durante dispensação (60s)
#define WDG_BLE_RECONEXAO_TIMEOUT_MS  60000UL

// [v2.1] PING Watchdog: se ESP ficar > 5s sem receber PING durante operação,
// fecha a válvula. O timer é resetado por watchdog_kick() que é chamado
// em handlePing() (command_parser.cpp) e em onWrite() (ble_protocol.cpp).
// PROTO_WATCHDOG_MS = 5000ms (definido em protocol.h)

// ── Inicialização ─────────────────────────────────────────────────────────
void watchdog_init() {
    g_opState.ultimoComandoMs = (uint64_t)esp_timer_get_time() / 1000ULL;
    DBG_PRINTLN("[WDG] Watchdog inicializado");
}

// ── Reset do timer ────────────────────────────────────────────────────────
void watchdog_kick() {
    g_opState.ultimoComandoMs = (uint64_t)esp_timer_get_time() / 1000ULL;
}

// ═══════════════════════════════════════════════════════════════════════════
// TAREFA FREERTOS: taskWatchdog
// Prioridade alta — verifica segurança a cada 500ms.
// ═══════════════════════════════════════════════════════════════════════════
void taskWatchdog(void* param) {
    // Timestamp de quando o BLE desconectou durante dispensação
    uint64_t bleDesconectadoEm = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        uint64_t agora = (uint64_t)esp_timer_get_time() / 1000ULL;

        // ── Watchdog de comando (válvula aberta sem comando) ──────────────
        if (valveController_isOpen() && g_opState.state == SYS_RUNNING) {

            // ── Caso 1: BLE desconectado durante dispensação ──────────────
            if (!g_opState.bleConectado) {
                if (bleDesconectadoEm == 0) {
                    bleDesconectadoEm = agora;
                    DBG_PRINTF("[WDG] BLE desconectado durante dispensação — aguardando reconexão por %u s\n",
                               WDG_BLE_RECONEXAO_TIMEOUT_MS / 1000);
                }

                uint64_t tempoDesconectado = agora - bleDesconectadoEm;
                if (tempoDesconectado >= WDG_BLE_RECONEXAO_TIMEOUT_MS) {
                    DBG_PRINTF("[WDG] SEGURANÇA: BLE não reconectou em %u s — fechando válvula\n",
                               WDG_BLE_RECONEXAO_TIMEOUT_MS / 1000);
                    valveController_stop("WDG_BLE_TIMEOUT");
                    bleDesconectadoEm = 0;
                }
                continue; // Não aplica watchdog de comando enquanto aguarda reconexão
            } else {
                // BLE reconectou — reseta o timer de desconexão
                if (bleDesconectadoEm > 0) {
                    DBG_PRINTLN("[WDG] BLE reconectado — retomando monitoramento");
                    bleDesconectadoEm = 0;
                    watchdog_kick(); // Reseta o timer de comando também
                }
            }

            // ── Caso 2: PING Watchdog (5s sem PING/comando com BLE ativo) ──
            // Qualquer dado recebido do Android (incluindo $PING) reseta o timer.
            // Se o app travar ou perder comunicação, a válvula é fechada com segurança.
            uint64_t tempoSemComando = agora - g_opState.ultimoComandoMs;
            if (tempoSemComando >= PROTO_WATCHDOG_MS) {
                DBG_PRINTF("[WDG] PING WATCHDOG: %u ms sem PING/comando com válvula aberta — fechando\n",
                           (uint32_t)tempoSemComando);
                DBG_PRINTLN("[WDG] TX: ERROR:WATCHDOG");
                bleProtocol_send("ERROR:WATCHDOG");
                valveController_stop("WDG_PING_TIMEOUT");
            }

        } else {
            // Válvula fechada — reseta o timer de desconexão BLE
            if (bleDesconectadoEm > 0) {
                bleDesconectadoEm = 0;
            }
        }

        // ── Watchdog de estado inconsistente ─────────────────────────────
        // Se o estado é RUNNING mas a válvula está fechada há mais de 2s,
        // algo deu errado — reseta o estado.
        if (g_opState.state == SYS_RUNNING && !valveController_isOpen()) {
            static uint64_t estadoInconsistenteEm = 0;
            if (estadoInconsistenteEm == 0) {
                estadoInconsistenteEm = agora;
            } else if ((agora - estadoInconsistenteEm) > 2000) {
                DBG_PRINTLN("[WDG] Estado inconsistente: RUNNING mas válvula fechada — resetando");
                g_opState.state = SYS_IDLE;
                estadoInconsistenteEm = 0;
            }
        } else {
            // Estado consistente — reseta o detector
        }
    }
}
