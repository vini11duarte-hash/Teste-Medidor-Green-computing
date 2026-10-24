#include <LiquidCrystal.h>
#include <SPIFFS.h>
#include <FS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

Preferences prefs;

// =======================================================
// SELEÇÃO DO MODO
// =======================================================
int MODO = 3;
int cal = 3;
String htmlPadrao = "Jarvis.htm";

// =======================================================
// PINOS
// =======================================================
const uint8_t pinAdcTensao = 32;
const uint8_t pinAdcCorrente = 33;

const uint8_t pinBtnNext = 13;
const uint8_t pinBtnPrev = 12;
const uint8_t pinBtnOK = 14;

// LCD 16x2
LiquidCrystal lcd(16, 17, 5, 18, 19, 21);

// =======================================================
// WI-FI / SERVIDOR
// =======================================================
AsyncWebServer server(80);
const char* ssid = "Fox-trot";
const char* senha = "squidward";

// Configurações do Wi-Fi Próprio (Ponto de Acesso)
const char* ap_ssid = "Medidor do PET";
const char* ap_senha = "PETELÉTRICA";

// =======================================================
// WebSocket
// =======================================================
WebSocketsServer webSocket(81);

// =======================================================
// CALIBRAÇÃO / OFFSET
// =======================================================
float offsetVAC = 1824;
float offsetIAC = 1841;

float coefA_VAC = 2.319112075012432e-01;
float coefB_VAC = 0.0;
float coefA_IAC = 6.655866633537638e-03;
float coefB_IAC = 0.0;

// =======================================================
// ESTRUTURA DOS DADOS
// =======================================================
struct Medicao {
  int32_t adcV = 0;
  int32_t adcI = 0;

  float vInst = 0.0;
  float iInst = 0.0;

  float vRMS = 0.0;
  float iRMS = 0.0;

  float potencia = 0.0;
  float potenciaAparente = 0.0;
  float fatorPotencia = 0.0;
  float energia = 0.0;      
  float consumoKWh = 0.0;   

  uint32_t tempoMs = 0;
};

// =======================================================
// DADOS COMPARTILHADOS ENTRE TASKS
// =======================================================
Medicao dadosCompartilhados;
portMUX_TYPE dadosMux = portMUX_INITIALIZER_UNLOCKED;

Medicao lerSnapshot() {
  Medicao copia;
  taskENTER_CRITICAL(&dadosMux);
  copia = dadosCompartilhados;
  taskEXIT_CRITICAL(&dadosMux);
  return copia;
}

void publicarSnapshot(const Medicao& novo) {
  taskENTER_CRITICAL(&dadosMux);
  dadosCompartilhados = novo;
  taskEXIT_CRITICAL(&dadosMux);
}

// =======================================================
// CONTROLE DE ESTADO
// =======================================================
volatile bool isPaused = false;
volatile bool isRecording = false;

volatile bool pedidoIniciarGravacao = false;
volatile bool pedidoPararGravacao = false;
volatile bool pedidoFormatarSPIFFS = false;
volatile bool pedidoResetConsumo = false;

int currentScreen = 0;
int lastScreen = -1;

unsigned long recordingStartMillis = 0;
unsigned long lastLogMillis = 0;
float energiaTotalWh = 0.0;

File logFile;

// =======================================================
// PROTÓTIPOS
// =======================================================
void tarefaMedicao(void* pv);
void tarefaInterface(void* pv);
void tarefaComunicacao(void* pv);

void iniciarGravacao();
void pararGravacao();

