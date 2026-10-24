
#include <LiquidCrystal.h>
#include <SPIFFS.h>
#include <FS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include <WebSocketsServer.h>
#include <Preferences.h> // Biblioteca nativa para salvar dados na memória permanente
Preferences prefs;



// =======================================================
// SELEÇÃO DO MODO
// 0 - Transmite valores brutos do ADC
// 1 - Transmite valores instantâneos em V e A
// 2 - Transmite valores RMS em V e A
// 3 - Transmite valores RMS em V e A para Serial FK
// =======================================================
int MODO = 3; // Em vez de #define MODO 3
// Variáveis de Calibração e Correção
int cal=3;
String htmlPadrao = "Jarvis.htm"; // Nome padrão do seu HTML principal
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

// =======================================================
// WebSocket
// =======================================================
WebSocketsServer webSocket(81);
// =======================================================
// CALIBRAÇÃO / OFFSET
// Ajuste depois da sua calibração.
// Se quiser, pode trocar esses valores por outros.
float offsetVAC = 1775.94;
float offsetIAC = 1815;

// Coeficientes de conversão dinâmicos(ajuste depois)
float coefA_VAC = 1.0;
float coefB_VAC = 0.0;
float coefA_IAC = 1.0;
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
  float energia = 0.0;

  uint32_t tempoMs = 0;
};

// =======================================================
// DADOS COMPARTILHADOS ENTRE TASKS
// =======================================================
Medicao dadosCompartilhados;
portMUX_TYPE dadosMux = portMUX_INITIALIZER_UNLOCKED;

// Snapshot rápido: copia o struct inteiro de forma segura
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

int currentScreen = 0;
int lastScreen = -1;

unsigned long recordingStartMillis = 0;
unsigned long lastLogMillis = 0;
float energiaTotal = 0.0;

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
String dadosParaTexto(const Medicao& d);

String paginaPrincipal();
String listFiles(String folderPath);
String proximaSessao();

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);

// Abre o espaço de memória "config" para ler os parâmetros salvos
// Abre o espaço de memória "config" para ler os parâmetros salvos
prefs.begin("config", true);
cal = prefs.getInt("cal", 3); // Ajustado para carregar na variavel 'cal' usada nas rotas
MODO = cal;                   // Garante sincronismo inicial
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

  //TESTE HTML
  File f = SPIFFS.open("/Jarvis.html", "r");

  if (!f) {
    Serial.println("Jarvis.html NÃO encontrado");
  } else {
    Serial.printf("Jarvis.html encontrado! %u bytes\n", f.size());
    f.close();
  }


  WiFi.begin(ssid, senha);
  Serial.print("Conectando no Wi-Fi");

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi nao conectado. O sistema continua offline.");
  }





  // Rota para ler o arquivo HTML dinâmico definido como padrão
server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String caminho =htmlPadrao;
    
    // 1ª Prioridade: Tenta carregar o HTML que está definido na variável dinâmica
    if (SPIFFS.exists(caminho)) {
        request->send(SPIFFS, caminho, "text/html");
    }  
    // 2ª Prioridade: Se o dinâmico não existir, tenta o "Jarvis.html" físico padrão como plano de fundo
    else if (SPIFFS.exists("/Jarvis.html")) {
        request->send(SPIFFS, "/Jarvis.html", "text/html");
    } 
    // 3ª Opção: Se nenhum dos dois existir na memória, exibe o aviso
    else {
        request->send(200, "text/plain", "Arquivo padrao nao encontrado. Use o painel para fazer upload.");
    }
});

// Envia as configurações atuais para o site preencher os campos ao abrir o painel
server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"htmlPadrao\":\"" + htmlPadrao + "\",";
    json += "\"cal\":" + String(cal) + ",";
    json += "\"coefA_VAC\":" + String(coefA_VAC, 5) + ",";
    json += "\"coefB_VAC\":" + String(coefB_VAC, 5) + ",";
    json += "\"coefA_IAC\":" + String(coefA_IAC, 5) + ",";
    json += "\"coefB_IAC\":" + String(coefB_IAC, 5);
    json += "}";

    request->send(200, "application/json", json);
});



