#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <time.h>
#include "mbedtls/md.h"

// --- CONFIGURAÇÕES DE REDE ---
const char* ssid      = "SSID";
const char* password  = "SENHA";
const char* serverUrl = "http://192.168.x.x:1880/caos";

// --- NTP (hora real) ---
// Fuso: America/Sao_Paulo = UTC-3 (-10800 segundos). Ajuste se necessário.
const char* ntpServer  = "pool.ntp.org";
const long  gmtOffset  = -3 * 3600; // UTC-3
const int   dstOffset  = 0;         // Brasil não usa horário de verão atualmente

// Retorna string no formato DD/MM/AAAA HH:MM:SS
String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "sem hora NTP";
  char buf[20];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  return String(buf);
}

// --- SENSOR DE TEMPERATURA INTERNO DO ESP32 ---
// Nota: mede o die do chip, não a temperatura ambiente.
// Imprecisão de ±5–10°C é normal — o valor absoluto importa menos
// que a variação, que reflete a carga térmica e a atividade do RNG.
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read(); // nome oficial da Espressif (typo intencional)
#ifdef __cplusplus
}
#endif

float getTempCPU() {
  return (temprature_sens_read() - 32) / 1.8f;
}

// --- CONTROLE DE TEMPO ---
unsigned long ultimaExecucao = 0;
const long intervalo = 60000; // 1 minuto

// --- WEBSERVER NA PORTA 80 ---
WebServer server(80);

// --- ENTROPIA PROTEGIDA ---
static portMUX_TYPE entropiaMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t entropia_viva = 0;

// --- ESTADO VISÍVEL NA WEB (atualizado a cada sorteio) ---
struct UltimoSorteio {
  int     numeros[6];
  uint32_t semente;
  char    hash[65];       // 32 bytes hex + null
  char    timestamp[32];
  int     total;
  float   tempCPU;
} ultimo;

bool temDados = false;

// ─────────────────────────────────────────────
// Core 0: Geração Contínua de Entropia Física
// ─────────────────────────────────────────────
void taskGeradoraDeCaos(void* pvParameters) {
  for (;;) {
    uint32_t novo = esp_random() ^ (uint32_t)micros();
    portENTER_CRITICAL(&entropiaMux);
    entropia_viva ^= novo ^ (uint32_t)(tan((double)entropia_viva) * 1e6);
    portEXIT_CRITICAL(&entropiaMux);
    vTaskDelay(1);
  }
}

uint32_t capturarEntropia() {
  uint32_t val;
  portENTER_CRITICAL(&entropiaMux);
  val = entropia_viva;
  portEXIT_CRITICAL(&entropiaMux);
  return val;
}

// ─────────────────────────────────────────────
// Uso dos núcleos via FreeRTOS runtime stats
//
// O FreeRTOS acumula "ticks de runtime" por task. Para calcular
// o uso percentual, tiramos dois snapshots com intervalo fixo e
// comparamos o delta de cada task com o delta total do sistema.
//
// Limitação: o ESP32 não expõe uso por núcleo diretamente.
// Identificamos o núcleo de cada task pelo campo xCoreID do
// TaskStatus_t e somamos os deltas das tasks de cada núcleo.
// ─────────────────────────────────────────────

// Habilita coleta de runtime stats no FreeRTOS
// (já ativado por padrão no ESP32 Arduino Core)
#define MAX_TASKS 20

struct CoreStats {
  float uso0;   // % Core 0
  float uso1;   // % Core 1
  uint32_t freq; // MHz
};

// Snapshot anterior para calcular delta
static uint32_t snap_runtime[MAX_TASKS] = {0};
static UBaseType_t snap_count = 0;
static TaskHandle_t snap_handles[MAX_TASKS] = {nullptr};

