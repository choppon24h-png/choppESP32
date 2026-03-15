#include "command_history.h"
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: command_history.cpp
// Memória circular de 20 CMD_IDs para proteção anti-duplicação.
// Thread-safe via mutex FreeRTOS.
// ═══════════════════════════════════════════════════════════════════════════

// ── Buffer circular ───────────────────────────────────────────────────────
static char             s_history[CMD_HISTORY_SIZE][CMD_HISTORY_ID_MAX_LEN];
static int              s_writeIdx  = 0;    // Próxima posição de escrita
static int              s_count     = 0;    // Entradas válidas no buffer
static SemaphoreHandle_t s_mutex    = nullptr;

// ── Inicialização ─────────────────────────────────────────────────────────
void cmdHistory_init() {
    memset(s_history, 0, sizeof(s_history));
    s_writeIdx = 0;
    s_count    = 0;

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }

    DBG_PRINTF("[HIST] Inicializado | capacidade=%d | id_max=%d bytes\n",
               CMD_HISTORY_SIZE, CMD_HISTORY_ID_MAX_LEN);
}

// ── Verifica duplicado (thread-safe) ─────────────────────────────────────
bool cmdHistory_isDuplicate(const char* cmd_id) {
    if (!cmd_id || cmd_id[0] == '\0') return false;
    if (!s_mutex) return false;

    bool found = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < CMD_HISTORY_SIZE; i++) {
            if (s_history[i][0] != '\0' && strcmp(s_history[i], cmd_id) == 0) {
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
    } else {
        // Mutex timeout — por segurança, assume não duplicado para não bloquear fluxo
        DBG_PRINTLN("[HIST] WARN: mutex timeout em isDuplicate — assumindo não duplicado");
    }

    if (found) {
        DBG_PRINTF("[HIST] DUPLICADO detectado: [%s]\n", cmd_id);
    }

    return found;
}

// ── Registra CMD_ID (thread-safe) ────────────────────────────────────────
void cmdHistory_register(const char* cmd_id) {
    if (!cmd_id || cmd_id[0] == '\0') return;
    if (!s_mutex) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        // Escreve na posição atual e avança o índice circular
        strncpy(s_history[s_writeIdx], cmd_id, CMD_HISTORY_ID_MAX_LEN - 1);
        s_history[s_writeIdx][CMD_HISTORY_ID_MAX_LEN - 1] = '\0';

        s_writeIdx = (s_writeIdx + 1) % CMD_HISTORY_SIZE;

        if (s_count < CMD_HISTORY_SIZE) s_count++;

        DBG_PRINTF("[HIST] Registrado: [%s] | total=%d/%d\n",
                   cmd_id, s_count, CMD_HISTORY_SIZE);

        xSemaphoreGive(s_mutex);
    } else {
        DBG_PRINTF("[HIST] WARN: mutex timeout ao registrar [%s]\n", cmd_id);
    }
}

// ── Limpa o histórico ─────────────────────────────────────────────────────
void cmdHistory_clear() {
    if (!s_mutex) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(s_history, 0, sizeof(s_history));
        s_writeIdx = 0;
        s_count    = 0;
        xSemaphoreGive(s_mutex);
    }

    DBG_PRINTLN("[HIST] Histórico limpo (reconexão BLE)");
}

// ── Contagem de entradas ──────────────────────────────────────────────────
int cmdHistory_count() {
    return s_count;
}
