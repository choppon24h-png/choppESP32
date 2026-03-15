#include "command_parser.h"
#include "command_history.h"
#include "command_queue.h"
#include "valve_controller.h"
#include "flow_sensor.h"
#include "watchdog.h"
#include "ble_protocol.h"
#include <string.h>
#include <stdlib.h>

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: command_parser.cpp — v2.1 (Robustez Industrial)
// ═══════════════════════════════════════════════════════════════════════════
//
// MELHORIAS v2.1:
//   - Integração com command_history (20 IDs, thread-safe, limpo no disconnect)
//   - Integração com command_queue (FIFO 5, QUEUE:FULL, STOP com prioridade)
//   - ACK IMEDIATO garantido ANTES de qualquer execução (< 100ms)
//   - Logs industriais padronizados: RX: / TX: / EXEC: / DONE / ERROR:
//   - Timestamp em cada log para rastreabilidade
// ═══════════════════════════════════════════════════════════════════════════

// ── Macro de log industrial com timestamp ─────────────────────────────────
#define LOG_RX(cmd)   DBG_PRINTF("[%llu] RX: %s\n",   (uint64_t)esp_timer_get_time()/1000ULL, cmd)
#define LOG_TX(resp)  DBG_PRINTF("[%llu] TX: %s\n",   (uint64_t)esp_timer_get_time()/1000ULL, resp)
#define LOG_EXEC(cmd, val) DBG_PRINTF("[%llu] EXEC: %s %s\n", (uint64_t)esp_timer_get_time()/1000ULL, cmd, val)
#define LOG_DONE()    DBG_PRINTF("[%llu] DONE\n",      (uint64_t)esp_timer_get_time()/1000ULL)
#define LOG_ERR(msg)  DBG_PRINTF("[%llu] ERROR: %s\n", (uint64_t)esp_timer_get_time()/1000ULL, msg)

// ── Wrapper de envio com log TX ───────────────────────────────────────────
static void sendAndLog(const char* resp) {
    LOG_TX(resp);
    bleProtocol_send(resp);
}

// ── Inicialização ─────────────────────────────────────────────────────────
void commandParser_init() {
    cmdHistory_init();
    cmdQueue_init();
    DBG_PRINTLN("[PARSER] Inicializado v2.1 (command_history + command_queue)");
}

// ── Deduplicação (delegada ao command_history) ────────────────────────────
bool commandParser_isDuplicate(const char* cmd_id) {
    return cmdHistory_isDuplicate(cmd_id);
}

void commandParser_registerCmdId(const char* cmd_id) {
    cmdHistory_register(cmd_id);
}