CoreStats getCoreStats() {
  CoreStats cs = {0, 0, 0};
  cs.freq = getCpuFrequencyMhz();

  TaskStatus_t tasks[MAX_TASKS];
  uint32_t totalRuntime;
  UBaseType_t n = uxTaskGetSystemState(tasks, MAX_TASKS, &totalRuntime);
  if (n == 0 || totalRuntime == 0) return cs;

  // Calcula delta do runtime total desde o último snapshot
  static uint32_t lastTotal = 0;
  uint32_t deltaTotal = totalRuntime - lastTotal;
  lastTotal = totalRuntime;
  if (deltaTotal == 0) return cs;

  float delta0 = 0, delta1 = 0;

  for (UBaseType_t i = 0; i < n; i++) {
    // Acha o runtime anterior desta task pelo handle
    uint32_t prevRuntime = 0;
    for (UBaseType_t j = 0; j < snap_count; j++) {
      if (snap_handles[j] == tasks[i].xHandle) {
        prevRuntime = snap_runtime[j];
        break;
      }
    }
    uint32_t delta = tasks[i].ulRunTimeCounter - prevRuntime;

    // xCoreID: 0 ou 1 para tasks fixadas; tskNO_AFFINITY (0x7FFFFFFF) para tasks flutuantes
    BaseType_t core = tasks[i].xCoreID;
    if (core == 0) delta0 += delta;
    else           delta1 += delta; // inclui tskNO_AFFINITY no core 1 (onde o loop() roda)
  }

  // Salva snapshot atual
  snap_count = (n < MAX_TASKS) ? n : MAX_TASKS;
  for (UBaseType_t i = 0; i < snap_count; i++) {
    snap_handles[i]  = tasks[i].xHandle;
    snap_runtime[i]  = tasks[i].ulRunTimeCounter;
  }

  // O ESP32 tem dois núcleos — divide o deltaTotal igualmente entre eles
  float halfTotal = deltaTotal / 2.0f;
  cs.uso0 = constrain((delta0 / halfTotal) * 100.0f, 0.0f, 100.0f);
  cs.uso1 = constrain((delta1 / halfTotal) * 100.0f, 0.0f, 100.0f);
  return cs;
}