// Modifica o HTML padrão e salva permanentemente
server.on("/setHtmlPadrao", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("nome")) {
        htmlPadrao = request->getParam("nome")->value();
        prefs.begin("config", false);
        prefs.putString("htmlPadrao", htmlPadrao);
        prefs.end();
        request->send(200, "text/plain", "HTML padrao alterado para " + htmlPadrao);
    } else { request->send(400, "text/plain", "Parametro faltando"); }
});

// Modifica o valor de CAL e re-executa a rotina de setup dos sensores
server.on("/setCal", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("valor")) {
        cal = request->getParam("valor")->value().toInt();
        
        prefs.begin("config", false);
        prefs.putInt("cal", cal);
        prefs.end();
        

        
        request->send(200, "text/plain", "Calibracao CAL aplicada e Setup de Sensores reexecutado!");
    } else { request->send(400, "text/plain", "Parametro faltando"); }
});

// Altera os fatores de calibração de V e I em tempo real
server.on("/setCorrecao", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("a_vac") && request->hasParam("b_vac") && 
        request->hasParam("a_iac") && request->hasParam("b_iac")) {
        
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
        
        request->send(200, "text/plain", "Fatores de calibracao de Tensao e Corrente atualizados!");
    } else { 
        request->send(400, "text/plain", "Parametros faltando (Requer: a_vac, b_vac, a_iac, b_iac)"); 
    }
});

