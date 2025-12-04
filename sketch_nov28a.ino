#include <Arduino.h>
#include "timer_ac.h"
#include <sys/time.h>
#include <WiFi.h>
#include <IRremote.h>

// Configurações WiFi
const char* WIFI_SSID = "Sem senha";
const char* WIFI_PASSWORD = "canariohouse";

// Configuração IR
const int IR_LED_PIN = 4;
IRsend irsend;

// Códigos IR do ar-condicionado
const unsigned long AC_ON_CODE = 0xE0E09966;
const unsigned long AC_OFF_CODE = 0xE0E019E6;

void sendOn() {
  Serial.println("IR -> LIGAR AR-CONDICIONADO");
  irsend.sendNEC(AC_ON_CODE, 32);
  delay(100);
}

void sendOff() {
  Serial.println("IR -> DESLIGAR AR-CONDICIONADO");
  irsend.sendNEC(AC_OFF_CODE, 32);
}

bool connectWiFi() {
  Serial.printf("Conectando ao WiFi: %s", WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.println("\nFalha na conexao WiFi!");
    return false;
  }
}

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
  Serial.printf("Relogio setado manualmente: %02d:%02d:%02d\n", hour, min, sec);
}

void programarMeusTimers() {
  Serial.println("PROGRAMANDO TIMERS:");
  
  int id;
  
  // Limpa timers antigos (opcional - descomente se quiser limpar)
  // ac_cancel_all();
  
  // Adiciona timers desejados
  if (ac_add_timer(7, 0, AC_TIMER_ACTION_ON, true, &id) == AC_TIMER_OK) {
    Serial.printf("   LIGAR: 07:00 (ID=%d, DIARIO)\n", id);
  }
  
  if (ac_add_timer(8, 0, AC_TIMER_ACTION_OFF, true, &id) == AC_TIMER_OK) {
    Serial.printf("   DESLIGAR: 08:00 (ID=%d, DIARIO)\n", id);
  }
  
  if (ac_add_timer(18, 0, AC_TIMER_ACTION_ON, true, &id) == AC_TIMER_OK) {
    Serial.printf("   LIGAR: 18:00 (ID=%d, DIARIO)\n", id);
  }
  
  if (ac_add_timer(23, 0, AC_TIMER_ACTION_OFF, true, &id) == AC_TIMER_OK) {
    Serial.printf("   DESLIGAR: 23:00 (ID=%d, DIARIO)\n", id);
  }
  
  // Timer de teste (2 minutos no futuro)
  time_t now = time(nullptr);
  struct tm now_tm;
  localtime_r(&now, &now_tm);
  
  int teste_hora = now_tm.tm_hour;
  int teste_minuto = now_tm.tm_min + 2;
  
  if (teste_minuto >= 60) {
    teste_minuto -= 60;
    teste_hora = (teste_hora + 1) % 24;
  }
  
  if (ac_add_timer(teste_hora, teste_minuto, AC_TIMER_ACTION_ON, false, &id) == AC_TIMER_OK) {
    Serial.printf("   TESTE: LIGAR %02d:%02d (ID=%d, UNICO)\n", teste_hora, teste_minuto, id);
  }
}

void listarTimers() {
  ACTimerEntry timers[AC_MAX_TIMERS];
  int count = ac_list_timers(timers, AC_MAX_TIMERS);
  
  if (count == 0) {
    Serial.println("Nenhum timer programado");
    return;
  }
  
  Serial.printf("TIMERS ATIVOS (%d/%d):\n", count, AC_MAX_TIMERS);
  Serial.println("ID  HORA    ACAO       REPETICAO");
  Serial.println("--------------------------------");
  
  for (int i = 0; i < count; i++) {
    const ACTimerEntry &t = timers[i];
    
    char horaStr[6];
    sprintf(horaStr, "%02d:%02d", t.hour, t.minute);
    
    const char* acao = (t.action == AC_TIMER_ACTION_ON) ? "LIGAR  " : "DESLIGAR";
    const char* repeticao = t.repeatDaily ? "DIARIO" : "UNICO  ";
    
    Serial.printf("%-3d %-7s %-10s %s\n", t.id, horaStr, acao, repeticao);
  }
}

