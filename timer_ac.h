#ifndef TIMER_AC_H
#define TIMER_AC_H

#include <Arduino.h>

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
    int hour;                   // 0..23
    int minute;                 // 0..59
    ac_timer_action_t action;   // ON / OFF
    bool enabled;               // habilitado?
    bool repeatDaily;           // repete todos os dias?
    int lastFiredDate;          // YYYYMMDD do último disparo
} ACTimerEntry;

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o módulo com sistema baseado em interrupções
void ac_timers_init(void (*sendOn)(), void (*sendOff)());

// Adiciona timer: hour/minute, action, repeatDaily.
ac_timer_status_t ac_add_timer(int hour, int minute, ac_timer_action_t action, bool repeatDaily, int *out_id);

// Remove timer por id lógico (0..AC_MAX_TIMERS-1)
ac_timer_status_t ac_remove_timer(int id);

// Habilita/desabilita
ac_timer_status_t ac_set_enabled(int id, bool enabled);

// Força disparo (executa callback imediatamente)
ac_timer_status_t ac_force_fire(int id);

// Retorna ponteiro const para entrada (não liberar)
const ACTimerEntry* ac_get_timer(int id);

// Lista timers ativos
int ac_list_timers(ACTimerEntry *out_array, int max_entries);

// Salva todos timers para NVS
void ac_save_timers_to_nvs();

// Cancela tudo e limpa recursos
void ac_cancel_all();

// Verifica se há timers ativos
bool ac_has_active_timers();

// Configura o timezone (opcional, padrão é GMT-3)
void ac_timers_set_timezone(long gmt_offset_sec, int daylight_offset_sec);

#ifdef __cplusplus
}
#endif

#endif // TIMER_AC_H