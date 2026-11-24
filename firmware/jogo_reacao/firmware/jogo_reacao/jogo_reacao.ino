/*
  Jogo de Tempo de Reacao - ESP32
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
const int64_t  LIMITE_RESPOSTA   = 3000000;  // 3 s para reagir
const int64_t  DURACAO_APITO     = 150000;   // 150 ms
const uint32_t TELA_RESULTADO_MS = 4000;

// ---------- Objetos ----------
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
esp_timer_handle_t timerSinal;

// ---------- Estado do jogo ----------
enum Estado { AGUARDANDO, PREPARANDO, VAI, RESULTADO };
volatile Estado estado = AGUARDANDO;

// Variaveis usadas nas interrupcoes: sempre volatile
volatile int64_t tSinal = 0;                 // instante do LED verde (us)
volatile int64_t tJ1 = 0, tJ2 = 0;           // primeiro toque de cada jogador (us)
volatile int64_t ultimoJ1 = 0, ultimoJ2 = 0, ultimoStart = 0;
volatile bool sinalDisparado = false;
volatile bool pedidoStart = false;

String modo   = "solo";      // "solo" ou "duelo"
String nomeJ1 = "Jogador 1";
String nomeJ2 = "Jogador 2";
bool apitoLigado = false;
unsigned long inicioTelaResultado = 0;

// ================= INTERRUPCOES =================
// Rapidas: so guardam o instante do toque.

void IRAM_ATTR isrJogador1() {
  int64_t agora = esp_timer_get_time();
  if (agora - ultimoJ1 < DEBOUNCE_US) return;   // debounce
  ultimoJ1 = agora;
  if ((estado == PREPARANDO || estado == VAI) && tJ1 == 0) tJ1 = agora;
}

void IRAM_ATTR isrJogador2() {
  int64_t agora = esp_timer_get_time();
  if (agora - ultimoJ2 < DEBOUNCE_US) return;
  ultimoJ2 = agora;
  if ((estado == PREPARANDO || estado == VAI) && tJ2 == 0) tJ2 = agora;
}

void IRAM_ATTR isrStart() {
  int64_t agora = esp_timer_get_time();
  if (agora - ultimoStart < DEBOUNCE_US) return;
  ultimoStart = agora;
  if (estado == AGUARDANDO) pedidoStart = true;
}

// Chamada pelo timer quando o atraso aleatorio termina
void aoDispararSinal(void* arg) {
  tSinal = esp_timer_get_time();
  digitalWrite(PIN_LED_VERM, LOW);
  digitalWrite(PIN_LED_VERDE, HIGH);
  sinalDisparado = true;
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

void publicarStatus(const char* s) { mqtt.publish(TOPICO_STATUS, s); }

void publicarResultado(const String& jogador, long tempoMs, bool queimou) {
  JsonDocument doc;
  doc["jogador"] = jogador;
  if (queimou) doc["tempo_ms"] = nullptr;
  else         doc["tempo_ms"] = tempoMs;
  doc["modo"] = modo;
  doc["queimou"] = queimou;
  char buffer[200];
  serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(TOPICO_RESULTADO, buffer);
  Serial.println(buffer);
}

void telaInicial() {
  digitalWrite(PIN_LED_VERDE, LOW);
  digitalWrite(PIN_LED_VERM, LOW);
  pararApito();
  mostrar("Modo: " + modo + "\nAperte INICIAR", "Pronto", 2);
}

void irParaResultado() {
  estado = RESULTADO;
  inicioTelaResultado = millis();
  publicarStatus("fim");
}

// ================= LOGICA DO JOGO =================

void iniciarRodada() {
  tJ1 = 0; tJ2 = 0; tSinal = 0;
  sinalDisparado = false;
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
  publicarResultado(nome, 0, true);
  irParaResultado();
}

void finalizarRodada() {
  pararApito();
  long r1 = tJ1 ? (long)((tJ1 - tSinal) / 1000) : -1;
  long r2 = tJ2 ? (long)((tJ2 - tSinal) / 1000) : -1;

  if (modo == "solo") {
    mostrar(nomeJ1, String(r1) + " ms", 2);
    publicarResultado(nomeJ1, r1, false);
  } else {
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

// ================= MQTT =================

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
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.print("\nIP: ");
  Serial.println(WiFi.localIP());
}

void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  while (!mqtt.connected()) {
    mostrar("Conectando MQTT...", "", 2);
    String id = "esp32-reacao-" + String((uint32_t)esp_random(), HEX);
    if (mqtt.connect(id.c_str())) {
      mqtt.subscribe(TOPICO_COMANDO);
      Serial.println("MQTT conectado");
    } else {
      Serial.printf("Falha MQTT, rc=%d\n", mqtt.state());
      delay(2000);
    }
  }
  if (estado == AGUARDANDO) telaInicial();
}

// ================= SETUP E LOOP =================

void setup() {
  Serial.begin(115200);

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
  conectarMQTT();
  telaInicial();
  publicarStatus("aguardando");
}

void loop() {
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  switch (estado) {
    case AGUARDANDO:
      if (pedidoStart) { pedidoStart = false; iniciarRodada(); }
      break;

    case PREPARANDO: {
      bool sinal = sinalDisparado;
      int64_t ts = tSinal;
      // apertou antes do LED verde = queima de largada
      if (tJ1 != 0 && (!sinal || tJ1 < ts)) queimaDeLargada(1);
      else if (modo == "duelo" && tJ2 != 0 && (!sinal || tJ2 < ts)) queimaDeLargada(2);
      else if (sinal) {
        estado = VAI;
        ligarApito(2000);
        mostrar("", "VAI!", 3);
        publicarStatus("vai");
      }
      break;
    }

    case VAI: {
      int64_t agora = esp_timer_get_time();
      if (agora - tSinal > DURACAO_APITO) pararApito();
      bool j1 = (tJ1 != 0), j2 = (tJ2 != 0);
      bool todos = (modo == "solo") ? j1 : (j1 && j2);
      bool esgotou = (agora - tSinal) > LIMITE_RESPOSTA;
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