void adicionarTimerSerial() {
  Serial.println("ADICIONAR NOVO TIMER:");
  
  Serial.print("Hora (0-23): ");
  while (!Serial.available());
  int hora = Serial.parseInt();
  
  Serial.print("Minuto (0-59): ");
  while (!Serial.available());
  int minuto = Serial.parseInt();
  
  Serial.print("Acao (0=LIGAR, 1=DESLIGAR): ");
  while (!Serial.available());
  int acaoInt = Serial.parseInt();
  ac_timer_action_t acao = (acaoInt == 0) ? AC_TIMER_ACTION_ON : AC_TIMER_ACTION_OFF;
  
  Serial.print("Repetir diariamente? (0=NAO, 1=SIM): ");
  while (!Serial.available());
  bool repetir = (Serial.parseInt() == 1);
  
  int id;
  if (ac_add_timer(hora, minuto, acao, repetir, &id) == AC_TIMER_OK) {
    Serial.printf("Timer adicionado! ID=%d\n", id);
  } else {
    Serial.println("Erro ao adicionar timer!");
  }
  
  while (Serial.available()) Serial.read();
}

void removerTimer(int id) {
  if (ac_remove_timer(id) == AC_TIMER_OK) {
    Serial.printf("Timer ID=%d removido!\n", id);
  } else {
    Serial.printf("Erro ao remover timer ID=%d\n", id);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== CONTROLE DE TIMERS PARA AR-CONDICIONADO ===");
  
  // Inicializar IR
  Serial.print("Inicializando IR... ");
  irsend.begin(IR_LED_PIN);
  Serial.println("OK");
  
  // Conectar WiFi
  Serial.print("Conectando WiFi... ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK (IP: %s)\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(" FALHA!");
    setManualTime(2025, 11, 29, 10, 30, 0);
  }
  
  // Inicializar sistema de timers
  Serial.print("Inicializando timers... ");
  ac_timers_init(sendOn, sendOff);
  Serial.println("OK");
  
  // Programar timers
  programarMeusTimers();
  
  // Listar timers
  listarTimers();
  
  // Mostrar hora atual
  time_t agora = time(nullptr);
  struct tm infoHora;
  localtime_r(&agora, &infoHora);
  Serial.printf("Hora atual: %02d:%02d:%02d\n", 
                infoHora.tm_hour, infoHora.tm_min, infoHora.tm_sec);
  
  // Menu via Serial
  Serial.println("COMANDOS DISPONIVEIS:");
  Serial.println("  L - Listar timers");
  Serial.println("  A - Adicionar timer");
  Serial.println("  R - Remover timer (ex: R3 para remover ID 3)");
  Serial.println("  T - Testar IR LIGAR");
  Serial.println("  D - Testar IR DESLIGAR");
  Serial.println("  ? - Mostrar ajuda");
}

void loop() {
  if (Serial.available() > 0) {
    char comando = Serial.read();
    
    switch (comando) {
      case 'L':
      case 'l':
        listarTimers();
        break;
        
      case 'A':
      case 'a':
        adicionarTimerSerial();
        break;
        
      case 'R':
      case 'r':
        {
          Serial.print("ID do timer para remover: ");
          while (!Serial.available());
          int idRemover = Serial.parseInt();
          removerTimer(idRemover);
          while (Serial.available()) Serial.read();
        }
        break;
        
      case 'T':
      case 't':
        Serial.println("Testando IR LIGAR...");
        sendOn();
        break;
        
      case 'D':
      case 'd':
        Serial.println("Testando IR DESLIGAR...");
        sendOff();
        break;
        
      case '?':
        Serial.println("COMANDOS:");
        Serial.println("  L - Listar timers");
        Serial.println("  A - Adicionar timer");
        Serial.println("  R id - Remover timer (ex: R3)");
        Serial.println("  T - Testar IR LIGAR");
        Serial.println("  D - Testar IR DESLIGAR");
        Serial.println("  ? - Ajuda");
        break;
        
      default:
        if (comando != '\n' && comando != '\r') {
          Serial.printf("Comando desconhecido: '%c'\n", comando);
          Serial.println("Digite ? para ajuda");
        }
    }
  }
  
  static unsigned long ultimaHora = 0;
  if (millis() - ultimaHora > 30000) {
    ultimaHora = millis();
    time_t agora = time(nullptr);
    struct tm infoHora;
    localtime_r(&agora, &infoHora);
    Serial.printf("%02d:%02d:%02d\n", 
                  infoHora.tm_hour, infoHora.tm_min, infoHora.tm_sec);
  }
  
  delay(100);
}