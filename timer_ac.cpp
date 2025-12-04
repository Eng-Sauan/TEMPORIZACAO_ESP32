#include "timer_ac.h"
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// --------------------------- Config NVS -----------------------------
static Preferences g_prefs;
static const char *PREF_NAMESPACE = "ac_timers";
static const char *TIMER_KEY_PREFIX = "t";

// --------------------------- Estado --------------------------------
static ACTimerEntry g_timers[AC_MAX_TIMERS];
static void (*g_sendOn)() = nullptr;
static void (*g_sendOff)() = nullptr;
static TaskHandle_t g_alarm_task_handle = nullptr;
static QueueHandle_t g_alarm_queue = nullptr;
static esp_timer_handle_t g_next_alarm_timer = nullptr;

// Timezone padrão (GMT-3 Brasília)
static long g_gmt_offset_sec = -3 * 3600;
static int g_daylight_offset_sec = 0;

// Estrutura para mensagem de alarme
typedef struct {
    int timer_id;
    ac_timer_action_t action;
} AlarmEvent;

// --------------------------- Declarações Antecipadas --------------------
static void schedule_next_alarm();
static void alarm_timer_callback(void* arg);
static int64_t calculate_next_alarm_us();

// --------------------------- Funções Auxiliares --------------------

static String prefsKeyForIndex(int idx) {
    return String(TIMER_KEY_PREFIX) + String(idx);
}

static void save_timer_to_nvs(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return;
    ACTimerEntry &e = g_timers[id];
    String k = prefsKeyForIndex(id);

    if (!e.enabled) {
        if (g_prefs.isKey(k.c_str())) g_prefs.remove(k.c_str());
        return;
    }

    String val = String(e.enabled ? 1 : 0) + "," +
                 String(e.repeatDaily ? 1 : 0) + "," +
                 String(e.hour) + "," +
                 String(e.minute) + "," +
                 String((int)e.action) + "," +
                 String(e.lastFiredDate);
    g_prefs.putString(k.c_str(), val.c_str());
}

static void delete_timer_from_nvs(int id) {
    String k = prefsKeyForIndex(id);
    if (g_prefs.isKey(k.c_str())) g_prefs.remove(k.c_str());
}

static void load_timers_from_nvs() {
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        String k = prefsKeyForIndex(i);
        if (!g_prefs.isKey(k.c_str())) {
            g_timers[i].enabled = false;
            g_timers[i].lastFiredDate = 0;
            continue;
        }
        String val = g_prefs.getString(k.c_str(), "");
        if (val.length() == 0) {
            g_timers[i].enabled = false;
            g_timers[i].lastFiredDate = 0;
            continue;
        }
        
        int parts[6] = {0,0,0,0,0,0};
        int idx = 0, pstart = 0;
        for (int c = 0; c <= val.length() && idx < 6; ++c) {
            if (c == val.length() || val[c] == ',') {
                String token = val.substring(pstart, c);
                parts[idx++] = token.toInt();
                pstart = c + 1;
            }
        }
        g_timers[i].enabled = (parts[0] != 0);
        g_timers[i].repeatDaily = (parts[1] != 0);
        g_timers[i].hour = parts[2];
        g_timers[i].minute = parts[3];
        g_timers[i].action = (ac_timer_action_t)parts[4];
        g_timers[i].lastFiredDate = parts[5];
    }
}

static void ensure_initialized_entries() {
    static bool inited = false;
    if (inited) return;
    inited = true;
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        g_timers[i].id = i;
        g_timers[i].hour = 0;
        g_timers[i].minute = 0;
        g_timers[i].action = AC_TIMER_ACTION_ON;
        g_timers[i].enabled = false;
        g_timers[i].repeatDaily = false;
        g_timers[i].lastFiredDate = 0;
    }
}

// util: retorna YYYYMMDD a partir de struct tm
static int date_from_tm(struct tm *t) {
    int y = t->tm_year + 1900;
    int m = t->tm_mon + 1;
    int d = t->tm_mday;
    return y * 10000 + m * 100 + d;
}