// ROTA DE UPLOAD REMOTO DE ARQUIVOS (Para receber novos HTMLs no SPIFFS)
server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Arquivo gravado com sucesso no SPIFFS!");
}, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if(!index){
        // Abre o arquivo no início do upload (sobrescreve se já existir)
        if(!filename.startsWith("/")) filename = "/" + filename;
        request->_tempFile = SPIFFS.open(filename, "w");
    }
    if(request->_tempFile){
        request->_tempFile.write(data, len);
    }
    if(final){
        // Fecha o arquivo ao finalizar o envio
        request->_tempFile.close();
    }
});

  // Lista arquivos
  server.on("/folder", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("name")) {
      String folderName = request->getParam("name")->value();
      request->send(200, "text/html", listFiles(folderName));
    } else {
      request->send(400, "text/plain", "Parametro 'name' faltando");
    }
  });

  // Download
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("file")) {
      String path = request->getParam("file")->value();

      if (!path.startsWith("/"))
        path = "/" + path;

      if (SPIFFS.exists(path))
        request->send(SPIFFS, path, "application/octet-stream");
      else
        request->send(404, "text/plain", "Arquivo nao encontrado");
    } else {
      request->send(400, "text/plain", "Parametro 'file' faltando");
    }
  });

  // Formatar
  server.on("/formatar", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoFormatarSPIFFS = true;
    request->send(200, "text/plain", "Dispositivo sera reiniciado.");
  });

  // Iniciar gravação
  server.on("/start", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoIniciarGravacao = true;
    request->send(200, "text/plain", "Gravacao solicitada");
  });

  // Parar gravação
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest* request) {
    pedidoPararGravacao = true;
    request->send(200, "text/plain", "Parada solicitada");
  });

  server.begin();
  webSocket.begin();

  // Task 1 - Medição
  xTaskCreatePinnedToCore(
    tarefaMedicao,
    "MEDICAO",
    4096,
    NULL,
    3,
    NULL,
    1);

  // Task 2 - LCD + botões
  xTaskCreatePinnedToCore(
    tarefaInterface,
    "INTERFACE",
    4096,
    NULL,
    2,
    NULL,
    1);

  // Task 3 - Wi-Fi + SPIFFS + gravação
  xTaskCreatePinnedToCore(
    tarefaComunicacao,
    "COMUNICACAO",
    6144,
    NULL,
    1,
    NULL,
    0);
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

    for (;;) {
        // Atualiza o MODO a cada ciclo para pegar as mudanças feitas pelo painel web
        MODO = cal; 

        if (MODO == 0) { // <-- Sem '#' e usando chaves '{'
            local.adcV = analogRead(pinAdcTensao);
            local.adcI = analogRead(pinAdcCorrente);
            local.tempoMs = millis();

            publicarSnapshot(local);

            Serial.print(local.adcV);
            Serial.print('\t');
            Serial.println(local.adcI);
            
            //==================APAGAR DEPOIS ====================================
            Serial.print("CAL: ");
            Serial.print(cal);
            Serial.print(" | A e B: ");
            Serial.print(coefA_VAC);
            Serial.print(" / ");
            Serial.println(coefA_IAC);

            vTaskDelay(pdMS_TO_TICKS(20));
        } 
        else if (MODO == 1) { // <-- Condicional de tempo de execução normal
            local.adcV = analogRead(pinAdcTensao);
            local.adcI = analogRead(pinAdcCorrente);

            local.vInst = (float(local.adcV) - offsetVAC) * coefA_VAC + coefB_VAC;
            local.iInst = (float(local.adcI) - offsetIAC) * coefA_IAC + coefB_IAC;

            local.tempoMs = millis();

            publicarSnapshot(local);

            Serial.print(local.vInst, 3);
            Serial.print('\t');
            Serial.println(local.iInst, 3);
            
            //==================APAGAR DEPOIS ====================================
            Serial.print("CAL: ");
            Serial.print(cal);
            Serial.print(" | A e B: ");
            Serial.print(coefA_VAC);
            Serial.print(" / ");
            Serial.println(coefA_IAC);

            vTaskDelay(pdMS_TO_TICKS(20));
        } 
        else if (MODO == 2 || MODO == 3) {

            const uint32_t duracaoAmostragemMs = 100;  // janela simples para RMS
            uint32_t inicio = millis();

            double somaQuadradosV = 0.0;
            double somaQuadradosI = 0.0;
            uint32_t qtd = 0;

            while ((millis() - inicio) < duracaoAmostragemMs) {
                int32_t amostraV = analogRead(pinAdcTensao) - offsetVAC;
                int32_t amostraI = analogRead(pinAdcCorrente) - offsetIAC;

                somaQuadradosV += double(amostraV) * double(amostraV);
                somaQuadradosI += double(amostraI) * double(amostraI);

                qtd++;

                if ((qtd & 0x3F) == 0) {
                    taskYIELD();
                }
            }

            if (qtd == 0) qtd = 1;

            float rmsADCV = sqrt(somaQuadradosV / float(qtd));
            float rmsADCI = sqrt(somaQuadradosI / float(qtd));

            local.vRMS = rmsADCV * coefA_VAC + coefB_VAC;
            local.iRMS = rmsADCI * coefA_IAC + coefB_IAC;

            if (local.vRMS < 2.5) local.vRMS = 0.0;
            if (local.iRMS < 0.10) local.iRMS = 0.0;

            local.potencia = local.vRMS * local.iRMS;

            uint32_t agora = millis();
            float dtHoras = (agora - ultimoTempoEnergia) / 3600000.0f;
            energiaTotal += local.potencia * dtHoras;
            local.energia = energiaTotal;
            local.tempoMs = agora;

            ultimoTempoEnergia = agora;

            publicarSnapshot(local);

            if (MODO == 2) {
                Serial.print(local.vRMS, 3);
                Serial.print('\t');
                Serial.println(local.iRMS, 3);
                //==================APAGAR DEPOIS ====================================
                Serial.print("CAL: ");
                Serial.print(cal);
                Serial.print(" | A e B: ");
                Serial.print(coefA_VAC);
                Serial.print(" / ");
                Serial.println(coefA_IAC);
            }

            if (MODO == 3) {
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
                Serial.println(local.iRMS, 3);
                //==================APAGAR DEPOIS ====================================
                Serial.print("CAL: ");
                Serial.print(cal);
                Serial.print(" | A e B: ");
                Serial.print(coefA_VAC);
                Serial.print(" / ");
                Serial.println(coefA_IAC);
            }

            vTaskDelay(pdMS_TO_TICKS(5));
        } // Fecha o else if (MODO == 2 || MODO == 0)
    } // Fecha o for(;;)
} // Fecha a função