void desenharTela(const Medicao& d);
String proximaSessao();

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);

  prefs.begin("config", true);
  cal = prefs.getInt("cal", 3);
  MODO = cal;
  coefA_VAC = prefs.getFloat("coefA_VAC", 1.0);
  coefB_VAC = prefs.getFloat("coefB_VAC", 0.0);
  coefA_IAC = prefs.getFloat("coefA_IAC", 1.0);
  coefB_IAC = prefs.getFloat("coefB_IAC", 0.0);
  htmlPadrao = prefs.getString("htmlPadrao", "Jarvis.htm");
  prefs.end();

  analogReadResolution(12);
  analogSetPinAttenuation(pinAdcTensao, ADC_11db);
  analogSetPinAttenuation(pinAdcCorrente, ADC_11db);

  pinMode(pinBtnNext, INPUT_PULLUP);
  pinMode(pinBtnPrev, INPUT_PULLUP);
  pinMode(pinBtnOK, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcd.clear();

  if (!SPIFFS.begin(true)) {
    Serial.println("Falha ao montar SPIFFS");
  }

  // WI-FI HÍBRIDO
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, senha);
  Serial.print("Conectando no Wi-Fi");

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Criando Ponto de Acesso...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid, ap_senha);
    Serial.print("Rede criada: ");
    Serial.println(ap_ssid);
  }

  if (!MDNS.begin("green")) {
    Serial.println("Erro mDNS!");
  } else {
    Serial.println("mDNS: http://green.local");
  }

  // CABEÇALHOS CORS
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      request->send(404, "text/plain", "Not found");
    }
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String path = htmlPadrao.startsWith("/") ? htmlPadrao : "/" + htmlPadrao;
    if (SPIFFS.exists(path)) {
      request->send(SPIFFS, path, "text/html");
    } else if (SPIFFS.exists("/Jarvis.html")) {
      request->send(SPIFFS, "/Jarvis.html", "text/html");
    } else if (SPIFFS.exists("/jarvis.htm")) {
      request->send(SPIFFS, "/jarvis.htm", "text/html");
    } else {
      request->send(200, "text/plain", "Servidor ESP32 Ativo. Nenhum HTML padrao encontrado no SPIFFS.");
    }
  });

  // ROTA DE UPLOAD DE ARQUIVOS (HTML / CSV)
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Upload concluido!");
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      if (!filename.startsWith("/")) filename = "/" + filename;
      request->_tempFile = SPIFFS.open(filename, FILE_WRITE);
      Serial.printf("Upload Iniciado: %s\n", filename.c_str());
    }
    if (request->_tempFile) {
      request->_tempFile.write(data, len);
    }
    if (final) {
      if (request->_tempFile) request->_tempFile.close();
      Serial.printf("Upload Finalizado: %s, %u B\n", filename.c_str(), index + len);
    }
  });

  server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{";
    json += "\"htmlPadrao\":\"" + htmlPadrao + "\",";
    json += "\"cal\":" + String(cal) + ",";
    json += "\"coefA_VAC\":" + String(coefA_VAC, 6) + ",";
    json += "\"coefB_VAC\":" + String(coefB_VAC, 6) + ",";
    json += "\"coefA_IAC\":" + String(coefA_IAC, 6) + ",";
    json += "\"coefB_IAC\":" + String(coefB_IAC, 6);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED) || (WiFi.softAPgetStationNum() > 0);
    String json = "{";
    json += "\"wifi\":" + String(wifiOk ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/spiffs", HTTP_GET, [](AsyncWebServerRequest* request) {
    size_t total = SPIFFS.totalBytes();
    size_t usado = SPIFFS.usedBytes();
    float livreMB = (float)(total - usado) / 1048576.0f;

    String json = "{";
    json += "\"total\":" + String(total) + ",";
    json += "\"usado\":" + String(usado) + ",";
    json += "\"livre\":" + String(livreMB, 2);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/setHtmlPadrao", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("nome")) {
      htmlPadrao = request->getParam("nome")->value();
      prefs.begin("config", false);
      prefs.putString("htmlPadrao", htmlPadrao);
      prefs.end();
      request->send(200, "text/plain", "HTML padrao alterado para " + htmlPadrao);
    } else {
      request->send(400, "text/plain", "Parametro 'nome' faltando");
    }
  });

  server.on("/setCal", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("valor")) {
      cal = request->getParam("valor")->value().toInt();
      MODO = cal;
      prefs.begin("config", false);
      prefs.putInt("cal", cal);
      prefs.end();
      request->send(200, "text/plain", "Calibracao CAL (" + String(cal) + ") aplicada!");
    } else {
      request->send(400, "text/plain", "Parametro 'valor' faltando");
    }
  });

  server.on("/setCorrecao", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("a_vac") && request->hasParam("b_vac") && request->hasParam("a_iac") && request->hasParam("b_iac")) {
      coefA_VAC = request->getParam("a_vac")->value().toFloat();
      coefB_VAC = request->getParam("b_vac")->value().toFloat();
      coefA_IAC = request->getParam("a_iac")->value().toFloat();
      coefB_IAC = request->getParam("b_iac")->value().toFloat();

      prefs.begin("config", false);
      prefs.putFloat("coefA_VAC", coefA_VAC);
      prefs.putFloat("coefB_VAC", coefB_VAC);
      prefs.putFloat("coefA_IAC", coefA_IAC);
      prefs.putFloat("coefB_IAC", coefB_IAC);
      prefs.end();

      request->send(200, "text/plain", "Fatores de correcao salvos com sucesso!");
    } else {
      request->send(400, "text/plain", "Parametros faltando");
    }
  });

  server.on("/folder", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "";
    File root = SPIFFS.open("/");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        String fileName = String(file.name());
        if (fileName.startsWith("/")) {
          fileName = fileName.substring(1);
        }
        html += "<div class='file'>";
        html += "<a href='http://green.local/download?file=" + fileName + "' target='_blank'>" + fileName + " (" + String(file.size()) + " B)</a>";
        html += "</div>";
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }
    if (html == "") html = "<p style='font-size:12px; color:#aaa;'>Nenhum arquivo encontrado.</p>";
    request->send(200, "text/html", html);
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("file")) {
      String path = request->getParam("file")->value();
      if (!path.startsWith("/")) path = "/" + path;

      if (SPIFFS.exists(path)) {
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, path, "application/octet-stream");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
      } else {
        request->send(404, "text/plain", "Arquivo nao encontrado");
      }
    } else {
      request->send(400, "text/plain", "Parametro 'file' faltando");
    }
  });

  server.on("/formatar", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoFormatarSPIFFS = true;
    request->send(200, "text/plain", "Formatacao solicitada.");
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoIniciarGravacao = true;
    request->send(200, "text/plain", "Gravacao solicitada");
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoPararGravacao = true;
    request->send(200, "text/plain", "Parada solicitada");
  });

  server.on("/resetConsumo", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoResetConsumo = true;
    request->send(200, "text/plain", "Consumo resetado!");
  });

  server.begin();
  webSocket.begin();

  xTaskCreatePinnedToCore(tarefaMedicao, "MEDICAO", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(tarefaInterface, "INTERFACE", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(tarefaComunicacao, "COMUNICACAO", 6144, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// =======================================================
// TASK 1 - MEDIÇÃO DOS DADOS
// =======================================================
void tarefaMedicao(void* pv) {
  Medicao local;
  uint32_t ultimoTempoEnergia = millis();

  const float FREQUENCIA_REDE = 60.0f;
  const int CICLOS_JANELA = 18;
  const int AMOSTRAS_POR_CICLO = 128;
  const uint32_t INTERVALO_AMOSTRA_US = (uint32_t)((1000000.0f / FREQUENCIA_REDE) / AMOSTRAS_POR_CICLO);
  const uint32_t TOTAL_AMOSTRAS = CICLOS_JANELA * AMOSTRAS_POR_CICLO;

  for (;;) {
    MODO = cal;

    if (pedidoResetConsumo) {
      pedidoResetConsumo = false;
      energiaTotalWh = 0.0f;
    }

    if (MODO == 0 || MODO == 1) {
      local = Medicao();
      local.adcV = analogRead(pinAdcTensao);
      local.adcI = analogRead(pinAdcCorrente);
      if(MODO == 1) {
        local.vInst = (float(local.adcV) - offsetVAC) * coefA_VAC + coefB_VAC;
        local.iInst = (float(local.adcI) - offsetIAC) * coefA_IAC + coefB_IAC;
      }
      local.tempoMs = millis();
      publicarSnapshot(local);
      vTaskDelay(pdMS_TO_TICKS(20));
    } else if (MODO == 2 || MODO == 3) {
      local = Medicao();
      double somaQuadradosV = 0.0;
      double somaQuadradosI = 0.0;
      double somaPotenciaInstantanea = 0.0;
      uint32_t amostrasColetadas = 0;
      uint32_t proximoSample = micros();

      while (amostrasColetadas < TOTAL_AMOSTRAS) {
        if ((int32_t)(micros() - proximoSample) >= 0) {
          proximoSample += INTERVALO_AMOSTRA_US;
          int adcV = analogRead(pinAdcTensao);
          int adcI = analogRead(pinAdcCorrente);

          float vInst = (float(adcV) - offsetVAC) * coefA_VAC + coefB_VAC;
          float iInst = (float(adcI) - offsetIAC) * coefA_IAC + coefB_IAC;

          somaQuadradosV += (double)vInst * (double)vInst;
          somaQuadradosI += (double)iInst * (double)iInst;
          somaPotenciaInstantanea += (double)vInst * (double)iInst;
          amostrasColetadas++;
        }
      }

      if (amostrasColetadas == 0) amostrasColetadas = 1;

      local.vRMS = sqrt(somaQuadradosV / (double)amostrasColetadas);
      local.iRMS = sqrt(somaQuadradosI / (double)amostrasColetadas);
      local.potencia = somaPotenciaInstantanea / (double)amostrasColetadas;

      if (local.potencia < 0.0f && fabs(local.potencia) < 0.05f) local.potencia = 0.0f;
      if (local.iRMS < 0.030f) { local.iRMS = 0.0f; local.potencia = 0.0f; }
      if (local.vRMS < 2.5f) local.vRMS = 0.0f;

      local.potenciaAparente = local.vRMS * local.iRMS;
      if (local.potenciaAparente > 0.0f && fabs(local.potencia) > local.potenciaAparente) {
        local.potencia = (local.potencia >= 0.0f) ? local.potenciaAparente : -local.potenciaAparente;
      }

      local.fatorPotencia = 0.0f;
      if (local.potenciaAparente > 0.01f) {
        local.fatorPotencia = local.potencia / local.potenciaAparente;
        if (local.fatorPotencia > 1.0f) local.fatorPotencia = 1.0f;
        if (local.fatorPotencia < -1.0f) local.fatorPotencia = -1.0f;
      }

      uint32_t agora = millis();
      float dtHoras = (agora - ultimoTempoEnergia) / 3600000.0f;
      energiaTotalWh += local.potencia * dtHoras;
      local.energia = energiaTotalWh;
      local.consumoKWh = energiaTotalWh / 1000.0f;
      local.tempoMs = agora;
      ultimoTempoEnergia = agora;

      publicarSnapshot(local);
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// =======================================================
// TASK 2 - LCD + BOTÕES
// =======================================================
void tarefaInterface(void* pv) {
  bool lastNext = HIGH, lastPrev = HIGH, lastOK = HIGH;
  unsigned long lastDebounceNext = 0, lastDebouncePrev = 0, lastDebounceOK = 0;
  const unsigned long debounceMs = 180;

  Medicao telaCache = lerSnapshot();

  for (;;) {
    bool atualNext = digitalRead(pinBtnNext);
    bool atualPrev = digitalRead(pinBtnPrev);
    bool atualOK = digitalRead(pinBtnOK);
    unsigned long agora = millis();

    if (lastNext == HIGH && atualNext == LOW && (agora - lastDebounceNext) > debounceMs) {
      currentScreen = (currentScreen + 1) % 3;
      lastDebounceNext = agora;
    }
    if (lastPrev == HIGH && atualPrev == LOW && (agora - lastDebouncePrev) > debounceMs) {
      currentScreen = (currentScreen + 2) % 3;
      lastDebouncePrev = agora;
    }
    if (lastOK == HIGH && atualOK == LOW && (agora - lastDebounceOK) > debounceMs) {
      if (currentScreen == 0 || currentScreen == 1) {
        isPaused = !isPaused;
      } else if (currentScreen == 2) {
        if (!isRecording) pedidoIniciarGravacao = true;
        else pedidoPararGravacao = true;
      }
      lastDebounceOK = agora;
    }

    lastNext = atualNext;
    lastPrev = atualPrev;
    lastOK = atualOK;

    if (!isPaused) telaCache = lerSnapshot();
    desenharTela(telaCache);
    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

// =======================================================
// TASK 3 - COMUNICAÇÃO WEB + SPIFFS + WI-FI
// =======================================================
void tarefaComunicacao(void* pv) {
  static unsigned long ultimoRetryWiFi = 0;
  static unsigned long ultimoEnvioWS = 0;

  for (;;) {
    if (pedidoFormatarSPIFFS) {
      pedidoFormatarSPIFFS = false;
      if (logFile) logFile.close();
      File root = SPIFFS.open("/");
      if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
          String nomeCompleto = String(file.name());
          String nomeMinusculo = nomeCompleto;
          nomeMinusculo.toLowerCase();
          if (nomeMinusculo.startsWith("/")) nomeMinusculo = nomeMinusculo.substring(1);

          file.close();
          if (nomeMinusculo != "jarvis.htm" && nomeMinusculo != "jarvis.html" && nomeMinusculo != "index.html") {
            SPIFFS.remove(nomeCompleto);
          }
          file = root.openNextFile();
        }
        root.close();
      }
    }

    if (pedidoIniciarGravacao) { pedidoIniciarGravacao = false; iniciarGravacao(); }
    if (pedidoPararGravacao) { pedidoPararGravacao = false; pararGravacao(); }

    webSocket.loop();

    if (millis() - ultimoEnvioWS >= 500) {
      Medicao d = lerSnapshot();
      String json = "{";
      json += "\"tensao\":" + String(d.vRMS, 2) + ",";
      json += "\"corrente\":" + String(d.iRMS, 2) + ",";
      json += "\"potencia\":" + String(d.potencia, 2) + ",";
      json += "\"potenciaAparente\":" + String(d.potenciaAparente, 2) + ",";
      json += "\"fp\":" + String(d.fatorPotencia, 3) + ",";
      json += "\"consumo\":" + String(d.consumoKWh, 5);
      json += "}";
      webSocket.broadcastTXT(json);
      ultimoEnvioWS = millis();
    }

    if (isRecording && logFile && (millis() - lastLogMillis >= 500)) {
      Medicao d = lerSnapshot();
      unsigned long tempo = millis() - recordingStartMillis;
      logFile.printf("%lu;%.2f;%.2f;%.2f;%.2f;%.3f;%.5f\n", tempo, d.vRMS, d.iRMS, d.potencia, d.potenciaAparente, d.fatorPotencia, d.consumoKWh);
      logFile.flush();
      lastLogMillis = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void iniciarGravacao() {
  if (isRecording) return;
  
  pedidoResetConsumo = true;

  String prefixo = proximaSessao();
  String caminho = "/" + prefixo + "_log.csv";
  logFile = SPIFFS.open(caminho, FILE_WRITE);
  if (!logFile) return;

  logFile.println("tempo_ms;vRMS;iRMS;potencia;potenciaAparente;fp;consumo_kWh");
  logFile.flush();
  recordingStartMillis = millis();
  lastLogMillis = millis();
  isRecording = true;
}

void pararGravacao() {
  if (!isRecording) return;
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  isRecording = false;
}

void desenharTela(const Medicao& d) {
  if (currentScreen != lastScreen) {
    lcd.clear();
    lastScreen = currentScreen;
  }

  if (currentScreen == 0) {
    lcd.setCursor(0, 0); lcd.print("V:");
    if (MODO == 0) lcd.print(d.adcV);
    else { lcd.print(d.vRMS, 2); lcd.print("V"); }

    lcd.setCursor(0, 1); lcd.print("I:");
    if (MODO == 0) lcd.print(d.adcI);
    else { lcd.print(d.iRMS, 2); lcd.print("A"); }
    if (isPaused) lcd.print("*");
  } else if (currentScreen == 1) {
    lcd.setCursor(0, 0); lcd.print("P:"); lcd.print(d.potencia, 1); lcd.print("W");
    lcd.setCursor(0, 1); lcd.print("E:"); lcd.print(d.consumoKWh, 4); lcd.print("kWh");
    if (isPaused) lcd.print("*");
  } else if (currentScreen == 2) {
    lcd.setCursor(0, 0); lcd.print("Gravando: "); lcd.print(isRecording ? "SIM" : "NAO");
    lcd.setCursor(0, 1);
    unsigned long t = isRecording ? (millis() - recordingStartMillis) / 1000 : 0;
    lcd.print("Tempo: "); lcd.print(t); lcd.print("s");
  }
}

String proximaSessao() {
  int i = 0;
  while (true) {
    String nome = "/sessao_" + String(i) + "_log.csv";
    if (!SPIFFS.exists(nome)) return "sessao_" + String(i);
    i++;
  }
}