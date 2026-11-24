# Jogo de Tempo de Reação — ESP32 + MQTT + Node-RED + SQLite

Projeto final da disciplina de Microcontroladores — UTFPR

**Integrantes:** (preencher)

## Descrição

Jogo de tempo de reação com ESP32. Depois de um atraso aleatório de 1 a 5 s, o LED verde acende e o buzzer apita; o jogador aperta o botão o mais rápido possível. O tempo em milissegundos aparece no OLED e é enviado por MQTT ao Node-RED, que grava no SQLite e mostra no dashboard.

- **Modo solo:** um jogador.
- **Modo duelo:** dois jogadores; vence o menor tempo.
- **Queima de largada:** apertar antes do LED verde acender gera penalidade e também é registrado.

## Como os critérios foram atendidos

| # | Critério | Implementação |
|---|---|---|
| 1 | Dispositivo de entrada e saída | 3 botões; OLED SSD1306, 2 LEDs e buzzer |
| 2 | Periférico dedicado / interrupção | Interrupção de GPIO nos botões, timer de hardware (`esp_timer`) para o atraso aleatório, PWM (LEDC) no buzzer |
| 3 | MQTT via Node-RED | ESP32 publica resultados e status; recebe comandos do dashboard |
| 4 | SQL via Node-RED (SQLite) | Tabela `partidas`; ranking e estatísticas por consulta |
| 5 | Simulação via Wokwi | Link abaixo |
| 6 | Protótipo em bancada | Fotos em `docs/` |
| 7 | Dashboard via Node-RED | Botão Iniciar, nomes, modo, último tempo, gráfico, ranking |
| 8 | Documentação no GitHub | Este repositório |
| 9 | Vídeo de apresentação | Link abaixo |

## Hardware e ligações

| Componente | Pino | ESP32 |
|---|---|---|
| OLED SSD1306 | VCC / GND / SDA / SCL | 3V3 / GND / GPIO 21 / GPIO 22 |
| Botão Jogador 1 | — | GPIO 25 e GND |
| Botão Jogador 2 | — | GPIO 26 e GND |
| Botão Iniciar | — | GPIO 27 e GND |
| LED verde (+ 220 Ω) | anodo | GPIO 18 |
| LED vermelho (+ 220 Ω) | anodo | GPIO 19 |
| Buzzer passivo | + | GPIO 23 |

Botões usam o pull-up interno (`INPUT_PULLUP`), sem resistor externo.

## Simulação no Wokwi

Link: (preencher)

Arquivos em `wokwi/` (`diagram.json` e `libraries.txt`).

## MQTT

Broker: `broker.hivemq.com:1883`

| Tópico | Direção | Exemplo |
|---|---|---|
| `utfpr/reacao/grupo1/resultado` | ESP32 → Node-RED | `{"jogador":"Ana","tempo_ms":243,"modo":"solo","queimou":false}` |
| `utfpr/reacao/grupo1/status` | ESP32 → Node-RED | `aguardando`, `preparando`, `vai`, `fim` |
| `utfpr/reacao/grupo1/comando` | Node-RED → ESP32 | `{"acao":"iniciar"}`, `{"modo":"duelo"}` |

## Banco de dados

Esquema em `database/schema.sql`.

## Dashboard

(print do dashboard)

## Como executar

1. Copie `firmware/jogo_reacao/secrets.example.h` para `secrets.h` e coloque a sua rede Wi-Fi.
2. Abra `firmware/jogo_reacao/jogo_reacao.ino` na Arduino IDE (core ESP32 3.x) e instale as bibliotecas de `wokwi/libraries.txt`.
3. Grave no ESP32.
4. No Node-RED, instale `node-red-node-sqlite` e `@flowfuse/node-red-dashboard` e importe `node-red/flows.json`.
5. Abra `http://127.0.0.1:1880/dashboard`.

## Vídeo de apresentação

Link: (preencher)
