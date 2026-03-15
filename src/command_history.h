#pragma once
#include "protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: command_history — Histórico Circular de CMD_IDs
// ═══════════════════════════════════════════════════════════════════════════
//
// Responsabilidades:
//   - Manter memória circular dos últimos CMD_HISTORY_SIZE commandIds
//   - Detectar comandos duplicados em O(n) com proteção de mutex
//   - Registrar novos commandIds de forma thread-safe
//   - Limpar o histórico em caso de reconexão BLE
//
// PROTEÇÃO ANTI-DUPLICAÇÃO:
//   - Tamanho: 20 entradas (CMD_HISTORY_SIZE)
//   - Estrutura: buffer circular com índice rotativo
//   - Thread-safe: protegido por mutex FreeRTOS
//   - Resposta ao duplicado: ML:DUPLICATE (para ML) ou ERROR:DUPLICATE
//
// INTEGRAÇÃO:
//   - Chamado por command_parser.cpp antes de executar qualquer comando
//   - Chamado por ble_protocol.cpp no onDisconnect() para limpar histórico
// ═══════════════════════════════════════════════════════════════════════════

// ── Configuração ──────────────────────────────────────────────────────────
#define CMD_HISTORY_SIZE        20      // Número máximo de CMD_IDs armazenados
#define CMD_HISTORY_ID_MAX_LEN  32      // Tamanho máximo de um CMD_ID

// ── Resposta de duplicado ─────────────────────────────────────────────────
#define RESP_DUPLICATE          "ERROR:DUPLICATE"

// ── Inicialização ─────────────────────────────────────────────────────────
// Deve ser chamado em setup() antes de bleProtocol_init().
void cmdHistory_init();

// ── Verifica se um CMD_ID já foi processado ───────────────────────────────
// Retorna true se o commandId já existe no histórico (duplicado).
// Thread-safe: usa mutex interno.
bool cmdHistory_isDuplicate(const char* cmd_id);

// ── Registra um CMD_ID como processado ───────────────────────────────────
// Adiciona o commandId ao buffer circular.
// Thread-safe: usa mutex interno.
void cmdHistory_register(const char* cmd_id);

// ── Limpa todo o histórico ────────────────────────────────────────────────
// Chamado no onDisconnect() para evitar falsos positivos após reconexão.
void cmdHistory_clear();

// ── Retorna o número de entradas no histórico ─────────────────────────────
// Para diagnóstico e logs.
int cmdHistory_count();
