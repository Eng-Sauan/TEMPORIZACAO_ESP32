#include <Arduino.h>
#include "timer_ac.h"
#include <sys/time.h>

// Funções de envio (test doubles)
void sendOn() {
  Serial.println("[IR] -> LIGAR (fake)");
}
void sendOff() {
  Serial.println("[IR] -> DESLIGAR (fake)");
}

// Ajusta hora do sistema manualmente: YYYY,MM,DD,HH,MM,SS
void setManualTime(int year, int month, int day, int hour, int min, int sec) {
  struct tm t;
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = min;
  t.tm_sec  = sec;
  t.tm_isdst = -1;
  time_t tt = mktime(&t);
  struct timeval tv = { .tv_sec = tt, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  Serial.printf("Relógio setado: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, min, sec);
}

void printTimers() {
  ACTimerEntry arr[AC_MAX_TIMERS];
  int n = ac_list_timers(arr, AC_MAX_TIMERS);
  Serial.printf("Timers salvos: %d\n", n);
  for (int i = 0; i < n; ++i) {
    const ACTimerEntry &t = arr[i];
    Serial.printf("ID=%d %02d:%02d action=%d repeat=%d lastFired=%d\n",
                  t.id, t.hour, t.minute, (int)t.action, t.repeatDaily, t.lastFiredDate);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== TESTE TEMPORIZAÇÃO (HORÁRIO MANUAL) ===");

  // Ajuste manual da hora local (exemplo)
  setManualTime(2025, 11, 29, 3, 15, 0); // ajuste conforme necessário

  // Inicializa módulo com callbacks fake
  ac_timers_init(sendOn, sendOff);

  // Remover todos timers existentes (opcional, somente para teste limpo)
  // ac_cancel_all();

  // Adiciona timers de exemplo (mude para testar)
  int id;
  if (ac_add_timer(3, 18, AC_TIMER_ACTION_ON, false, &id) == AC_TIMER_OK) {
    Serial.printf("Criado timer ID=%d -> LIGAR 14:31 (one-shot)\n", id);
  }
  if (ac_add_timer(3, 19, AC_TIMER_ACTION_OFF, true, &id) == AC_TIMER_OK) {
    Serial.printf("Criado timer ID=%d -> DESLIGAR 14:32 (repetir diário)\n", id);
  }

  printTimers();
}

void loop() {
  // nada aqui — o módulo faz a verificação em background
  delay(1000);
}
