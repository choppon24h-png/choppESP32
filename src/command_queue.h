#pragma once
#include "protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: command_queue — Fila FIFO de Comandos BLE
// ═══════════════════════════════════════════════════════════════════════════
//
// Responsabilidades:
//   - Receber comandos BLE e enfileirá-los para execução sequencial
//   - Evitar execução concorrente de comandos
//   - Responder QUEUE:FULL quando a fila estiver cheia
//   - Limpar a fila em caso de desconexão BLE ou STOP
//
// COMPORTAMENTO:
//   - MAX_QUEUE = 5 entradas
//   - Fila cheia → responde QUEUE:FULL e descarta o comando
//   - Cada comando é executado somente após o anterior terminar
//   - STOP tem prioridade máxima — limpa a fila e executa imediatamente
//
// INTEGRAÇÃO:
//   - ble_protocol.cpp → onWrite() chama cmdQueue_enqueue()
//   - command_parser.cpp → taskCommandProcessor() chama cmdQueue_dequeue()
//   - ble_protocol.cpp → onDisconnect() chama cmdQueue_clear()
// ═══════════════════════════════════════════════════════════════════════════

// ── Configuração ──────────────────────────────────────────────────────────
#define CMD_QUEUE_MAX_SIZE      5       // Máximo de comandos na fila
#define CMD_QUEUE_CMD_MAX_LEN   PROTO_RX_BUFFER_SIZE

// ── Resposta de fila cheia ────────────────────────────────────────────────
#define RESP_QUEUE_FULL         "QUEUE:FULL"

// ── Inicialização ─────────────────────────────────────────────────────────
void cmdQueue_init();

// ── Enfileira um comando recebido via BLE ─────────────────────────────────
// Retorna true se enfileirado com sucesso.
// Retorna false e envia QUEUE:FULL se a fila estiver cheia.
// Thread-safe: pode ser chamado do contexto BLE (ISR-like).
bool cmdQueue_enqueue(const char* rawCmd);

// ── Retira o próximo comando da fila (bloqueante) ─────────────────────────
// Bloqueia até que um comando esteja disponível.
// Retorna true se um comando foi retirado com sucesso.
bool cmdQueue_dequeue(char* outBuf, size_t bufSize, uint32_t timeoutMs);

// ── Limpa toda a fila ─────────────────────────────────────────────────────
// Chamado no onDisconnect() e no handler de STOP.
void cmdQueue_clear();

// ── Retorna o número de comandos na fila ─────────────────────────────────
int cmdQueue_size();

// ── Verifica se a fila está vazia ─────────────────────────────────────────
bool cmdQueue_isEmpty();
