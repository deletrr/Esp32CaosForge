# ⚡ ESP32 CaosForge

> **Gerador de entropia física com sorteio criptográfico verificável, monitoramento estatístico em tempo real e integração com Google Sheets via Node-RED.**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-teal?style=flat-square)](https://www.arduino.cc/)
[![Node-RED](https://img.shields.io/badge/Node--RED-2.x-red?style=flat-square)](https://nodered.org/)
[![License](https://img.shields.io/badge/license-Open%20Source-green?style=flat-square)](#)

---

## 📋 Índice

- [O que é](#-o-que-é)
- [Como funciona](#-como-funciona)
- [Arquitetura do sistema](#-arquitetura-do-sistema)
- [Segurança criptográfica](#-segurança-criptográfica)
- [Monitor de Integridade](#-monitor-de-integridade)
- [WebServer embutido](#-webserver-embutido)
- [Temperatura do die (CPU)](#-temperatura-do-die-cpu)
- [Configuração do ESP32](#-configuração-do-esp32)
- [Configuração do Node-RED](#-configuração-do-node-red)
- [Configuração do Google Sheets](#-configuração-do-google-sheets)
- [Estrutura do repositório](#-estrutura-do-repositório)
- [Dados ao vivo](#-dados-ao-vivo)

---

## 🌀 O que é

O **ESP32 CaosForge** é um sistema de geração de aleatoriedade baseada em entropia física real. Em vez de depender de algoritmos de software (que são pseudo-aleatórios e determinísticos), o projeto explora o comportamento caótico do hardware do ESP32 — ruído térmico do chip, jitter de clock, variações do oscilador interno — para produzir números genuinamente imprevisíveis.

O resultado é um pipeline completo de sorteio auditável:

```
[Entropia Física] → [HMAC-SHA256] → [Sorteio 1–60] → [Node-RED] → [Google Sheets]
```

Cada sorteio é acompanhado de uma assinatura HMAC que permite verificação criptográfica independente — qualquer um pode confirmar que o resultado não foi manipulado.

---

## ⚙️ Como funciona

O ESP32 possui dois núcleos de processamento independentes, e o CaosForge os usa de forma deliberada:

### Core 0 — A Forja de Caos
Roda em loop infinito e é responsável exclusivamente por gerar e acumular entropia. A cada tick, combina três fontes de aleatoriedade via XOR:

```cpp
entropia_viva ^= esp_random()           // RNG de hardware (ruído térmico)
              ^ (uint32_t)micros()       // Jitter do oscilador interno
              ^ (uint32_t)(tan(...))     // Amplificação caótica do estado anterior
```

O resultado é uma variável que muda dezenas de milhares de vezes por segundo, impossível de prever ou reproduzir.

### Core 1 — O Oráculo
A cada 60 segundos, "congela" um instante do caos acumulado e executa o pipeline criptográfico:

1. Captura `entropia_viva` de forma thread-safe (com mutex)
2. Combina com `micros()` e `esp_random()` para a semente final
3. Gera um **HMAC-SHA256** com chave secreta configurável
4. Sorteia 6 números únicos de 1 a 60 sem viés estatístico
5. Envia o resultado via HTTP POST para o Node-RED

### Por que HMAC e não SHA-256 simples?
O HMAC (RFC 2104) mistura a chave secreta com os dados de forma padronizada e resistente a *length extension attacks* — um vetor de ataque que SHA-256 puro está suscetível. Usar HMAC garante que mesmo conhecendo a semente, não é possível calcular o hash sem a chave.

---

## 🏗️ Arquitetura do sistema

```
┌─────────────────────────────────────────────┐
│                  ESP32                       │
│                                             │
│  ┌──────────────┐    ┌─────────────────┐   │
│  │    Core 0    │    │     Core 1      │   │
│  │  "A Forja"   │───▶│  "O Oráculo"   │   │
│  │              │    │                 │   │
│  │  esp_random()│    │  HMAC-SHA256    │   │
│  │  micros()    │    │  Sorteio 1-60   │   │
│  │  tan(caos)   │    │  Temp. do die   │   │
│  └──────────────┘    │  HTTP POST      │   │
│                      │  WebServer :80  │   │
│                      └────────┬────────┘   │
│                               │             │
└───────────────────────────────┼─────────────┘
              ┌─────────────────┴──────────────────┐
              │ JSON (POST)                         │ HTTP (GET)
              ▼                                     ▼
┌─────────────────────────┐          ┌──────────────────────────┐
│        Node-RED         │          │   Browser / qualquer     │
│  /caos → pipeline       │          │   dispositivo na rede    │
│  Monitor de Integridade │          │   http://[IP]/           │
└────────────┬────────────┘          │   http://[IP]/json       │
             │                       └──────────────────────────┘
   ┌─────────┴─────────┐
   ▼                   ▼
┌──────────┐    ┌──────────────┐
│  Dados   │    │   Alertas    │
│  Sheets  │    │   Sheets     │
└──────────┘    └──────────────┘
```

---

## 🔐 Segurança criptográfica

### Geração da semente
```cpp
uint32_t semente = entropia_viva  // Caos acumulado (Core 0)
                 ^ micros()       // Jitter de clock
                 ^ esp_random();  // RNG de hardware
```
Três fontes independentes combinadas via XOR garantem que a semente seja impraticável de prever.

### Sorteio sem viés
Para gerar números de 1 a 60 sem favorecer nenhum, o código descarta bytes ≥ 240:

```
Bytes válidos: 0–239  →  240 valores
240 ÷ 60 = 4 grupos exatos  →  probabilidade uniforme ✅
```

Isso elimina o viés de módulo que afeta geradores ingênuos.

### Proteção de race condition (dual-core)
Acessos à variável `entropia_viva` (escrita no Core 0, leitura no Core 1) são protegidos com spinlock nativo do FreeRTOS:

```cpp
portENTER_CRITICAL(&entropiaMux);
val = entropia_viva;
portEXIT_CRITICAL(&entropiaMux);
```

`volatile` sozinho não é suficiente no ESP32 dual-core — o spinlock garante consistência real.

---

## 📊 Monitor de Integridade

O Node-RED inclui um nó de **monitoramento estatístico contínuo** que analisa cada sorteio em busca de anomalias no hardware.

### O que é detectado

| Detecção | Método | O que indica |
|---|---|---|
| **Reboot do ESP32** | Semente = 0 | `micros()` zerou — chip reiniciou |
| **RNG travado** | Hash duplicado | Estado do gerador ciclando |
| **Entropia baixa** | Δ semente < 1000 | Degradação física do gerador |
| **Correlação serial** | >15% repetição entre sorteios | Memória indesejada no sistema |
| **Viés estatístico** | Qui-quadrado (χ²) > 75 | Alguns números saindo mais que outros |
| **Faixa estreita** | Desvio padrão EWMA < 70% | Números concentrados numa região |
| **Sequências** | Análise de runs | Números consecutivos suspeitos |

### Score de saúde do RNG

```
🟢 SAUDÁVEL   Score ≥ 85   Tudo normal
🟡 ATENÇÃO    Score ≥ 60   Anomalias leves detectadas
🔴 CRÍTICO    Score < 60   Problema grave — verificar hardware
```

Alertas são gravados automaticamente na aba **"Alertas"** do Google Sheets com timestamp, tipo, severidade e métricas contextuais.

---

## 🌐 WebServer embutido

O firmware inclui um servidor HTTP leve rodando na porta 80, acessível por qualquer dispositivo na mesma rede local — sem precisar de cabo serial ou monitor externo.

### Rotas disponíveis

| Rota | Descrição |
|---|---|
| `http://[IP]/` | Página visual com os dados do último sorteio |
| `http://[IP]/json` | Resposta JSON pura — útil para integrar outras ferramentas |

O IP é exibido no Serial Monitor na inicialização:

```
┌─────────────────────────────────────┐
│        ESP32 CaosForge Online        │
├─────────────────────────────────────┤
│ IP:   192.168.X.X                   │
│ Web:  http://192.168.X.X/           │
│ JSON: http://192.168.X.X/json       │
└─────────────────────────────────────┘
```

### O que a página exibe

- **Bolinhas dos 6 números** do último sorteio
- Semente, timestamp de uptime e número do sorteio
- Hash HMAC-SHA256 completo
- Temperatura do die do chip (veja seção abaixo)
- IP, uptime, RSSI do WiFi e heap livre em tempo real
- **Auto-refresh automático** a cada 65 segundos

### Resposta `/json`

```json
{
  "semente": 2847361920,
  "hash": "a3f2...c91d",
  "timestamp": "00d 00:03:00",
  "total": 3,
  "temp_cpu": 53.4,
  "numeros": [7, 23, 41, 5, 58, 19]
}
```

> O WebServer é mantido pelo `server.handleClient()` no `loop()`, leve o suficiente para não interferir no timing do sorteio de 60s.

---

## 🌡️ Temperatura do die (CPU)

O ESP32 possui um sensor de temperatura interno que mede o **die do chip** — o silício em si, não o ambiente ao redor.

### Sobre a leitura

A função utilizada é a da própria Espressif, exposta via `extern "C"`:

```cpp
extern "C" uint8_t temprature_sens_read(); // typo intencional — é o nome oficial do SDK

float getTempCPU() {
  return (temprature_sens_read() - 32) / 1.8f; // converte Fahrenheit → Celsius
}
```

> O nome `temprature_sens_read` com o "a" faltando é o nome real da função no SDK da Espressif — não é um erro de digitação no código.

### Precisão e utilidade

O sensor tem imprecisão de **±5–10°C** dependendo do chip e da tensão. O valor absoluto não é confiável como termômetro — mas a **variação** é relevante para o projeto:

| Temperatura | O que indica |
|---|---|
| Subindo gradualmente | Carga crescente do Core 0 gerando entropia |
| Pico após sorteio | HMAC-SHA256 aquecendo o processador brevemente |
| Queda repentina para valor fixo | Possível reboot do chip |

Na página web, a leitura é exibida com uma barra de progresso que muda de cor conforme a faixa:

```
🔵 Azul    < 55°C   Operação normal / carga leve
🟠 Laranja  55–70°C  Carga moderada
🔴 Vermelho > 70°C   Carga alta — verifique ventilação
```

### Compatibilidade

| Chip | Suporte |
|---|---|
| ESP32 (clássico, dual-core) | ✅ `temprature_sens_read()` |
| ESP32-S2 / ESP32-C3 / ESP32-S3 | ⚠️ API diferente (`temperature_sensor_*`) — requer adaptação |

---

## 💻 Configuração do ESP32

### Dependências
- [Arduino ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) (via Board Manager)
- Biblioteca `mbedtls` (incluída no ESP32 Arduino Core — não precisa instalar separado)

### Configuração no código (`caos.ino`)

```cpp
// Rede WiFi
const char* ssid     = "NOME_DO_SEU_WIFI";
const char* password = "SENHA_DO_WIFI";

// IP local do computador onde o Node-RED está rodando
const char* serverUrl = "http://192.168.X.X:1880/caos";
```

Para encontrar o IP do seu computador:
- **Windows:** `ipconfig` no terminal
- **Linux/Mac:** `ip addr` ou `ifconfig`

### Chave secreta HMAC

```cpp
const char* chave_secreta = "SUA_CHAVE_AQUI"; // Troque por algo único
```

A chave secreta é usada para assinar o hash de cada sorteio. Ela deve ser a mesma em todos os dispositivos que precisarem verificar os resultados.

### Upload
1. Abra o arquivo `caos.ino` na Arduino IDE (ou PlatformIO)
2. Selecione a placa: **ESP32 Dev Module**
3. Ajuste as configurações acima
4. Faça o upload

---

## 🔗 Configuração do Node-RED

### Importando o fluxo
1. Abra o Node-RED (`http://localhost:1880`)
2. Menu → **Import** → Cole o conteúdo de `flows.json`
3. Clique em **Import**

### Configuração obrigatória
Após importar, localize o nó **`requisição http`** e cole a URL do seu Google Apps Script (veja seção abaixo).

### Estrutura do fluxo

```
[POST /caos]
     │
     ├──▶ [http response]      ← Responde 200 ao ESP32
     │
     └──▶ [json]
               │
               ├──▶ [hash]     ──▶ Dashboard
               ├──▶ [sementes] ──▶ Dashboard
               ├──▶ [numeros]  ──▶ Dashboard
               ├──▶ [String]   ──▶ Debug
               ├──▶ [Learning] ──▶ Análise de tendências
               ├──▶ [IA]       ──▶ [Google Sheets - Dados]
               └──▶ [MONITOR]
                        │
                        ├──▶ Saída 1: Debug / Dashboard
                        └──▶ Saída 2: [Google Sheets - Alertas]
```

> **Nota sobre o nó IA:** O nó utiliza a biblioteca Synaptic para criar uma rede neural que tenta detectar padrões nos sorteios. Por design do HMAC-SHA256, nenhum padrão é encontrável — a rede serve como prova empírica do caos, registrando que acertos ficam na faixa do acaso (~46% de chance de acertar pelo menos 1 número por puro azar).

---

## 📊 Configuração do Google Sheets

### Apps Script (versão atual)

O script atual suporta duas abas automaticamente:

**Aba `Dados`** — sorteios normais:
| Data | Semente | Hash | N1 | N2 | N3 | N4 | N5 | N6 | IA_tentativa | IA_Acertos |

**Aba `Alertas`** — anomalias detectadas pelo Monitor:
| Timestamp | Tipo | Severidade | Mensagem | Valor | Score_RNG | Status_Geral | Chi2 | Desvio_EWMA | Total_Sorteios |

### Instalação
1. Abra sua planilha no [Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0)
2. **Extensões → Apps Script**
3. Cole o código do arquivo `google_apps_script.js` substituindo todo o conteúdo existente
4. Salve (Ctrl+S)
5. **Implantar → Nova implantação**
   - Tipo: **Aplicativo da Web**
   - Executar como: **Eu**
   - Quem pode acessar: **Qualquer pessoa**
6. Autorize as permissões solicitadas
7. **Copie a URL gerada** e cole no nó `requisição http` do Node-RED

> A mesma URL funciona tanto para dados normais quanto para alertas — o script detecta automaticamente o tipo de payload pelo campo `alertas`.

---

## 📁 Estrutura do repositório

```
Esp32CaosForge/
│
├── caos.ino                    # Firmware do ESP32
│                               # → Geração de entropia (dual-core)
│                               # → HMAC-SHA256 + sorteio sem viés
│                               # → WebServer embutido (porta 80)
│                               # → Sensor de temperatura do die
│                               # → Envio via HTTP POST
│
├── flows.json                  # Fluxo completo do Node-RED
│                               # → Recepção e roteamento dos dados
│                               # → Nó IA (Synaptic Perceptron)
│                               # → Monitor de Integridade estatístico
│                               # → Integração com Google Sheets
│
├── google_apps_script.js       # Script do Google Apps Script
│                               # → Roteamento automático Dados/Alertas
│                               # → Formatação condicional por severidade
│                               # → Health check via GET /exec
│
├── flow.jpg                    # Screenshot do fluxo Node-RED
│
└── README.md                   # Este arquivo
```

---

## 📈 Dados ao vivo

Os sorteios são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**

---

## 🧠 Fundamentos teóricos

**Por que hardware e não software?**
Geradores de números pseudo-aleatórios (PRNGs) como `Math.random()` ou `rand()` são algoritmos determinísticos — dada a mesma semente, produzem a mesma sequência. O ESP32 usa fontes físicas genuinamente não-determinísticas: variações de temperatura, interferências eletromagnéticas e instabilidades no oscilador de crystal.

**Por que o viés de módulo importa?**
Se você tem 256 valores (0–255) e aplica `% 60`, os números 0–15 têm 5 chances de aparecer enquanto os outros têm apenas 4. Com 10 mil sorteios, esse viés acumula ~4.000 ocorrências a mais para metade dos números — detectável estatisticamente. O CaosForge elimina esse problema descartando os 16 bytes problemáticos (240–255).

**O que o Qui-quadrado mede?**
O teste χ² compara a frequência observada de cada número com a frequência esperada para uma distribuição uniforme. Um χ² alto indica que alguns números aparecem sistematicamente mais que outros — sinal de viés no gerador.

---

*Projeto open source para estudo de entropia, criptografia aplicada e sistemas embarcados.*