// =======================================================
// TASK 2 - LCD + BOTÕES
// =======================================================
void tarefaInterface(void* pv) {
  bool lastNext = HIGH;
  bool lastPrev = HIGH;
  bool lastOK = HIGH;

  unsigned long lastDebounceNext = 0;
  unsigned long lastDebouncePrev = 0;
  unsigned long lastDebounceOK = 0;
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

    if (!isPaused) {
      telaCache = lerSnapshot();
    }

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
    //==================================================
    // FORMATAÇÃO DO SPIFFS
    //==================================================
    if (pedidoFormatarSPIFFS) {
      pedidoFormatarSPIFFS = false;

if (logFile) {
    logFile.close();
}

// Abre o diretório raiz do SPIFFS
File root = SPIFFS.open("/");
if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
        String nomeCompleto = String(file.name());
        
        // Converte para minúsculo para evitar problemas com maiúsculas/minúsculas
        String nomeMinusculo = nomeCompleto;
        nomeMinusculo.toLowerCase(); 

        // Remove a barra "/" do início do nome (se existir) para comparar apenas o nome puro do arquivo
        String nomeSemBarra = nomeMinusculo;
        if (nomeSemBarra.startsWith("/")) {
            nomeSemBarra = nomeSemBarra.substring(1);
        }

        file.close(); // Fecha a referência do arquivo antes de tentar deletá-lo

        // LÓGICA DE PRESERVAÇÃO:
        // Se o arquivo NÃO for um dos listados abaixo, ele será apagado!
        if (nomeSemBarra != "jarvis.htm"  && 
            nomeSemBarra != "jarvis.html" && 
            nomeSemBarra != "index.html"  && 
            nomeSemBarra != "desktop.ini") {
            
            // Tenta deletar o arquivo
            if (SPIFFS.remove(nomeCompleto)) {
                Serial.print("Arquivo removido: ");
                Serial.println(nomeCompleto);
            } else {
                // Tenta uma segunda vez garantindo a barra "/" no início, caso a biblioteca exija
                String caminhoAlternativo = nomeCompleto;
                if (!caminhoAlternativo.startsWith("/")) {
                    caminhoAlternativo = "/" + caminhoAlternativo;
                }
                if (SPIFFS.remove(caminhoAlternativo)) {
                    Serial.print("Arquivo removido (segunda tentativa): ");
                    Serial.println(caminhoAlternativo);
                } else {
                    Serial.print("Falha ao remover o arquivo: ");
                    Serial.println(nomeCompleto);
                }
            }
        } else {
            // Se for um arquivo vital, ele ignora e protege o arquivo
            Serial.print("Arquivo VITAL protegido: ");
            Serial.println(nomeCompleto);
        }
        
        // Avança para o próximo arquivo
        file = root.openNextFile();
    }
    root.close();
} else {
    Serial.println("Falha ao abrir a raiz do SPIFFS.");
}

    }

    //==================================================
    // INICIAR GRAVAÇÃO
    //==================================================
    if (pedidoIniciarGravacao) {
      pedidoIniciarGravacao = false;
      iniciarGravacao();
    }

    //==================================================
    // PARAR GRAVAÇÃO
    //==================================================
    if (pedidoPararGravacao) {
      pedidoPararGravacao = false;
      pararGravacao();
    }

    //==================================================
    // RECONEXÃO WI-FI
    //==================================================
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - ultimoRetryWiFi >= 10000) {
        ultimoRetryWiFi = millis();

        Serial.println("Tentando reconectar WiFi...");

        WiFi.disconnect();
        WiFi.begin(ssid, senha);
      }
    }

    //==================================================
    // WEBSOCKET
    //==================================================
    webSocket.loop();

    if (millis() - ultimoEnvioWS >= 500) {
      Medicao d = lerSnapshot();

      String json = "{";
      json += "\"tensao\":" + String(d.vRMS, 2) + ",";
      json += "\"corrente\":" + String(d.iRMS, 2) + ",";
      json += "\"potencia\":" + String(d.potencia, 2);
      json += "}";

      webSocket.broadcastTXT(json);

      ultimoEnvioWS = millis();
    }

    //==================================================
    // GRAVAÇÃO NO SPIFFS
    //==================================================
    if (isRecording && logFile) {
      if (millis() - lastLogMillis >= 500) {
        Medicao d = lerSnapshot();

        unsigned long tempo = millis() - recordingStartMillis;

        logFile.printf("%lu;%.2f;%.2f;%.2f\n",
                       tempo,
                       d.vRMS,
                       d.iRMS,
                       d.potencia);

        logFile.flush();

        lastLogMillis = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}


// =======================================================
// INICIAR GRAVAÇÃO
// =======================================================
void iniciarGravacao() {
  if (isRecording)
    return;

  String prefixo = proximaSessao();
  String caminho = "/" + prefixo + "_log.csv";

  Serial.print("Gravando em: ");
  Serial.println(caminho);

  logFile = SPIFFS.open(caminho, FILE_WRITE);

  if (!logFile) {
    Serial.println("Falha ao criar arquivo.");
    return;
  }

  logFile.println("tempo_ms;vRMS;iRMS;potencia");
  logFile.flush();

  recordingStartMillis = millis();
  lastLogMillis = millis();

  isRecording = true;

  Serial.println("Gravacao iniciada.");
}

// =======================================================
// PARAR GRAVAÇÃO
// =======================================================
void pararGravacao() {
  if (!isRecording)
    return;

  if (logFile) {
    logFile.flush();
    logFile.close();
  }

  isRecording = false;

  Serial.println("Gravacao encerrada.");
}

// =======================================================
// DESENHO NO LCD
// =======================================================
void desenharTela(const Medicao& d) {
  if (currentScreen != lastScreen) {
    lcd.clear();
    lastScreen = currentScreen;
  }

  lcd.setCursor(0, 0);
  lcd.setCursor(0, 1);

  if (currentScreen == 0) {
    lcd.setCursor(0, 0);
    lcd.print("V:");
    if (MODO == 0) {
      lcd.print(d.adcV);
    } else {
      lcd.print(d.vRMS, 2);
      lcd.print("V");
    }

    lcd.setCursor(0, 1);
    lcd.print("I:");
    if (MODO == 0) {
      lcd.print(d.adcI);
    } else {
      lcd.print(d.iRMS, 2);
      lcd.print("A");
    }

    if (isPaused) {
      lcd.print("*");
    }
  } else if (currentScreen == 1) {
    lcd.setCursor(0, 0);
    lcd.print("P:");
    lcd.print(d.potencia, 1);
    lcd.print("W");

    lcd.setCursor(0, 1);
    lcd.print("E:");
    lcd.print(d.energia, 2);
    lcd.print("Wh");

    if (isPaused) {
      lcd.print("*");
    }
  } else if (currentScreen == 2) {
    lcd.setCursor(0, 0);
    lcd.print("Gravando: ");
    lcd.print(isRecording ? "SIM" : "NAO");

    lcd.setCursor(0, 1);
    unsigned long t = isRecording ? (millis() - recordingStartMillis) / 1000 : 0;
    lcd.print("Tempo: ");
    lcd.print(t);
    lcd.print("s");
  }
}

// =======================================================
// TEXTO PARA WEB
// =======================================================
String dadosParaTexto(const Medicao& d) {
  String s;
#if MODO == 0
  s = "adcV=" + String(d.adcV);
  s += ";adcI=" + String(d.adcI);
#elif MODO == 1
  s = "vInst=" + String(d.vInst, 3);
  s += ";iInst=" + String(d.iInst, 3);
#else
  s = "vRMS=" + String(d.vRMS, 3);
  s += ";iRMS=" + String(d.iRMS, 3);
  s += ";potencia=" + String(d.potencia, 3);
  s += ";energia=" + String(d.energia, 3);
#endif
  return s;
}



String listFiles(String folderPath) {

  String html = "";

  File dir = SPIFFS.open(folderPath);

  if (!dir || !dir.isDirectory()) {
    return "<p>Pasta nao encontrada!</p>";
  }

  File file = dir.openNextFile();

  while (file) {

    String filename = file.name();

    html += "<div class='file'>";

    html += "<a href=\"/download?file=" + filename + "\">";

    html += "📄 ";

    html += filename;

    html += "</a>";

    html += "</div>";

    file.close();
    file = dir.openNextFile();
  }

  dir.close();

  if (html == "") {
    html = "<p>Nenhum arquivo encontrado.</p>";
  }

  return html;
}



String proximaSessao() {
  int i = 0;
  while (true) {
    String nome = "/sessao_" + String(i) + "_log.csv";
    if (!SPIFFS.exists(nome)) return "sessao_" + String(i);
    i++;
  }
}


