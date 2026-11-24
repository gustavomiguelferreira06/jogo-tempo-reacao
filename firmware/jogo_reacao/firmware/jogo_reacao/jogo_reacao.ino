/*
  Jogo de Tempo de Reacao - ESP32  (versao 3)
  Projeto final de Microcontroladores - UTFPR
  Requer: core ESP32 Arduino 3.x, Adafruit SSD1306, PubSubClient, ArduinoJson 7
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_timer.h"
#include "secrets.h"   // WIFI_SSID e WIFI_PASS (copie de secrets.example.h)

#define VERSAO "3"

// ---------- Pinos ----------
#define PIN_BTN_J1     25
#define PIN_BTN_J2     26
#define PIN_BTN_START  27
#define PIN_LED_VERDE  18
#define PIN_LED_VERM   19
#define PIN_BUZZER     23

// ---------- MQTT (troquem "grupo1" por um nome unico do grupo) ----------
const char* MQTT_BROKER      = "broker.hivemq.com";
const int   MQTT_PORTA       = 1883;
const char* TOPICO_RESULTADO = "utfpr/reacao/grupo1/resultado";
const char* TOPICO_COMANDO   = "utfpr/reacao/grupo1/comando";
const char* TOPICO_STATUS    = "utfpr/reacao/grupo1/status";

// ---------- Tempos ----------
const int64_t  DEBOUNCE_US       = 50000;    // 50 ms
const int64_t  REACAO_MINIMA_US  = 80000;    // menos de 80 ms = antecipacao (queima)
const int64_t  LIMITE_RESPOSTA   = 3000000;  // 3 s para reagir
const int64_t  DURACAO_APITO     = 150000;   // 150 ms
const uint32_t TELA_RESULTADO_MS = 4000;
const uint32_t WIFI_TIMEOUT_MS   = 20000;    // desiste do Wi-Fi apos 20 s e joga offline
const uint32_t MQTT_RETRY_MS     = 10000;    // tenta reconectar o MQTT a cada 10 s

// ---------- Objetos ----------
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
esp_timer_handle_t timerSinal;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;   // protege variaveis compartilhadas

// ---------- Estado do jogo ----------
enum Estado { AGUARDANDO, PREPARANDO, VAI, RESULTADO };
Estado estado = AGUARDANDO;

// Preenchidas pelas interrupcoes dos botoes (instante do toque ainda nao processado, em us)
volatile int64_t toqueJ1 = 0, toqueJ2 = 0, toqueStart = 0;
volatile int64_t ultimoJ1 = 0, ultimoJ2 = 0, ultimoStart = 0;

// Preenchidas pelo timer do sinal
volatile int64_t tSinal = 0;
volatile bool sinalDisparado = false;

// Usadas apenas no loop
int64_t inicioSinal = 0;          // instante em que o LED verde acendeu (us)
int64_t tJ1 = 0, tJ2 = 0;         // toque valido de cada jogador (us)
bool pedidoStart = false;         // pedido de inicio vindo do MQTT
String modo   = "solo";           // "solo" ou "duelo"
String nomeJ1 = "Jogador 1";
String nomeJ2 = "Jogador 2";
bool apitoLigado = false;
unsigned long inicioTelaResultado = 0;
unsigned long ultimaTentativaMQTT = 0;

// ================= INTERRUPCOES =================
// Rapidas: so guardam o instante do toque. Quem decide o que fazer e o loop.

void IRAM_ATTR isrJogador1() {
  int64_t agora = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&mux);
  if (agora - ultimoJ1 >= DEBOUNCE_US) toqueJ1 = agora;   // debounce
  ultimoJ1 = agora;
  portEXIT_CRITICAL_ISR(&mux);
}

void IRAM_ATTR isrJogador2() {
  int64_t agora = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&mux);
  if (agora - ultimoJ2 >= DEBOUNCE_US) toqueJ2 = agora;
  ultimoJ2 = agora;
  portEXIT_CRITICAL_ISR(&mux);
}

void IRAM_ATTR isrStart() {
  int64_t agora = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&mux);
  if (agora - ultimoStart >= DEBOUNCE_US) toqueStart = agora;
  ultimoStart = agora;
  portEXIT_CRITICAL_ISR(&mux);
}

// Chamada pelo timer de hardware quando o atraso aleatorio termina
void aoDispararSinal(void* arg) {
  int64_t agora = esp_timer_get_time();
  digitalWrite(PIN_LED_VERM, LOW);
  digitalWrite(PIN_LED_VERDE, HIGH);
  portENTER_CRITICAL(&mux);
  tSinal = agora;
  sinalDisparado = true;
  portEXIT_CRITICAL(&mux);
}

// ================= LEITURA SEGURA DOS EVENTOS =================

// Devolve o instante do toque se ele for real (botao ainda apertado); senao, 0.
int64_t lerToque(volatile int64_t& toque, int pino) {
  portENTER_CRITICAL(&mux);
  int64_t t = toque;
  toque = 0;
  portEXIT_CRITICAL(&mux);
  if (t == 0) return 0;
  if (digitalRead(pino) != LOW) return 0;   // pulso falso: botao nao esta apertado
  return t;
}

bool lerSinal(int64_t& instante) {
  portENTER_CRITICAL(&mux);
  bool disparou = sinalDisparado;
  instante = tSinal;
  portEXIT_CRITICAL(&mux);
  return disparou;
}

// ================= SAIDAS =================

// topo: texto pequeno (pode ter \n); grande: texto em destaque
void mostrar(const String& topo, const String& grande, uint8_t tamanho = 2) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(topo);
  oled.setTextSize(tamanho);
  oled.setCursor(0, 36);
  oled.println(grande);
  oled.display();
}

void ligarApito(uint32_t freq) { ledcWriteTone(PIN_BUZZER, freq); apitoLigado = true; }
void pararApito() { if (apitoLigado) { ledcWriteTone(PIN_BUZZER, 0); apitoLigado = false; } }

void publicarStatus(const char* s) {
  if (mqtt.connected()) mqtt.publish(TOPICO_STATUS, s);
}

void publicarResultado(const String& jogador, long tempoMs, bool queimou) {
  JsonDocument doc;
  doc["jogador"] = jogador;
  if (queimou) doc["tempo_ms"] = nullptr;
  else         doc["tempo_ms"] = tempoMs;
  doc["modo"] = modo;
  doc["queimou"] = queimou;
  char buffer[200];
  serializeJson(doc, buffer, sizeof(buffer));
  Serial.println(buffer);
  if (mqtt.connected()) mqtt.publish(TOPICO_RESULTADO, buffer);
  else Serial.println("(offline: resultado nao enviado)");
}

void telaInicial() {
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERM, LOW);
  pararApito();
  String rede = mqtt.connected() ? "MQTT: conectado" : "MQTT: offline";
  mostrar("Modo: " + modo + "\n" + rede + "\nAperte INICIAR", "Pronto", 2);
}

void irParaResultado() {
  estado = RESULTADO;
  inicioTelaResultado = millis();
  publicarStatus("fim");
}

// ================= LOGICA DO JOGO =================

void iniciarRodada() {
  portENTER_CRITICAL(&mux);
  toqueJ1 = 0; toqueJ2 = 0; toqueStart = 0;
  tSinal = 0; sinalDisparado = false;
  portEXIT_CRITICAL(&mux);
  tJ1 = 0; tJ2 = 0; inicioSinal = 0;

  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERM, HIGH);
  mostrar("Prepare-se...", "ESPERE", 2);

  uint32_t atrasoMs = 1000 + esp_random() % 4000;   // 1 a 5 s
  esp_timer_start_once(timerSinal, (uint64_t)atrasoMs * 1000);
  estado = PREPARANDO;
  publicarStatus("preparando");
}

void queimaDeLargada(int jogador) {
  esp_timer_stop(timerSinal);
  String nome = (jogador == 1) ? nomeJ1 : nomeJ2;
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERM, HIGH);
  mostrar("Queima de largada!", nome, 2);
  ligarApito(300);
  delay(400);
  pararApito();
  Serial.printf("Queima de largada: %s\n", nome.c_str());
  publicarResultado(nome, 0, true);
  irParaResultado();
}

void finalizarRodada() {
  pararApito();
  long r1 = tJ1 ? (long)((tJ1 - inicioSinal) / 1000) : -1;
  long r2 = tJ2 ? (long)((tJ2 - inicioSinal) / 1000) : -1;

  if (modo == "solo") {
    Serial.printf("Reacao: %s = %ld ms\n", nomeJ1.c_str(), r1);
    mostrar(nomeJ1, String(r1) + " ms", 2);
    publicarResultado(nomeJ1, r1, false);
  } else {
    Serial.printf("Reacao: J1 = %ld ms | J2 = %ld ms\n", r1, r2);
    String vencedor = (r2 < 0 || (r1 >= 0 && r1 <= r2)) ? nomeJ1 : nomeJ2;
    String placar = String("J1: ") + (r1 >= 0 ? String(r1) + " ms" : String("--"))
                  + "\nJ2: " + (r2 >= 0 ? String(r2) + " ms" : String("--"))
                  + "\nVencedor:";
    mostrar(placar, vencedor, 2);
    if (r1 >= 0) publicarResultado(nomeJ1, r1, false);
    if (r2 >= 0) publicarResultado(nomeJ2, r2, false);
  }
  irParaResultado();
}

// ================= REDE =================

void aoReceberMQTT(char* topico, byte* payload, unsigned int tamanho) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, tamanho)) return;   // ignora JSON invalido
  if (doc["jogador1"].is<const char*>()) nomeJ1 = doc["jogador1"].as<String>();
  if (doc["jogador2"].is<const char*>()) nomeJ2 = doc["jogador2"].as<String>();
  if (doc["modo"] == "solo" || doc["modo"] == "duelo") modo = doc["modo"].as<String>();
  if (doc["acao"] == "iniciar" && estado == AGUARDANDO) pedidoStart = true;
  if (estado == AGUARDANDO && !pedidoStart) telaInicial();
}

void conectarWiFi() {
  mostrar("Conectando Wi-Fi...", "", 2);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nIP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nSem Wi-Fi: jogando offline (tenta de novo sozinho)");
  }
}

// Uma tentativa so, sem travar o jogo
void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  String id = "esp32-reacao-" + String((uint32_t)esp_random(), HEX);
  if (mqtt.connect(id.c_str())) {
    mqtt.subscribe(TOPICO_COMANDO);
    Serial.println("MQTT conectado");
  } else {
    Serial.printf("Falha MQTT, rc=%d\n", mqtt.state());
  }
}

// ================= SETUP E LOOP =================

void setup() {
  Serial.begin(115200);
  Serial.println("\nJogo de Reacao - versao " VERSAO);

  pinMode(PIN_BTN_J1, INPUT_PULLUP);
  pinMode(PIN_BTN_J2, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_VERM, OUTPUT);

  // PWM (LEDC) no buzzer, resolucao de 8 bits
  ledcAttach(PIN_BUZZER, 2000, 8);
  ledcWriteTone(PIN_BUZZER, 0);

  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED nao encontrado");

  // Interrupcoes de GPIO
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_J1), isrJogador1, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_J2), isrJogador2, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_START), isrStart, FALLING);

  // Timer de hardware para o atraso aleatorio
  esp_timer_create_args_t args = {};
  args.callback = &aoDispararSinal;
  args.name = "sinal";
  esp_timer_create(&args, &timerSinal);

  conectarWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(aoReceberMQTT);
  mqtt.setSocketTimeout(5);
  mostrar("Conectando MQTT...", "", 2);
  conectarMQTT();
  ultimaTentativaMQTT = millis();
  telaInicial();
  publicarStatus("aguardando");
}

void loop() {
  // Rede: mantem o MQTT vivo; se caiu, tenta de novo so entre rodadas
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (estado == AGUARDANDO && millis() - ultimaTentativaMQTT > MQTT_RETRY_MS) {
    ultimaTentativaMQTT = millis();
    conectarMQTT();
    if (mqtt.connected()) {
      telaInicial();
      publicarStatus("aguardando");
    }
  }

  switch (estado) {
    case AGUARDANDO: {
      int64_t start = lerToque(toqueStart, PIN_BTN_START);
      lerToque(toqueJ1, PIN_BTN_J1);   // descarta toques fora de hora
      lerToque(toqueJ2, PIN_BTN_J2);
      if (start || pedidoStart) {
        pedidoStart = false;
        iniciarRodada();
      }
      break;
    }

    case PREPARANDO: {
      int64_t ts;
      if (lerSinal(ts)) {
        // LED verde acendeu: comeca a contar
        inicioSinal = ts;
        estado = VAI;
        ligarApito(2000);
        mostrar("", "VAI!", 3);
        publicarStatus("vai");
        break;
      }
      // Ainda nao acendeu: qualquer toque real e queima de largada
      if (lerToque(toqueJ1, PIN_BTN_J1)) {
        queimaDeLargada(1);
      } else if (modo == "duelo" && lerToque(toqueJ2, PIN_BTN_J2)) {
        queimaDeLargada(2);
      }
      if (modo == "solo") lerToque(toqueJ2, PIN_BTN_J2);   // J2 nao joga no modo solo
      break;
    }

    case VAI: {
      int64_t agora = esp_timer_get_time();
      if (agora - inicioSinal > DURACAO_APITO) pararApito();

      int64_t t1 = lerToque(toqueJ1, PIN_BTN_J1);
      int64_t t2 = lerToque(toqueJ2, PIN_BTN_J2);

      // Toque antes do sinal ou rapido demais (< 80 ms) = queima de largada
      if (t1 && tJ1 == 0) {
        if (t1 - inicioSinal < REACAO_MINIMA_US) { queimaDeLargada(1); break; }
        tJ1 = t1;
      }
      if (modo == "duelo" && t2 && tJ2 == 0) {
        if (t2 - inicioSinal < REACAO_MINIMA_US) { queimaDeLargada(2); break; }
        tJ2 = t2;
      }

      bool j1 = (tJ1 != 0), j2 = (tJ2 != 0);
      bool todos = (modo == "solo") ? j1 : (j1 && j2);
      bool esgotou = (agora - inicioSinal) > LIMITE_RESPOSTA;

      if (todos || (esgotou && (j1 || j2))) {
        finalizarRodada();
      } else if (esgotou) {
        pararApito();
        mostrar("Ninguem apertou", "ESGOTOU", 2);
        irParaResultado();
      }
      break;
    }

    case RESULTADO:
      if (millis() - inicioTelaResultado > TELA_RESULTADO_MS) {
        estado = AGUARDANDO;
        telaInicial();
        publicarStatus("aguardando");
      }
      break;
  }
}