// ─────────────────────────────────────────────
// Página HTML do WebServer
// Auto-refresh a cada 65s para sempre mostrar o último sorteio
// ─────────────────────────────────────────────
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta http-equiv="refresh" content="65">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 CaosForge</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d0d0d;color:#e0e0e0;font-family:'Courier New',monospace;
       display:flex;flex-direction:column;align-items:center;padding:30px 16px;min-height:100vh}
  h1{color:#00e5ff;font-size:1.4rem;letter-spacing:4px;margin-bottom:4px;text-transform:uppercase}
  .sub{color:#555;font-size:.75rem;margin-bottom:32px;letter-spacing:2px}
  .card{background:#111;border:1px solid #1e1e1e;border-radius:8px;
        padding:24px 28px;width:100%;max-width:540px;margin-bottom:16px}
  .label{color:#555;font-size:.7rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}
  .numbers{display:flex;gap:10px;flex-wrap:wrap}
  .ball{background:#00e5ff;color:#000;font-weight:bold;font-size:1.1rem;
        width:46px;height:46px;border-radius:50%;display:flex;align-items:center;
        justify-content:center;flex-shrink:0}
  .hash{color:#00e5ff;font-size:.72rem;word-break:break-all;line-height:1.7}
  .row{display:flex;justify-content:space-between;margin-bottom:10px}
  .val{color:#fff;font-size:.9rem}
  .muted{color:#444;font-size:.7rem}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
       background:#00e5ff;margin-right:6px;animation:blink 1.2s infinite}
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
  .footer{color:#333;font-size:.65rem;margin-top:24px;text-align:center;line-height:1.8}
  .temp-bar-bg{background:#1e1e1e;border-radius:4px;height:6px;width:100%;margin-top:6px}
  .temp-bar{height:6px;border-radius:4px;background:linear-gradient(90deg,#00e5ff,#ff6d00);transition:width .5s}
  .temp-val{font-size:1.6rem;font-weight:bold;color:#ff6d00}
  .cpu-row{display:flex;align-items:center;gap:10px;margin-bottom:10px}
  .cpu-label{color:#555;font-size:.7rem;width:56px;flex-shrink:0}
  .cpu-bar-bg{flex:1;background:#1e1e1e;border-radius:4px;height:8px}
  .cpu-bar{height:8px;border-radius:4px;transition:width .5s}
  .cpu-pct{color:#fff;font-size:.8rem;width:38px;text-align:right;flex-shrink:0}
  .nodata{color:#444;text-align:center;padding:20px;font-size:.85rem}
</style>
</head>
<body>
<h1>⚡ CaosForge</h1>
<p class="sub">ESP32 · HMAC-SHA256 · LIVE</p>
)rawhtml";

  if (!temDados) {
    html += "<div class='card'><p class='nodata'>Aguardando primeiro sorteio...</p></div>";
  } else {
    // Números
    html += "<div class='card'>";
    html += "<div class='label'>Último Sorteio</div>";
    html += "<div class='numbers'>";
    for (int i = 0; i < 6; i++) {
      html += "<div class='ball'>" + String(ultimo.numeros[i]) + "</div>";
    }
    html += "</div></div>";

    // Metadados
    html += "<div class='card'>";
    html += "<div class='row'><span class='muted'>Timestamp</span><span class='val'>" + String(ultimo.timestamp) + "</span></div>";
    html += "<div class='row'><span class='muted'>Semente</span><span class='val'>" + String(ultimo.semente) + "</span></div>";
    html += "<div class='row'><span class='muted'>Sorteios</span><span class='val'>" + String(ultimo.total) + "</span></div>";
    html += "</div>";

    // Hash
    html += "<div class='card'>";
    html += "<div class='label'>HMAC-SHA256</div>";
    html += "<div class='hash'>" + String(ultimo.hash) + "</div>";
    html += "</div>";

    // Temperatura do CPU
    float t = ultimo.tempCPU;
    // Barra: faixa normal 40–80°C mapeada para 0–100%
    int pct = constrain((int)((t - 30) / 60.0f * 100), 0, 100);
    String corTemp = t < 55 ? "#00e5ff" : t < 70 ? "#ffb300" : "#f44336";
    html += "<div class='card'>";
    html += "<div class='label'>Temperatura do Die (CPU)</div>";
    html += "<div class='row' style='align-items:baseline'>";
    html += "<span class='temp-val' style='color:" + corTemp + "'>" + String(t, 1) + "°C</span>";
    html += "<span class='muted' style='margin-left:10px'>die interno · ±5–10°C de imprecisão</span>";
    html += "</div>";
    html += "<div class='temp-bar-bg'><div class='temp-bar' style='width:" + String(pct) + "%;background:" + corTemp + "'></div></div>";
    html += "</div>";
  }

  // Card de uso dos núcleos
  CoreStats cs = getCoreStats();
  String cor0 = cs.uso0 < 60 ? "#00e5ff" : cs.uso0 < 85 ? "#ffb300" : "#f44336";
  String cor1 = cs.uso1 < 60 ? "#00e5ff" : cs.uso1 < 85 ? "#ffb300" : "#f44336";
  html += "<div class='card'>";
  html += "<div class='label'>Núcleos do Processador · " + String(cs.freq) + " MHz</div>";
  // Core 0
  html += "<div class='cpu-row'>";
  html += "<span class='cpu-label'>Core 0</span>";
  html += "<div class='cpu-bar-bg'><div class='cpu-bar' style='width:" + String((int)cs.uso0) + "%;background:" + cor0 + "'></div></div>";
  html += "<span class='cpu-pct' style='color:" + cor0 + "'>" + String(cs.uso0, 1) + "%</span>";
  html += "</div>";
  // Core 1
  html += "<div class='cpu-row' style='margin-bottom:0'>";
  html += "<span class='cpu-label'>Core 1</span>";
  html += "<div class='cpu-bar-bg'><div class='cpu-bar' style='width:" + String((int)cs.uso1) + "%;background:" + cor1 + "'></div></div>";
  html += "<span class='cpu-pct' style='color:" + cor1 + "'>" + String(cs.uso1, 1) + "%</span>";
  html += "</div>";
  html += "<div style='margin-top:10px;color:#333;font-size:.65rem'>Core 0 = Forja de Caos &nbsp;·&nbsp; Core 1 = Oráculo + WebServer</div>";
  html += "</div>";

  // Status do sistema
  html += "<div class='card'>";
  html += "<div class='label'>Sistema</div>";
  html += "<div class='row'><span class='muted'>IP</span><span class='val'>" + WiFi.localIP().toString() + "</span></div>";
  html += "<div class='row'><span class='muted'>Uptime</span><span class='val'>" + String(millis() / 1000) + "s</span></div>";
  html += "<div class='row'><span class='muted'>RSSI WiFi</span><span class='val'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  html += "<div class='row'><span class='muted'>Heap livre</span><span class='val'>" + String(ESP.getFreeHeap() / 1024) + " KB</span></div>";
  html += "<div class='row'><span class='muted'>Status</span><span class='val'><span class='dot'></span>Online</span></div>";
  html += "</div>";

  html += "<p class='footer'>Atualiza automaticamente a cada 65s<br>ESP32 CaosForge · github.com/deletrr/Esp32CaosForge</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Endpoint JSON puro — útil para integrar com outras ferramentas
void handleJson() {
  if (!temDados) {
    server.send(200, "application/json", "{\"status\":\"aguardando\"}");
    return;
  }
  CoreStats cs = getCoreStats();
  String json = "{\"semente\":" + String(ultimo.semente) +
                ",\"hash\":\"" + String(ultimo.hash) + "\"" +
                ",\"timestamp\":\"" + String(ultimo.timestamp) + "\"" +
                ",\"total\":" + String(ultimo.total) +
                ",\"temp_cpu\":" + String(ultimo.tempCPU, 1) +
                ",\"cpu_freq_mhz\":" + String(cs.freq) +
                ",\"core0_pct\":" + String(cs.uso0, 1) +
                ",\"core1_pct\":" + String(cs.uso1, 1) +
                ",\"numeros\":[" +
                String(ultimo.numeros[0]) + "," + String(ultimo.numeros[1]) + "," +
                String(ultimo.numeros[2]) + "," + String(ultimo.numeros[3]) + "," +
                String(ultimo.numeros[4]) + "," + String(ultimo.numeros[5]) + "]}";
  server.send(200, "application/json", json);
}

// ─────────────────────────────────────────────
// Sorteio + envio para Node-RED
// ─────────────────────────────────────────────
void realizarSorteioEEnviar() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    return;
  }

  uint32_t semente_capturada = capturarEntropia() ^ (uint32_t)micros() ^ esp_random();
  const char* chave_secreta  = "SALT"; // Substitua por uma chave forte

  uint8_t hash_final[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)chave_secreta, strlen(chave_secreta));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)&semente_capturada, sizeof(semente_capturada));
  mbedtls_md_hmac_finish(&ctx, hash_final);
  mbedtls_md_free(&ctx);

  int numeros[6];
  int encontrados = 0, byte_idx = 0;
  while (encontrados < 6) {
    if (byte_idx >= 32) {
      mbedtls_md_init(&ctx);
      mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
      mbedtls_md_hmac_starts(&ctx, (const unsigned char*)chave_secreta, strlen(chave_secreta));
      mbedtls_md_hmac_update(&ctx, hash_final, 32);
      mbedtls_md_hmac_finish(&ctx, hash_final);
      mbedtls_md_free(&ctx);
      byte_idx = 0;
    }
    uint8_t b = hash_final[byte_idx++];
    if (b >= 240) continue;
    int num = (b % 60) + 1;
    bool rep = false;
    for (int i = 0; i < encontrados; i++) if (numeros[i] == num) { rep = true; break; }
    if (!rep) numeros[encontrados++] = num;
  }

  // Monta hash string
  char hashStr[65];
  for (int i = 0; i < 32; i++) sprintf(hashStr + i * 2, "%02x", hash_final[i]);
  hashStr[64] = '\0';

  // Atualiza estado visível na web
  for (int i = 0; i < 6; i++) ultimo.numeros[i] = numeros[i];
  ultimo.semente  = semente_capturada;
  ultimo.tempCPU  = getTempCPU();
  memcpy(ultimo.hash, hashStr, 65);
  ultimo.total++;
  // Timestamp formatado: DD/MM/AAAA HH:MM:SS
  // Nota: sem NTP o ESP32 conta a partir de 01/01/1970 — conecte NTP para hora real.
  // Por ora usa uptime legível para não depender de servidor externo.
  String ts = getTimestamp();
  ts.toCharArray(ultimo.timestamp, sizeof(ultimo.timestamp));
  temDados = true;

  // Serial print
  Serial.println("┌─────────────────────────────────────┐");
  Serial.printf( "│ Sorteio #%-27d │\n", ultimo.total);
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf( "│ Números: %02d  %02d  %02d  %02d  %02d  %02d       │\n",
    numeros[0], numeros[1], numeros[2], numeros[3], numeros[4], numeros[5]);
  Serial.printf( "│ Semente: %-27u │\n", semente_capturada);
  Serial.println("│ Hash:                               │");
  Serial.printf( "│  %.37s │\n", hashStr);
  Serial.printf( "│  %.37s │\n", hashStr + 37);  // segunda metade
  Serial.printf( "│ URL: http://%-24s │\n", (WiFi.localIP().toString() + "/").c_str());
  Serial.printf( "│ Temp CPU: %-26s │\n", (String(ultimo.tempCPU, 1) + " °C (die interno)").c_str());
  CoreStats csSerial = getCoreStats();
  Serial.printf( "│ Core 0: %-28s │\n", (String(csSerial.uso0, 1) + "% · Forja de Caos").c_str());
  Serial.printf( "│ Core 1: %-28s │\n", (String(csSerial.uso1, 1) + "% · Oráculo + Web").c_str());
  Serial.printf( "│ CPU Freq: %-26s │\n", (String(csSerial.freq) + " MHz").c_str());
  Serial.println("└─────────────────────────────────────┘");

  // Envia para Node-RED
  String jsonPayload = "{\"origem\":\"ESP32_Chaos\",\"semente\":" + String(semente_capturada) +
                       ",\"hash\":\"" + String(hashStr) + "\",\"numeros\":[" +
                       String(numeros[0]) + "," + String(numeros[1]) + "," +
                       String(numeros[2]) + "," + String(numeros[3]) + "," +
                       String(numeros[4]) + "," + String(numeros[5]) + "]}";

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonPayload);
  http.end();

  Serial.printf("→ Node-RED: HTTP %d\n\n", code);
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();

  // Sincroniza hora via NTP
  configTime(gmtOffset, dstOffset, ntpServer);
  Serial.print("Sincronizando NTP");
  struct tm t;
  while (!getLocalTime(&t)) { delay(500); Serial.print("."); }
  Serial.println(" OK — " + getTimestamp());

  // Rotas do WebServer
  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.begin();

  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│        ESP32 CaosForge Online        │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf( "│ IP:   %-30s│\n", (WiFi.localIP().toString()).c_str());
  Serial.printf( "│ Web:  http://%-23s│\n", (WiFi.localIP().toString() + "/").c_str());
  Serial.printf( "│ JSON: http://%-23s│\n", (WiFi.localIP().toString() + "/json").c_str());
  Serial.println("└─────────────────────────────────────┘\n");

  xTaskCreatePinnedToCore(taskGeradoraDeCaos, "UsinaCaos", 4096, NULL, 1, NULL, 0);

  ultimo.total = 0;
}

// ─────────────────────────────────────────────
void loop() {
  server.handleClient(); // mantém o webserver respondendo

  if (millis() - ultimaExecucao >= intervalo) {
    ultimaExecucao = millis();
    realizarSorteioEEnviar();
  }
}
