#include "timer_ac.h"
#include <Preferences.h>
#include <time.h>

// --------------------------- Config NVS -----------------------------
static Preferences g_prefs;
static const char *PREF_NAMESPACE = "ac_timers";
static const char *TIMER_KEY_PREFIX = "t"; // t0, t1, ...

// --------------------------- Estado --------------------------------
static ACTimerEntry g_timers[AC_MAX_TIMERS];
static void (*g_sendOn)() = nullptr;
static void (*g_sendOff)() = nullptr;
static TaskHandle_t g_task_handle = nullptr;

static String prefsKeyForIndex(int idx) {
    return String(TIMER_KEY_PREFIX) + String(idx);
}

// salva um único timer na NVS (chave tN, valor CSV)
static void save_timer_to_nvs(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return;
    ACTimerEntry &e = g_timers[id];
    String k = prefsKeyForIndex(id);

    if (!e.enabled) {
        if (g_prefs.isKey(k.c_str())) g_prefs.remove(k.c_str());
        return;
    }

    // CSV: enabled,repeat,hour,minute,action,lastFiredDate
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

// carrega todos timers do NVS
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
        // parse CSV
        int parts[6] = {0,0,0,0,0,0};
        int idx = 0, pstart = 0;
        for (int c = 0; c <= val.length() && idx < 6; ++c) {
            if (c == val.length() || val.charAt(c) == ',') {
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

// inicializa estrutura em RAM
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

// função que executa ação de envio
static void do_send_action(const ACTimerEntry &e) {
    if (e.action == AC_TIMER_ACTION_ON) {
        if (g_sendOn) g_sendOn();
    } else {
        if (g_sendOff) g_sendOff();
    }
}

// task que roda a cada 1 segundo e verifica timers
static void timer_watcher_task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        int hh = timeinfo.tm_hour;
        int mm = timeinfo.tm_min;
        int today = date_from_tm(&timeinfo);

        bool changed = false; // para salvar NVS se algum timer for alterado

        for (int i = 0; i < AC_MAX_TIMERS; ++i) {
            ACTimerEntry &t = g_timers[i];
            if (!t.enabled) continue;

            if (t.hour == hh && t.minute == mm) {
                // ainda não disparou hoje?
                if (t.lastFiredDate != today) {
                    Serial.printf("TIMER %d disparou: %02d:%02d action=%d\n", i, t.hour, t.minute, (int)t.action);
                    do_send_action(t);
                    t.lastFiredDate = today;
                    changed = true;

                    if (!t.repeatDaily) {
                        // desabilita se não repetir
                        t.enabled = false;
                        delete_timer_from_nvs(i);
                    }
                }
            }
        }

        if (changed) {
            // salva todos timers modificados (poderíamos salvar apenas alterados)
            for (int i = 0; i < AC_MAX_TIMERS; ++i) save_timer_to_nvs(i);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 segundo
    }
}

// --------------------- API pública --------------------------
void ac_timers_init(void (*sendOn)(), void (*sendOff)()) {
    ensure_initialized_entries();
    g_sendOn = sendOn;
    g_sendOff = sendOff;

    g_prefs.begin(PREF_NAMESPACE, false);
    load_timers_from_nvs();

    // cria task apenas uma vez
    if (g_task_handle == nullptr) {
        xTaskCreatePinnedToCore(timer_watcher_task, "timer_watch", 4096, NULL, 1, &g_task_handle, 1);
    }
}

ac_timer_status_t ac_add_timer(int hour, int minute, ac_timer_action_t action, bool repeatDaily, int *out_id) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return AC_TIMER_INVALID_ID;

    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        if (!g_timers[i].enabled) {
            g_timers[i].hour = hour;
            g_timers[i].minute = minute;
            g_timers[i].action = action;
            g_timers[i].enabled = true;
            g_timers[i].repeatDaily = repeatDaily;
            g_timers[i].lastFiredDate = 0;
            save_timer_to_nvs(i);
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
    return AC_TIMER_OK;
}

ac_timer_status_t ac_set_enabled(int id, bool enabled) {
    if (id < 0 || id >= AC_MAX_TIMERS) return AC_TIMER_INVALID_ID;
    g_timers[id].enabled = enabled;
    save_timer_to_nvs(id);
    return AC_TIMER_OK;
}

ac_timer_status_t ac_force_fire(int id) {
    if (id < 0 || id >= AC_MAX_TIMERS) return AC_TIMER_INVALID_ID;
    ACTimerEntry &e = g_timers[id];
    if (!e.enabled) return AC_TIMER_INVALID_ID;
    do_send_action(e);
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
    for (int i = 0; i < AC_MAX_TIMERS; ++i) save_timer_to_nvs(i);
}

void ac_cancel_all() {
    if (g_task_handle) {
        vTaskDelete(g_task_handle);
        g_task_handle = nullptr;
    }
    for (int i = 0; i < AC_MAX_TIMERS; ++i) {
        g_timers[i].enabled = false;
        delete_timer_from_nvs(i);
    }
    g_prefs.end();
}
