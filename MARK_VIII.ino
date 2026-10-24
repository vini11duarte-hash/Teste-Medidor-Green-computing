// =======================================================
// MARK IX - Medidor de energia ESP32
// Base: MARK_IIX  +  funções recuperadas do MARK_VI  +  correções
//
// P  = média(v(t) * i(t))   [W]
// S  = Vrms * Irms          [VA]
// FP = P / S
//
// MODOS (variável cal / MODO)
// 0 - ADC bruto (Serial: adcV \t adcI)
// 1 - Valores instantâneos em V e A (Serial: v \t i)
// 2 - RMS/P/S/FP/consumo em texto legível no Serial
// 3 - RMS/P/S/FP em formato tabulado (Serial FK)
// =======================================================

#include <LiquidCrystal.h>
#include <SPIFFS.h>
#include <FS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include <vector>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

Preferences prefs;

// =======================================================
// SELEÇÃO DO MODO
// =======================================================
volatile int MODO = 3;
volatile int cal = 3;
String htmlPadrao = "Jarvis.html";

// =======================================================
// PINOS
// =======================================================
const uint8_t pinAdcTensao = 34;
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

// Rede local (roteador)
const char* ssid = "Fox-trot";
const char* senha = "squidward";

// Ponto de acesso próprio (sempre ativo). Senha em ASCII, sem acento!
const char* ap_ssid = "Medidor do PET";
const char* ap_senha = "PETELETRICA";

const char* NOME_MDNS = "green";  // http://green.local

// =======================================================
// WebSocket
// =======================================================
WebSocketsServer webSocket(81);

// =======================================================
// CALIBRAÇÃO / OFFSET
// (estes valores são usados se nada tiver sido salvo na memória)
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

  float potencia = 0.0;          // P ativa [W]
  float potenciaAparente = 0.0;  // S [VA]
  float fatorPotencia = 0.0;     // FP
  float energia = 0.0;           // Energia acumulada [Wh]
  float consumoKWh = 0.0;        // Energia acumulada [kWh]

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
unsigned long lastFlushMillis = 0;

double energiaTotalWh = 0.0;  // double: não perde resolução com o tempo

File logFile;

const size_t SPIFFS_LIVRE_MINIMO = 16384;  // para a gravação se sobrar menos que isso (bytes)

// =======================================================
// PROTÓTIPOS
// =======================================================
void tarefaMedicao(void* pv);
void tarefaInterface(void* pv);
void tarefaComunicacao(void* pv);

void iniciarGravacao();
void pararGravacao();
void iniciarMDNS();
void formatarSPIFFS();

