#include "command_queue.h"
#include "ble_protocol.h"
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: command_queue.cpp
// Fila FIFO de até 5 comandos BLE para execução sequencial sem concorrência.
// Thread-safe via FreeRTOS Queue + Semaphore.
// ═══════════════════════════════════════════════════════════════════════════

// ── Fila FreeRTOS interna ─────────────────────────────────────────────────
static QueueHandle_t s_queue     = nullptr;
static volatile int  s_queueSize = 0;   // Contador atômico para verificação rápida

// ── Mutex para o contador ─────────────────────────────────────────────────
static SemaphoreHandle_t s_countMutex = nullptr;

// ── Inicialização ─────────────────────────────────────────────────────────
void cmdQueue_init() {
    // Cria fila FreeRTOS com capacidade CMD_QUEUE_MAX_SIZE
    // Cada item é um buffer de CMD_QUEUE_CMD_MAX_LEN bytes
    s_queue = xQueueCreate(CMD_QUEUE_MAX_SIZE, CMD_QUEUE_CMD_MAX_LEN);
    s_countMutex = xSemaphoreCreateMutex();
    s_queueSize = 0;

    DBG_PRINTF("[CMDQ] Inicializado | max=%d | item_size=%d bytes\n",
               CMD_QUEUE_MAX_SIZE, CMD_QUEUE_CMD_MAX_LEN);
}

// ── Enfileira comando (thread-safe, chamável de ISR-like context) ─────────
bool cmdQueue_enqueue(const char* rawCmd) {
    if (!s_queue || !rawCmd || rawCmd[0] == '\0') return false;

    // ── Verifica se STOP tem prioridade — limpa fila e enfileira imediatamente
    // Detecta $STOP no início do rawCmd
    bool isStop = (rawCmd[0] == '$' &&
                   rawCmd[1] == 'S' && rawCmd[2] == 'T' &&
                   rawCmd[3] == 'O' && rawCmd[4] == 'P');

    if (isStop) {
        // STOP tem prioridade máxima: limpa a fila e enfileira na frente
        cmdQueue_clear();
        DBG_PRINTLN("[CMDQ] STOP com prioridade — fila limpa");
    }

    // ── Verifica se a fila está cheia ─────────────────────────────────────
    if (uxQueueSpacesAvailable(s_queue) == 0) {
        DBG_PRINTF("[CMDQ] FILA CHEIA (%d/%d) — descartando: [%s]\n",
                   CMD_QUEUE_MAX_SIZE, CMD_QUEUE_MAX_SIZE, rawCmd);
        bleProtocol_send(RESP_QUEUE_FULL);
        return false;
    }

    // ── Copia para buffer local com tamanho seguro ────────────────────────
    char buf[CMD_QUEUE_CMD_MAX_LEN];
    strncpy(buf, rawCmd, CMD_QUEUE_CMD_MAX_LEN - 1);
    buf[CMD_QUEUE_CMD_MAX_LEN - 1] = '\0';

    // ── Enfileira (não bloqueante — retorna imediatamente se cheio) ───────
    if (xQueueSend(s_queue, buf, 0) == pdTRUE) {
        if (xSemaphoreTake(s_countMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            s_queueSize++;
            xSemaphoreGive(s_countMutex);
        }
        DBG_PRINTF("[CMDQ] Enfileirado: [%s] | fila=%d/%d\n",
                   rawCmd, s_queueSize, CMD_QUEUE_MAX_SIZE);
        return true;
    }

    // Fila ficou cheia entre a verificação e o envio (race condition raro)
    DBG_PRINTF("[CMDQ] RACE: fila cheia ao enfileirar [%s]\n", rawCmd);
    bleProtocol_send(RESP_QUEUE_FULL);
    return false;
}

// ── Retira próximo comando da fila (bloqueante com timeout) ───────────────
bool cmdQueue_dequeue(char* outBuf, size_t bufSize, uint32_t timeoutMs) {
    if (!s_queue || !outBuf || bufSize == 0) return false;

    TickType_t ticks = (timeoutMs == portMAX_DELAY)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeoutMs);

    char tmp[CMD_QUEUE_CMD_MAX_LEN];

    if (xQueueReceive(s_queue, tmp, ticks) == pdTRUE) {
        strncpy(outBuf, tmp, bufSize - 1);
        outBuf[bufSize - 1] = '\0';

        if (xSemaphoreTake(s_countMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (s_queueSize > 0) s_queueSize--;
            xSemaphoreGive(s_countMutex);
        }

        DBG_PRINTF("[CMDQ] Desenfileirado: [%s] | fila=%d/%d\n",
                   outBuf, s_queueSize, CMD_QUEUE_MAX_SIZE);
        return true;
    }

    return false; // Timeout
}

// ── Limpa toda a fila ─────────────────────────────────────────────────────
void cmdQueue_clear() {
    if (!s_queue) return;

    int removidos = 0;
    char tmp[CMD_QUEUE_CMD_MAX_LEN];

    // Drena a fila sem bloquear
    while (xQueueReceive(s_queue, tmp, 0) == pdTRUE) {
        removidos++;
    }

    if (xSemaphoreTake(s_countMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_queueSize = 0;
        xSemaphoreGive(s_countMutex);
    }

    if (removidos > 0) {
        DBG_PRINTF("[CMDQ] Fila limpa | %d comandos descartados\n", removidos);
    }
}

// ── Tamanho atual da fila ─────────────────────────────────────────────────
int cmdQueue_size() {
    return s_queueSize;
}

// ── Verifica se está vazia ────────────────────────────────────────────────
bool cmdQueue_isEmpty() {
    return (s_queueSize == 0);
}
