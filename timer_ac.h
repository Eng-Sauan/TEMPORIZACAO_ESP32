#ifndef TIMER_AC_H
#define TIMER_AC_H

#include <Arduino.h>
#include <time.h>
#include <esp_timer.h>

#ifndef AC_MAX_TIMERS
#define AC_MAX_TIMERS 8
#endif

// Tipo de ação
typedef enum {
    AC_TIMER_ACTION_ON = 0,
    AC_TIMER_ACTION_OFF = 1
} ac_timer_action_t;

// Status de retorno
typedef enum {
    AC_TIMER_OK = 0,
    AC_TIMER_FULL = 1,
    AC_TIMER_INVALID_ID = 2
} ac_timer_status_t;

typedef struct {
    int id;                     // id lógico (0..AC_MAX_TIMERS-1)
    int hour;               // 0..23
    int minute;             // 0..59
    ac_timer_action_t action;   // ON / OFF
    bool enabled;               // habilitado?
    bool repeatDaily;           // repete todos os dias?
    int lastFiredDate;          // YYYYMMDD do último disparo para evitar múltiplos no mesmo dia
} ACTimerEntry;

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o módulo (carrega NVS e cria a task que verifica timers).
// sendOn/sendOff são callbacks do usuário (podem imprimir no Serial para teste).
void ac_timers_init(void (*sendOn)(), void (*sendOff)());

// Adiciona timer: hour/minute, action, repeatDaily.
// Retorna AC_TIMER_OK e escreve id em out_id, ou erro.
ac_timer_status_t ac_add_timer(int hour, int minute, ac_timer_action_t action, bool repeatDaily, int *out_id);

// Remove timer por id lógico (0..AC_MAX_TIMERS-1)
ac_timer_status_t ac_remove_timer(int id);

// Habilita/desabilita
ac_timer_status_t ac_set_enabled(int id, bool enabled);

// Força disparo (executa callback imediatamente)
ac_timer_status_t ac_force_fire(int id);

// Retorna ponteiro const para entrada (não liberar)
const ACTimerEntry* ac_get_timer(int id);

// Lista timers ativos preenchendo out_array (até max_entries). Retorna número retornado.
int ac_list_timers(ACTimerEntry *out_array, int max_entries);

// Salva todos timers para NVS (normalmente é automático)
void ac_save_timers_to_nvs();

// Cancela checagem (desliga task) e para tudo
void ac_cancel_all();

#ifdef __cplusplus
}
#endif

#endif // TIMER_AC_H