// ── Parser ────────────────────────────────────────────────────────────────
// Formato: $CMD:VALOR:CMDID  ou  $CMD:CMDID  ou  $CMD:VALOR  ou  $CMD
bool commandParser_parse(const char* raw, ParsedCommand* out) {
    if (!raw || !out) return false;

    memset(out, 0, sizeof(ParsedCommand));

    if (raw[0] != PROTO_PREFIX) {
        LOG_ERR("Prefixo inválido");
        return false;
    }

    char buf[PROTO_RX_BUFFER_SIZE];
    strncpy(buf, raw + 1, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* token1 = strtok(buf, ":");
    char* token2 = strtok(NULL, ":");
    char* token3 = strtok(NULL, ":");

    if (!token1) return false;

    strncpy(out->cmd, token1, sizeof(out->cmd) - 1);

    if (token2) {
        if (token3) {
            // $CMD:VALOR:CMDID
            strncpy(out->value,  token2, sizeof(out->value)  - 1);
            strncpy(out->cmd_id, token3, sizeof(out->cmd_id) - 1);
            out->has_value  = true;
            out->has_cmd_id = true;
        } else {
            bool isNumeric = (token2[0] >= '0' && token2[0] <= '9');
            bool cmdHasValue = (strcmp(out->cmd, CMD_ML)   == 0 ||
                                strcmp(out->cmd, CMD_PL)   == 0 ||
                                strcmp(out->cmd, CMD_TO)   == 0 ||
                                strcmp(out->cmd, CMD_AUTH) == 0);

            if (cmdHasValue || isNumeric) {
                strncpy(out->value, token2, sizeof(out->value) - 1);
                out->has_value  = true;
                out->has_cmd_id = false;
            } else {
                strncpy(out->cmd_id, token2, sizeof(out->cmd_id) - 1);
                out->has_value  = false;
                out->has_cmd_id = true;
            }
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLERS — ACK IMEDIATO ANTES DE QUALQUER EXECUÇÃO
// ═══════════════════════════════════════════════════════════════════════════

static void handleAuth(const ParsedCommand* cmd) {
    // AUTH não precisa de ACK separado — AUTH:OK é o próprio ACK
    if (!cmd->has_value) {
        sendAndLog(RESP_AUTH_FAIL);
        return;
    }
    if (strcmp(cmd->value, BLE_AUTH_PIN) == 0) {
        g_opState.bleAutenticado = true;
        sendAndLog(RESP_AUTH_OK);
        LOG_EXEC(CMD_AUTH, "OK");
    } else {
        g_opState.bleAutenticado = false;
        sendAndLog(RESP_AUTH_FAIL);
        LOG_ERR("AUTH:FAIL — PIN incorreto");
    }
}

static void handleML(const ParsedCommand* cmd) {
    if (!cmd->has_value) {
        sendAndLog(RESP_ERROR_INVALID);
        return;
    }

    uint32_t ml = (uint32_t)atol(cmd->value);
    if (ml == 0 || ml > 5000) {
        sendAndLog(RESP_ERROR_INVALID);
        LOG_ERR("ML valor inválido");
        return;
    }

    // ── ACK IMEDIATO (< 100ms) — ANTES de iniciar a dispensação ──────────
    sendAndLog(RESP_ML_ACK);
    watchdog_kick();

    // ── Executa a operação APÓS o ACK ─────────────────────────────────────
    LOG_EXEC(CMD_ML, cmd->value);
    if (!valveController_startDispensacao(ml)) {
        sendAndLog(RESP_ERROR_BUSY);
        LOG_ERR("ML:BUSY — dispensação já em andamento");
    }
}

static void handleStop(const ParsedCommand* cmd) {
    // ── ACK imediato para STOP ────────────────────────────────────────────
    sendAndLog(RESP_STOP_OK);
    LOG_EXEC(CMD_STOP, "");

    // Limpa a fila de comandos pendentes
    cmdQueue_clear();

    // Para a válvula (envia VALVE:CLOSED internamente)
    valveController_stop("CMD_STOP");
    LOG_DONE();
}

static void handleStatus(const ParsedCommand* cmd) {
    const char* resp;
    switch (g_opState.state) {
        case SYS_RUNNING:  resp = RESP_STATUS_RUNNING; break;
        case SYS_ERROR:    resp = RESP_STATUS_ERROR;   break;
        default:           resp = RESP_STATUS_IDLE;    break;
    }
    sendAndLog(resp);
    LOG_EXEC(CMD_STATUS, resp);
}

static void handlePing(const ParsedCommand* cmd) {
    sendAndLog(RESP_PONG);
    watchdog_kick();
    // Sem LOG_EXEC para não poluir o log com PINGs frequentes
}

static void handlePL(const ParsedCommand* cmd) {
    if (!cmd->has_value) { sendAndLog(RESP_ERROR_INVALID); return; }
    uint32_t pl = (uint32_t)atol(cmd->value);
    if (pl == 0) { sendAndLog(RESP_ERROR_INVALID); return; }
    flowSensor_setPulsosLitro(pl);
    sendAndLog(RESP_PL_ACK);
    LOG_EXEC(CMD_PL, cmd->value);
}

static void handleTO(const ParsedCommand* cmd) {
    if (!cmd->has_value) { sendAndLog(RESP_ERROR_INVALID); return; }
    uint32_t to = (uint32_t)atol(cmd->value);
    if (to < 1000) to = 1000;
    g_opState.timeoutSensor = to;
    sendAndLog(RESP_TO_ACK);
    LOG_EXEC(CMD_TO, cmd->value);
}

// ═══════════════════════════════════════════════════════════════════════════
// TAREFA FREERTOS: taskCommandProcessor v2.1
// Lê da command_queue (FIFO 5) em vez da fila FreeRTOS direta.
// Garante execução sequencial sem concorrência.
// ═══════════════════════════════════════════════════════════════════════════
void taskCommandProcessor(void* param) {
    char rawCmd[PROTO_RX_BUFFER_SIZE];

    DBG_PRINTLN("[PARSER] taskCommandProcessor iniciada (v2.1)");

    for (;;) {
        // ── Aguarda próximo comando da command_queue (bloqueante) ─────────
        if (!cmdQueue_dequeue(rawCmd, sizeof(rawCmd), portMAX_DELAY)) continue;

        // ── Log RX industrial ─────────────────────────────────────────────
        LOG_RX(rawCmd);

        // ── Parse ─────────────────────────────────────────────────────────
        ParsedCommand cmd;
        if (!commandParser_parse(rawCmd, &cmd)) {
            LOG_ERR("Comando inválido — parse falhou");
            sendAndLog(RESP_ERROR_INVALID);
            continue;
        }

        // ── Deduplicação via command_history ──────────────────────────────
        if (cmd.has_cmd_id && cmdHistory_isDuplicate(cmd.cmd_id)) {
            DBG_PRINTF("[PARSER] DUPLICADO: cmd=%s id=%s | hist=%d\n",
                       cmd.cmd, cmd.cmd_id, cmdHistory_count());
            if (strcmp(cmd.cmd, CMD_ML) == 0) {
                sendAndLog(RESP_ML_DUPLICATE);
            } else {
                sendAndLog(RESP_DUPLICATE);
            }
            continue;
        }

        // ── Registra CMD_ID no histórico ──────────────────────────────────
        if (cmd.has_cmd_id) {
            cmdHistory_register(cmd.cmd_id);
        }

        // ── Verifica autenticação ─────────────────────────────────────────
        bool requerAuth = (strcmp(cmd.cmd, CMD_AUTH)   != 0 &&
                           strcmp(cmd.cmd, CMD_PING)   != 0 &&
                           strcmp(cmd.cmd, CMD_STATUS) != 0);

        if (requerAuth && !g_opState.bleAutenticado) {
            LOG_ERR("Comando sem autenticação");
            sendAndLog(RESP_ERROR_AUTH);
            continue;
        }

        // ── Despacha handler ──────────────────────────────────────────────
        if      (strcmp(cmd.cmd, CMD_AUTH)   == 0) handleAuth(&cmd);
        else if (strcmp(cmd.cmd, CMD_ML)     == 0) handleML(&cmd);
        else if (strcmp(cmd.cmd, CMD_STOP)   == 0) handleStop(&cmd);
        else if (strcmp(cmd.cmd, CMD_STATUS) == 0) handleStatus(&cmd);
        else if (strcmp(cmd.cmd, CMD_PING)   == 0) handlePing(&cmd);
        else if (strcmp(cmd.cmd, CMD_PL)     == 0) handlePL(&cmd);
        else if (strcmp(cmd.cmd, CMD_TO)     == 0) handleTO(&cmd);
        else {
            LOG_ERR("Comando desconhecido");
            sendAndLog(RESP_ERROR_INVALID);
        }
    }
}