// Calcula microssegundos até o próximo alarme
static int64_t calculate_next_alarm_us() {
    time_t now = time(nullptr);
    if (now < 24 * 3600) {
        return -1; // Tempo não sincronizado
    }
    
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    
    int64_t min_delta_us = INT64_MAX;
    int today = date_from_tm(&now_tm);
    
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        ACTimerEntry &t = g_timers[i];
        if (!t.enabled) continue;
        
        // Verifica se já disparou hoje
        bool already_fired_today = (t.lastFiredDate == today);
        if (already_fired_today && !t.repeatDaily) continue;
        
        // Calcula segundos até este timer
        struct tm alarm_tm = now_tm;
        alarm_tm.tm_hour = t.hour;
        alarm_tm.tm_min = t.minute;
        alarm_tm.tm_sec = 0;
        
        time_t alarm_time = mktime(&alarm_tm);
        
        // Se o horário já passou hoje e é timer único que já disparou
        if (alarm_time <= now) {
            if (already_fired_today) {
                // Já disparou hoje, próxima execução é amanhã (se repeatDaily)
                if (t.repeatDaily) {
                    alarm_time += 24 * 3600;
                } else {
                    continue; // Timer único já executado hoje
                }
            } else {
                // Ainda não disparou hoje mas horário já passou
                if (t.repeatDaily) {
                    // Timer diário: executa amanhã
                    alarm_time += 24 * 3600;
                } else {
                    // Timer único que perdeu a hora de hoje
                    continue;
                }
            }
        }
        
        int64_t delta_sec = alarm_time - now;
        int64_t delta_us = delta_sec * 1000000LL;
        
        if (delta_us < min_delta_us && delta_us > 0) {
            min_delta_us = delta_us;
        }
    }
    
    return (min_delta_us == INT64_MAX) ? -1 : min_delta_us;
}

// Callback do timer de alta precisão
static void alarm_timer_callback(void* arg) {
    time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    int today = date_from_tm(&now_tm);
    int current_hour = now_tm.tm_hour;
    int current_minute = now_tm.tm_min;
    int current_second = now_tm.tm_sec;
    
    Serial.printf("Interrupcao de alarme! Hora: %02d:%02d:%02d\n", 
                  current_hour, current_minute, current_second);
    
    bool changed = false;
    
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        ACTimerEntry &t = g_timers[i];
        if (!t.enabled) continue;
        
        if (t.hour == current_hour && t.minute == current_minute) {
            if (t.lastFiredDate != today || (t.lastFiredDate == today && t.repeatDaily)) {
                // Prepara evento de alarme
                AlarmEvent event;
                event.timer_id = i;
                event.action = t.action;
                
                // Envia para a fila
                if (g_alarm_queue != nullptr) {
                    xQueueSend(g_alarm_queue, &event, 0);
                }
                
                // Atualiza estado
                t.lastFiredDate = today;
                changed = true;
                
                if (!t.repeatDaily) {
                    t.enabled = false;
                    delete_timer_from_nvs(i);
                } else {
                    save_timer_to_nvs(i);
                }
                
                Serial.printf("  Timer %d disparado: %02d:%02d %s\n", 
                              i, t.hour, t.minute, 
                              t.action == AC_TIMER_ACTION_ON ? "LIGAR" : "DESLIGAR");
            }
        }
    }
    
    if (changed) {
        // Agenda próximo alarme
        schedule_next_alarm();
    }
}