void desenharTela(const Medicao& d);
void lcdLinha(uint8_t linha, const char* texto, bool asterisco = false);
String proximaSessao();
bool arquivoProtegido(String nome);

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);

  // ---- Lê configurações salvas (se não existir, mantém os valores do topo do código) ----
  prefs.begin("config", true);
  cal = prefs.getInt("cal", 3);
  MODO = cal;
  coefA_VAC = prefs.getFloat("coefA_VAC", coefA_VAC);
  coefB_VAC = prefs.getFloat("coefB_VAC", coefB_VAC);
  coefA_IAC = prefs.getFloat("coefA_IAC", coefA_IAC);
  coefB_IAC = prefs.getFloat("coefB_IAC", coefB_IAC);
  htmlPadrao = prefs.getString("htmlPadrao", "Jarvis.html");
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
  } else {
    File f = SPIFFS.open("/Jarvis.html", "r");
    if (!f) {
      Serial.println("Jarvis.html NAO encontrado no SPIFFS");
    } else {
      Serial.printf("Jarvis.html encontrado! %u bytes\n", (unsigned)f.size());
      f.close();
    }
  }

  // ---- WI-FI: AP sempre ativo + conexão no roteador com reconexão automática ----
  WiFi.persistent(false);
  WiFi.setHostname(NOME_MDNS);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // melhora resposta do site/WebSocket
  WiFi.setAutoReconnect(true);

  if (WiFi.softAP(ap_ssid, ap_senha)) {
    Serial.print("Ponto de acesso ativo: ");
    Serial.print(ap_ssid);
    Serial.print("  IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Falha ao criar o ponto de acesso!");
  }

  WiFi.begin(ssid, senha);
  Serial.print("Conectando no Wi-Fi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Roteador nao encontrado ainda. Continuo tentando em segundo plano.");
    Serial.println("Acesse pelo ponto de acesso: http://192.168.4.1");
  }

  iniciarMDNS();

  // ---- CABEÇALHOS CORS (permite abrir o HTML do PC e acessar o ESP) ----
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Private-Network", "true");

  server.onNotFound([](AsyncWebServerRequest* request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      request->send(404, "text/plain", "Not found");
    }
  });

  // ---- Página principal ----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String path = htmlPadrao.startsWith("/") ? htmlPadrao : "/" + htmlPadrao;
    if (SPIFFS.exists(path)) {
      request->send(SPIFFS, path, "text/html");
    } else if (SPIFFS.exists("/Jarvis.html")) {
      request->send(SPIFFS, "/Jarvis.html", "text/html");
    } else if (SPIFFS.exists("/jarvis.html")) {
      request->send(SPIFFS, "/jarvis.html", "text/html");
    } else if (SPIFFS.exists("/index.html")) {
      request->send(SPIFFS, "/index.html", "text/html");
    } else {
      request->send(200, "text/plain", "Servidor ESP32 Ativo. Nenhum HTML padrao encontrado no SPIFFS.");
    }
  });

  // ---- Upload de arquivos (HTML / CSV) ----
  server.on(
    "/upload", HTTP_POST, [](AsyncWebServerRequest* request) {
      request->send(200, "text/plain", "Upload concluido!");
    },
    [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
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
        Serial.printf("Upload Finalizado: %s, %u B\n", filename.c_str(), (unsigned)(index + len));
      }
    });

  // ---- Configurações atuais ----
  server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{";
    json += "\"htmlPadrao\":\"" + htmlPadrao + "\",";
    json += "\"cal\":" + String((int)cal) + ",";
    json += "\"coefA_VAC\":" + String(coefA_VAC, 9) + ",";
    json += "\"coefB_VAC\":" + String(coefB_VAC, 9) + ",";
    json += "\"coefA_IAC\":" + String(coefA_IAC, 9) + ",";
    json += "\"coefB_IAC\":" + String(coefB_IAC, 9);
    json += "}";
    request->send(200, "application/json", json);
  });

  // ---- Status de rede (campo "wifi" mantido; extras adicionados) ----
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    bool sta = (WiFi.status() == WL_CONNECTED);
    bool wifiOk = sta || (WiFi.softAPgetStationNum() > 0);
    String json = "{";
    json += "\"wifi\":" + String(wifiOk ? "true" : "false") + ",";
    json += "\"sta\":" + String(sta ? "true" : "false") + ",";
    json += "\"ip\":\"" + (sta ? WiFi.localIP().toString() : String("")) + "\",";
    json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"gravando\":" + String(isRecording ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  // ---- Espaço do SPIFFS ----
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
      int v = request->getParam("valor")->value().toInt();
      if (v < 0 || v > 3) {
        request->send(400, "text/plain", "Valor invalido (use 0 a 3)");
        return;
      }
      cal = v;
      MODO = v;
      prefs.begin("config", false);
      prefs.putInt("cal", v);
      prefs.end();
      request->send(200, "text/plain", "Calibracao CAL (" + String(v) + ") aplicada!");
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
      request->send(400, "text/plain", "Parametros faltando (a_vac, b_vac, a_iac, b_iac)");
    }
  });

  // ---- Lista de arquivos (HTML pronto, como antes) ----
  // O link usa o MESMO endereço que o cliente usou para chegar no ESP
  // (green.local, IP do roteador ou 192.168.4.1), então funciona em qualquer caso.
  server.on("/folder", HTTP_GET, [](AsyncWebServerRequest* request) {
    String host = request->host();
    if (host.length() == 0) host = String(NOME_MDNS) + ".local";

    String html = "";
    File root = SPIFFS.open("/");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        String fileName = String(file.name());
        if (fileName.startsWith("/")) fileName = fileName.substring(1);
        html += "<div class='file'>";
        html += "<a href='http://" + host + "/download?file=" + fileName + "' target='_blank'>" + fileName + " (" + String((unsigned)file.size()) + " B)</a>";
        html += "</div>";
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }
    if (html == "") html = "<p style='font-size:12px; color:#aaa;'>Nenhum arquivo encontrado.</p>";
    request->send(200, "text/html", html);
  });

  // ---- Lista de arquivos em JSON (opcional, para usar no HTML no futuro) ----
  server.on("/files", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "[";
    bool primeiro = true;
    File root = SPIFFS.open("/");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        String fileName = String(file.name());
        if (fileName.startsWith("/")) fileName = fileName.substring(1);
        if (!primeiro) json += ",";
        primeiro = false;
        json += "{\"nome\":\"" + fileName + "\",\"tamanho\":" + String((unsigned)file.size()) + "}";
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // ---- Download (o CORS já é adicionado pelos cabeçalhos padrão) ----
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("file")) {
      String path = request->getParam("file")->value();
      if (!path.startsWith("/")) path = "/" + path;

      if (SPIFFS.exists(path)) {
        request->send(SPIFFS, path, "application/octet-stream");
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

  // Medição sozinha no núcleo 1 (busy-wait não atrapalha mais LCD/botões)
  xTaskCreatePinnedToCore(tarefaMedicao, "MEDICAO", 4096, NULL, 3, NULL, 1);
  // LCD + botões no núcleo 0
  xTaskCreatePinnedToCore(tarefaInterface, "INTERFACE", 4096, NULL, 2, NULL, 0);
  // Wi-Fi + SPIFFS + gravação no núcleo 0
  xTaskCreatePinnedToCore(tarefaComunicacao, "COMUNICACAO", 6144, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// =======================================================
// mDNS
// =======================================================
void iniciarMDNS() {
  MDNS.end();
  if (MDNS.begin(NOME_MDNS)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local\n", NOME_MDNS);
  } else {
    Serial.println("Erro ao iniciar mDNS.");
  }
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
      energiaTotalWh = 0.0;
    }

    if (MODO == 0) {
      // ---------------- MODO 0: ADC BRUTO ----------------
      local = Medicao();
      local.adcV = analogRead(pinAdcTensao);
      local.adcI = analogRead(pinAdcCorrente);
      local.energia = (float)energiaTotalWh;
      local.consumoKWh = (float)(energiaTotalWh / 1000.0);
      local.tempoMs = millis();
      publicarSnapshot(local);

      Serial.print(local.adcV);
      Serial.print('\t');
      Serial.println(local.adcI);

      ultimoTempoEnergia = millis();  // evita "salto" de energia ao voltar para modo 2/3
      vTaskDelay(pdMS_TO_TICKS(20));

    } else if (MODO == 1) {
      // ---------------- MODO 1: INSTANTÂNEO ----------------
      local = Medicao();
      local.adcV = analogRead(pinAdcTensao);
      local.adcI = analogRead(pinAdcCorrente);
      local.vInst = (float(local.adcV) - offsetVAC) * coefA_VAC + coefB_VAC;
      local.iInst = (float(local.adcI) - offsetIAC) * coefA_IAC + coefB_IAC;
      local.energia = (float)energiaTotalWh;
      local.consumoKWh = (float)(energiaTotalWh / 1000.0);
      local.tempoMs = millis();
      publicarSnapshot(local);

      Serial.print(local.vInst, 3);
      Serial.print('\t');
      Serial.println(local.iInst, 3);

      ultimoTempoEnergia = millis();
      vTaskDelay(pdMS_TO_TICKS(20));

    } else if (MODO == 2 || MODO == 3) {
      // ---------------- MODO 2/3: RMS + P + S + FP ----------------
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
      if (local.iRMS < 0.030f) {
        local.iRMS = 0.0f;
        local.potencia = 0.0f;
      }
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

      // Reset pedido durante a janela é aplicado AGORA (o log já sai zerado)
      if (pedidoResetConsumo) {
        pedidoResetConsumo = false;
        energiaTotalWh = 0.0;
      }

      uint32_t agora = millis();
      double dtHoras = (double)(agora - ultimoTempoEnergia) / 3600000.0;
      energiaTotalWh += (double)local.potencia * dtHoras;
      local.energia = (float)energiaTotalWh;
      local.consumoKWh = (float)(energiaTotalWh / 1000.0);
      local.tempoMs = agora;
      ultimoTempoEnergia = agora;

      publicarSnapshot(local);

      // ---- SERIAL ----
      if (MODO == 2) {
        Serial.print("V: ");
        Serial.print(local.vRMS, 2);
        Serial.print(" V | I: ");
        Serial.print(local.iRMS, 3);
        Serial.print(" A | P: ");
        Serial.print(local.potencia, 2);
        Serial.print(" W | S: ");
        Serial.print(local.potenciaAparente, 2);
        Serial.print(" VA | FP: ");
        Serial.print(local.fatorPotencia, 3);
        Serial.print(" | Consumo: ");
        Serial.print(local.consumoKWh, 5);
        Serial.println(" kWh");
      }

      if (MODO == 3) {
        // Formato idêntico ao MARK_VI (compatível com o Serial FK)
        Serial.print("t");
        Serial.print('\t');
        Serial.print(micros());
        Serial.print('\t');
        Serial.print("Vac");
        Serial.print('\t');
        Serial.print(local.vRMS, 3);
        Serial.print('\t');
        Serial.print("Iac");
        Serial.print('\t');
        Serial.print(local.iRMS, 3);
        Serial.print('\t');
        Serial.print("P");
        Serial.print('\t');
        Serial.print(local.potencia, 3);
        Serial.print('\t');
        Serial.print("S");
        Serial.print('\t');
        Serial.print(local.potenciaAparente, 3);
        Serial.print('\t');
        Serial.print("FP");
        Serial.print('\t');
        Serial.println(local.fatorPotencia, 4);
      }

      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      // Valor de MODO inesperado: não trava a task
      vTaskDelay(pdMS_TO_TICKS(100));
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
  static bool staEstavaConectado = (WiFi.status() == WL_CONNECTED);

  for (;;) {
    // ---- Formatar (preserva HTMLs) ----
    if (pedidoFormatarSPIFFS) {
      pedidoFormatarSPIFFS = false;
      formatarSPIFFS();
    }

    if (pedidoIniciarGravacao) {
      pedidoIniciarGravacao = false;
      iniciarGravacao();
    }
    if (pedidoPararGravacao) {
      pedidoPararGravacao = false;
      pararGravacao();
    }

    // ---- Reconexão automática com o roteador (AP continua ativo) ----
    bool staAgora = (WiFi.status() == WL_CONNECTED);

    if (staAgora && !staEstavaConectado) {
      Serial.print("Wi-Fi reconectado. IP: ");
      Serial.println(WiFi.localIP());
      iniciarMDNS();
    }
    if (!staAgora && staEstavaConectado) {
      Serial.println("Wi-Fi do roteador caiu.");
    }
    staEstavaConectado = staAgora;

    if (!staAgora && (millis() - ultimoRetryWiFi >= 30000)) {
      ultimoRetryWiFi = millis();
      Serial.println("Tentando reconectar no roteador...");
      WiFi.disconnect(false);
      WiFi.begin(ssid, senha);
    }

    // ---- WebSocket a cada 500 ms ----
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

    // ---- Gravação ----
    if (isRecording && logFile) {
      unsigned long agora = millis();

      if (agora - lastLogMillis >= 500) {
        Medicao d = lerSnapshot();
        unsigned long tempo = agora - recordingStartMillis;
        logFile.printf("%lu;%.2f;%.2f;%.2f;%.2f;%.3f;%.5f\n",
                       tempo, d.vRMS, d.iRMS, d.potencia, d.potenciaAparente, d.fatorPotencia, d.consumoKWh);
        lastLogMillis = agora;
      }

      // Grava de fato na flash só a cada 5 s (menos desgaste e menos travamentos)
      if (agora - lastFlushMillis >= 5000) {
        logFile.flush();
        lastFlushMillis = agora;

        size_t livre = SPIFFS.totalBytes() - SPIFFS.usedBytes();
        if (livre < SPIFFS_LIVRE_MINIMO) {
          Serial.println("[REC] SPIFFS quase cheio. Gravacao encerrada automaticamente.");
          pararGravacao();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =======================================================
// FORMATAR SPIFFS (preserva HTMLs)
// =======================================================
bool arquivoProtegido(String nome) {
  nome.toLowerCase();
  if (nome.startsWith("/")) nome = nome.substring(1);
  return (nome == "jarvis.htm" || nome == "jarvis.html" || nome == "index.html" || nome == "desktop.ini");
}

void formatarSPIFFS() {
  // Encerra a gravação corretamente (antes só fechava o arquivo e deixava isRecording = true)
  if (isRecording) pararGravacao();

  // 1) Junta os nomes primeiro (apagar durante a leitura do diretório pode pular arquivos)
  std::vector<String> nomes;
  File root = SPIFFS.open("/");
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      nomes.push_back(String(file.name()));
      file.close();
      file = root.openNextFile();
    }
    root.close();
  } else {
    Serial.println("[SPIFFS] Falha ao abrir a raiz.");
    return;
  }

  // 2) Apaga o que não é protegido
  int apagados = 0;
  for (size_t i = 0; i < nomes.size(); i++) {
    if (arquivoProtegido(nomes[i])) {
      Serial.printf("[SPIFFS] Protegido: %s\n", nomes[i].c_str());
      continue;
    }
    String caminho = nomes[i].startsWith("/") ? nomes[i] : "/" + nomes[i];
    if (SPIFFS.remove(caminho)) {
      apagados++;
      Serial.printf("[SPIFFS] Removido: %s\n", caminho.c_str());
    } else {
      Serial.printf("[SPIFFS] Falha ao remover: %s\n", caminho.c_str());
    }
  }
  Serial.printf("[SPIFFS] Limpeza concluida. %d arquivo(s) removido(s).\n", apagados);
}

// =======================================================
// GRAVAÇÃO
// =======================================================
void iniciarGravacao() {
  if (isRecording) return;

  String prefixo = proximaSessao();
  String caminho = "/" + prefixo + "_log.csv";
  logFile = SPIFFS.open(caminho, FILE_WRITE);
  if (!logFile) {
    Serial.println("[REC] Falha ao criar arquivo.");
    return;
  }

  logFile.println("tempo_ms;vRMS;iRMS;potencia;potenciaAparente;fp;consumo_kWh");
  logFile.flush();

  // O consumo só zera se o arquivo foi criado com sucesso
  pedidoResetConsumo = true;

  recordingStartMillis = millis();
  lastLogMillis = millis();
  lastFlushMillis = millis();
  isRecording = true;
  Serial.printf("[REC] Gravacao iniciada em: %s\n", caminho.c_str());
}

void pararGravacao() {
  if (!isRecording) return;
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  isRecording = false;
  Serial.println("[REC] Gravacao parada.");
}

// =======================================================
// LCD (linhas sempre com 16 caracteres: sem "lixo" na tela)
// =======================================================
void lcdLinha(uint8_t linha, const char* texto, bool asterisco) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", texto);
  if (asterisco) buf[15] = '*';
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

void desenharTela(const Medicao& d) {
  if (currentScreen != lastScreen) {
    lcd.clear();
    lastScreen = currentScreen;
  }

  char t[40];
  int modo = MODO;

  if (currentScreen == 0) {
    if (modo == 0) snprintf(t, sizeof(t), "V:%ld", (long)d.adcV);
    else if (modo == 1) snprintf(t, sizeof(t), "V:%.1fV", d.vInst);
    else snprintf(t, sizeof(t), "V:%.2fV", d.vRMS);
    lcdLinha(0, t);

    if (modo == 0) snprintf(t, sizeof(t), "I:%ld", (long)d.adcI);
    else if (modo == 1) snprintf(t, sizeof(t), "I:%.3fA", d.iInst);
    else snprintf(t, sizeof(t), "I:%.2fA", d.iRMS);
    lcdLinha(1, t, isPaused);

  } else if (currentScreen == 1) {
    snprintf(t, sizeof(t), "P:%.1fW", d.potencia);
    lcdLinha(0, t);

    snprintf(t, sizeof(t), "E:%.4fkWh", d.consumoKWh);
    lcdLinha(1, t, isPaused);

  } else if (currentScreen == 2) {
    snprintf(t, sizeof(t), "Gravando: %s", isRecording ? "SIM" : "NAO");
    lcdLinha(0, t);

    unsigned long seg = isRecording ? (millis() - recordingStartMillis) / 1000 : 0;
    snprintf(t, sizeof(t), "Tempo: %lus", seg);
    lcdLinha(1, t);
  }
}

// =======================================================
// NOME DA PRÓXIMA SESSÃO DE LOG
// =======================================================
String proximaSessao() {
  int i = 0;
  while (true) {
    String nome = "/sessao_" + String(i) + "_log.csv";
    if (!SPIFFS.exists(nome)) return "sessao_" + String(i);
    i++;
  }
}