// Agenda o próximo alarme
static void schedule_next_alarm() {
    // Cancela timer anterior se existir
    if (g_next_alarm_timer != nullptr) {
        esp_timer_stop(g_next_alarm_timer);
        esp_timer_delete(g_next_alarm_timer);
        g_next_alarm_timer = nullptr;
    }
    
    // Calcula próximo alarme
    int64_t next_alarm_us = calculate_next_alarm_us();
    
    if (next_alarm_us > 0 && next_alarm_us < (24LL * 3600LL * 1000000LL)) {
        // Cria e inicia novo timer
        esp_timer_create_args_t timer_args = {
            .callback = alarm_timer_callback,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ac_alarm_timer"
        };
        
        if (esp_timer_create(&timer_args, &g_next_alarm_timer) == ESP_OK) {
            esp_timer_start_once(g_next_alarm_timer, next_alarm_us);
            
            // Debug: mostra próximo alarme
            struct tm next_tm;
            time_t now = time(nullptr);
            localtime_r(&now, &next_tm);
            time_t alarm_time = now + (next_alarm_us / 1000000LL);
            localtime_r(&alarm_time, &next_tm);
            
            Serial.printf("Proximo alarme: %02d:%02d:%02d (em %.1f minutos)\n",
                         next_tm.tm_hour, next_tm.tm_min, next_tm.tm_sec,
                         next_alarm_us / 60000000.0);
        }
    } else {
        Serial.println("Nenhum alarme ativo para agendar");
    }
}

// Task que processa os alarmes
static void alarm_processor_task(void *pvParameters) {
    AlarmEvent event;
    
    Serial.println("Task de processamento de alarmes iniciada");
    
    for (;;) {
        // Aguarda evento de alarme
        if (xQueueReceive(g_alarm_queue, &event, portMAX_DELAY)) {
            // Pequeno delay para garantir que estamos no minuto exato
            vTaskDelay(pdMS_TO_TICKS(100));
            
            Serial.printf("Executando acao: Timer %d -> %s\n",
                         event.timer_id,
                         event.action == AC_TIMER_ACTION_ON ? "LIGAR" : "DESLIGAR");
            
            // Executa ação
            if (event.action == AC_TIMER_ACTION_ON) {
                if (g_sendOn) g_sendOn();
            } else {
                if (g_sendOff) g_sendOff();
            }
        }
    }
}

// Configura NTP
static bool setup_ntp_time() {
    const char* ntpServer = "pool.ntp.org";
    configTime(g_gmt_offset_sec, g_daylight_offset_sec, ntpServer);
    
    Serial.print("Sincronizando NTP");
    int retries = 0;
    while (time(nullptr) < 24 * 3600 && retries < 30) {
        Serial.print(".");
        delay(1000);
        retries++;
    }
    
    if (retries >= 30) {
        Serial.println(" Falha NTP");
        return false;
    }
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    Serial.printf(" OK %02d:%02d:%02d\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
}

// --------------------------- API Pública --------------------------

void ac_timers_set_timezone(long gmt_offset_sec, int daylight_offset_sec) {
    g_gmt_offset_sec = gmt_offset_sec;
    g_daylight_offset_sec = daylight_offset_sec;
}

void ac_timers_init(void (*sendOn)(), void (*sendOff)()) {
    ensure_initialized_entries();
    g_sendOn = sendOn;
    g_sendOff = sendOff;

    // Inicializa NVS
    g_prefs.begin(PREF_NAMESPACE, false);
    load_timers_from_nvs();
    
    Serial.println("Sistema de timers inicializado");

    // Cria fila para eventos de alarme
    g_alarm_queue = xQueueCreate(10, sizeof(AlarmEvent));
    
    if (g_alarm_queue == NULL) {
        Serial.println("Falha ao criar fila de alarmes");
        return;
    }
    
    // Cria task para processar alarmes
    if (g_alarm_task_handle == nullptr) {
        xTaskCreatePinnedToCore(
            alarm_processor_task, 
            "alarm_processor", 
            4096, 
            NULL, 
            2,  // Prioridade alta
            &g_alarm_task_handle, 
            1   // Core 1
        );
    }
    
    // Sincroniza tempo via NTP
    if (setup_ntp_time()) {
        // Agenda primeiro alarme após NTP sincronizado
        schedule_next_alarm();
    } else {
        Serial.println("Aguardando sincronizacao de tempo para agendar alarmes");
        // Tenta novamente em 30 segundos
        esp_timer_create_args_t retry_args = {
            .callback = [](void* arg) {
                if (time(nullptr) >= 24 * 3600) {
                    schedule_next_alarm();
                }
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ntp_retry"
        };
        esp_timer_handle_t retry_timer;
        esp_timer_create(&retry_args, &retry_timer);
        esp_timer_start_once(retry_timer, 30000000); // 30 segundos
    }
}

ac_timer_status_t ac_add_timer(int hour, int minute, ac_timer_action_t action, 
                              bool repeatDaily, int *out_id) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return AC_TIMER_INVALID_ID;
    }

    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        if (!g_timers[i].enabled) {
            g_timers[i].hour = hour;
            g_timers[i].minute = minute;
            g_timers[i].action = action;
            g_timers[i].enabled = true;
            g_timers[i].repeatDaily = repeatDaily;
            g_timers[i].lastFiredDate = 0;
            
            save_timer_to_nvs(i);
            
            // Reagenda alarmes
            schedule_next_alarm();
            
            if (out_id) *out_id = i;
            return AC_TIMER_OK;
        }
    }
    return AC_TIMER_FULL;
}

ac_timer_status_t ac_remove_timer(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return AC_TIMER_INVALID_ID;
    ACTimerEntry &e = g_timers[id];
    if (!e.enabled) return AC_TIMER_INVALID_ID;
    
    e.enabled = false;
    e.repeatDaily = false;
    e.lastFiredDate = 0;
    
    delete_timer_from_nvs(id);
    
    // Reagenda alarmes
    schedule_next_alarm();
    
    return AC_TIMER_OK;
}

ac_timer_status_t ac_set_enabled(int id, bool enabled) {
    if (id < 0 || id >= AC_MAX_TIMERS) return AC_TIMER_INVALID_ID;
    
    g_timers[id].enabled = enabled;
    if (!enabled) {
        g_timers[id].lastFiredDate = 0;
    }
    
    save_timer_to_nvs(id);
    
    // Reagenda alarmes
    schedule_next_alarm();
    
    return AC_TIMER_OK;
}

ac_timer_status_t ac_force_fire(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return AC_TIMER_INVALID_ID;
    ACTimerEntry &e = g_timers[id];
    if (!e.enabled) return AC_TIMER_INVALID_ID;
    
    // Envia evento de alarme imediatamente
    AlarmEvent event;
    event.timer_id = id;
    event.action = e.action;
    
    if (g_alarm_queue != nullptr) {
        xQueueSend(g_alarm_queue, &event, 0);
    }
    
    return AC_TIMER_OK;
}

const ACTimerEntry* ac_get_timer(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return nullptr;
    return &g_timers[id];
}

int ac_list_timers(ACTimerEntry *out_array, int max_entries) {
    int c = 0;
    for (int i = 0; i < AC_MAX_TIMERS && c < max_entries; ++i) {
        if (g_timers[i].enabled) {
            out_array[c++] = g_timers[i];
        }
    }
    return c;
}

void ac_save_timers_to_nvs() {
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        save_timer_to_nvs(i);
    }
}

void ac_cancel_all() {
    // Cancela task de processamento
    if (g_alarm_task_handle) {
        vTaskDelete(g_alarm_task_handle);
        g_alarm_task_handle = nullptr;
    }
    
    // Cancela timer de alarme
    if (g_next_alarm_timer != nullptr) {
        esp_timer_stop(g_next_alarm_timer);
        esp_timer_delete(g_next_alarm_timer);
        g_next_alarm_timer = nullptr;
    }
    
    // Limpa fila
    if (g_alarm_queue != nullptr) {
        vQueueDelete(g_alarm_queue);
        g_alarm_queue = nullptr;
    }
    
    // Limpa todos timers
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        g_timers[i].enabled = false;
        delete_timer_from_nvs(i);
    }
    
    g_prefs.end();
}

bool ac_has_active_timers() {
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        if (g_timers[i].enabled) return true;
    }
    return false;
}