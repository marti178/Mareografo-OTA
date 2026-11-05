/**************************************************************
* Proyect: Mareografo V1
* Author: Cronon Tecnologia
* Date: 9/02/2023
* Proyect Manager: Fernando Agostino
* Proyect Leader: Martin Marotta
**************************************************************/

// Credenciales (APN, MQTT, CallMeBot): ver include/secrets.example.h
// include/secrets.h NO se sube a git (.gitignore).
#include "secrets.h"

// La version viene de version.txt (la inyecta scripts/version.py).
// Es la UNICA fuente: el workflow de GitHub tambien la lee de ahi.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

// --- OTA ---
#define OTA_HOST "marti178.github.io"
#define OTA_PATH_BIN "/Mareografo-OTA/firmware.bin"
#define OTA_PATH_VERSION "/Mareografo-OTA/version.json"
#define OTA_REQUIRE_SHA256 1 // 1 = no se instala nada sin sha256 valido en version.json

// --- Ciclo de trabajo ---
#define AWAKE_INTERVAL (2UL * 60UL * 1000UL)  // ventana despierto, medida desde que termina setup() (ms)
#define PUBLISH_INTERVAL (30UL * 1000UL)      // cada cuanto se sensa/publica mientras esta despierto (ms)
#define SLEEP_MINUTES 15                      // cada cuanto se despierta el dispositivo
#define NET_MAX_TRIES 6                       // intentos de registro en red antes de rendirse y dormir
#define MAX_UNCONFIRMED_BOOTS 3               // arranques sin "confirmar" firmware nuevo antes de volver al anterior

// Select your modem:
#define TINY_GSM_MODEM_SIM7070 true
// Set serial
#define SerialMon Serial
#define SerialMaxB Serial1
#define SerialAT Serial2
#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false
#define MODE_GPRS 1
#define ADC_PIN 35
// OJO: TINY_GSM_RX_BUFFER (1024) se define en platformio.ini (build_flags).
// Con el valor por defecto (64) la descarga OTA es ~16 veces mas lenta.

// Your GPRS credentials, if any
const char apn[]      = GPRS_APN;
const char gprsUser[] = GPRS_USER;
const char gprsPass[] = GPRS_PASS;

// MQTT details
const char* broker = MQTT_BROKER;
const char* topicDistancia      = "Aquaman1/Distancia(m)";
const char* topicInit      = "Aquaman1/init";
const char* topicTemperatura = "Aquaman1/Temperatura";
const char* topicPresion = "Aquaman1/Presion(hPa)";
const char* topicSignal = "Aquaman1/Signal";
const char* topicBateria = "Aquaman1/Bateria(V)";
const char* topicFirmware = "Aquaman1/Firmware";
const char* topicCommand = "Aquaman1/Command";
const char* topicState = "Aquaman1/State";
const char* topicSolar = "Aquaman1/Solar(V)";
const char* topicDebug = "Aquaman1/Debug";
const char* topicOta = "Aquaman1/OTA"; // porcentaje (0-100) mientras se actualiza

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Arduino.h>
#include <SFE_BMP180.h>

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>

#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"

// Just in case someone defined the wrong thing..
#if TINY_GSM_USE_GPRS && not defined TINY_GSM_MODEM_HAS_GPRS
#undef TINY_GSM_USE_GPRS
#undef TINY_GSM_USE_WIFI
#define TINY_GSM_USE_GPRS false
#define TINY_GSM_USE_WIFI true
#endif
#if TINY_GSM_USE_WIFI && not defined TINY_GSM_MODEM_HAS_WIFI
#undef TINY_GSM_USE_GPRS
#undef TINY_GSM_USE_WIFI
#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false
#endif

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm        modem(debugger);
#else
TinyGsm        modem(SerialAT);
#endif
// MQTT 1
TinyGsmClient client(modem, 0);
PubSubClient mqtt(client);

// OTA HTTPS
TinyGsmClientSecure otaClient(modem, 2);
HttpClient http(otaClient, OTA_HOST, 443);

// CallMeBot HTTPS
TinyGsmClientSecure botcliente(modem, 1);
HttpClient callmebot(botcliente, "api.callmebot.com", 443);

#define MODEM_POWER_KEY 4
#define DATAIN 32
#define RANGING 33

uint32_t lastReconnectAttempt = 0;
uint32_t awakeStart = 0;
SFE_BMP180 pressure;
double temp = NAN;
double pres = NAN;
char mqttClientId[24];

// Comando recibido por MQTT. El callback SOLO lo guarda; se ejecuta desde loop()
// (antes el OTA corria adentro del callback, re-entrando a mqtt.loop()).
static char pendingCmd[32];
static volatile bool cmdPending = false;

boolean mqttConnect() {
  SerialMon.print("Connecting to ");
  SerialMon.print(broker);
  boolean status = mqtt.connect(mqttClientId, MQTT_USER, MQTT_PASS);

  if (status == false) {
    SerialMon.println(" fail");
    SerialMon.print("MQTT state: ");
    SerialMon.println(mqtt.state());
    return false;
  }

  SerialMon.println(" success");
  mqtt.publish(topicInit, "Aquaman1 started");
  bool subOk = mqtt.subscribe(topicCommand);
  SerialMon.print("Suscripcion a ");
  SerialMon.print(topicCommand);
  SerialMon.println(subOk ? ": OK" : ": FALLO");
  return mqtt.connected();
}

void publicarDatos(double temperatura, double presion, double nivel, double bateria) {

    JsonDocument doc;

    // NaN se serializa como null (valor invalido != valor 0)
    doc["nivel_marea"] = nivel;
    doc["temp_exterior"] = nullptr;
    doc["temp_equipo"] = temperatura;
    doc["bateria_v"] = bateria;
    doc["presion_hpa"] = presion;

    char buffer[256];
    serializeJson(doc, buffer);

    mqtt.publish("amigosdelagua/mareografo/telemetria", buffer);
}

void enviarCallMeBot() {
#ifdef CALLMEBOT_APIKEY
    Serial.println("Enviando mensaje por CallMeBot...");

    // %2B = '+' (un '+' crudo en una query se interpreta como espacio)
    String ruta = String("/whatsapp.php?phone=%2B") + CALLMEBOT_PHONE +
                  "&text=Aquaman1%3A%20iniciando%20actualizacion%20OTA" +
                  "&apikey=" + CALLMEBOT_APIKEY;

    int error = callmebot.get(ruta);

    Serial.print("Resultado GET: ");
    Serial.println(error);

    // Si error es 0, intentamos leer la respuesta HTTP
    if (error == 0) {
        int statusCode = callmebot.responseStatusCode();

        Serial.print("HTTP Status Code: ");
        Serial.println(statusCode);

        String respuesta = callmebot.responseBody();

        Serial.println("Respuesta del servidor:");
        Serial.println(respuesta);
    }
    else {
        Serial.println("Error al realizar la solicitud HTTP");
    }
    callmebot.stop(); // liberar el socket (mux 1) en cualquier caso
#endif
}

void powerOffModem()
{
  // AT+CPOWD=1 es el apagado "prolijo" por software del SIM7070/7080
  // (a diferencia de simplemente cortarle la alimentacion). Despues de
  // esto, para volver a prenderlo hace falta el pulso de PWRKEY de
  // nuevo — que es justo lo que powerOnModem() ya hace de forma segura.
  SerialMon.println("Apagando modem antes de dormir...");
  modem.sendAT("+CPOWD=1");
  modem.waitResponse(5000);
  delay(2000); // darle tiempo real a que se apague antes de que el ESP32 tambien duerma
}

void debugLog(const char* format, ...)
{
    char buffer[256];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (mqtt.connected())
        mqtt.publish(topicDebug, buffer);
}

// ---------------------------------------------------------------------
// Rollback por software.
// El bootloader de Arduino no trae rollback automatico, asi que lo hacemos
// a mano: tras un OTA se marca "pend"; si el firmware nuevo arranca
// MAX_UNCONFIRMED_BOOTS veces sin llegar a un camino normal de sleep
// (crash loop / cuelgue + reset), se vuelve a la particion anterior.
// ---------------------------------------------------------------------
void markFirmwarePending()
{
    Preferences p;
    p.begin("ota", false);
    p.putUChar("pend", 1);
    p.putUChar("boots", 0);
    p.end();
}

void confirmFirmware()
{
    Preferences p;
    p.begin("ota", false);
    if (p.getUChar("pend", 0)) {
        p.putUChar("pend", 0);
        p.putUChar("boots", 0);
        SerialMon.println("Firmware nuevo confirmado");
    }
    p.end();
}

void checkRollback()
{
    Preferences p;
    p.begin("ota", false);
    if (p.getUChar("pend", 0)) {
        uint8_t boots = p.getUChar("boots", 0) + 1;
        if (boots > MAX_UNCONFIRMED_BOOTS) {
            p.putUChar("pend", 0);
            p.putUChar("boots", 0);
            p.end();
            const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
            if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
                SerialMon.println("Firmware nuevo no se confirmo: volviendo a la version anterior");
                delay(200);
                ESP.restart();
            }
            SerialMon.println("Rollback fallo: no hay particion anterior valida");
            return;
        }
        p.putUChar("boots", boots);
    }
    p.end();
}

void goToSleep(uint32_t minutes)
{
    SerialMon.printf("Durmiendo %u minutos...\n", (unsigned)minutes);
    confirmFirmware(); // llegar a un sleep normal = el firmware esta vivo
    if (mqtt.connected()) {
        debugLog("Durmiendo %u minutos...", (unsigned)minutes);
        mqtt.publish(topicState, "Sleeping");
        mqtt.loop();
        mqtt.disconnect();
    }
    client.stop();
    powerOffModem();
    esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL); // microsegundos
    esp_deep_sleep_start(); // de aca no vuelve: el chip se reinicia al despertar
}

// ---------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------
static void hexOf(const uint8_t* h, size_t n, char* out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", h[i]);
    out[2 * n] = 0;
}

static void logNetworkInfo()
{
    SerialMon.println("===== INFO RED =====");
    SerialMon.print("Operador: ");
    SerialMon.println(modem.getOperator());
    SerialMon.print("Senial (CSQ): ");
    SerialMon.println(modem.getSignalQuality());
    SerialMon.print("Modem: ");
    SerialMon.println(modem.getModemInfo());
    SerialMon.print("Conectado a red: ");
    SerialMon.println(modem.isNetworkConnected() ? "SI" : "NO");
    SerialMon.print("GPRS conectado: ");
    SerialMon.println(modem.isGprsConnected() ? "SI" : "NO");
    SerialMon.print("IP: ");
    SerialMon.println(modem.localIP());
    SerialMon.println("===== INFO CELDA =====");
    modem.sendAT("+CPSI?");
    String resp;
    modem.waitResponse(5000, resp);
    SerialMon.println(resp);
    SerialMon.println("======================");
}

// Publica el avance del OTA. Se llama cada 5% (~20 veces por descarga), no por
// cada lectura: cada publish son comandos AT que compiten con el socket de
// descarga. El modem bufferea los datos del socket mientras tanto.
// Si MQTT esta caido NO se reconecta aca (seria lento): simplemente se saltea.
static void publishOtaProgress(int percent, size_t w, size_t t, float kbps)
{
    if (!mqtt.connected()) return;
    char b[8];
    snprintf(b, sizeof(b), "%d", percent);
    mqtt.publish(topicOta, b);
    debugLog("OTA %d%% %u/%u bytes %.1f KB/s", percent, (unsigned)w, (unsigned)t, kbps);
    mqtt.loop();
}

bool downloadFirmware(const String& expectedSha)
{
#if OTA_REQUIRE_SHA256
    if (expectedSha.length() != 64)
    {
        SerialMon.println("version.json sin sha256 valido: OTA cancelada");
        debugLog("OTA cancelada: falta sha256");
        return false;
    }
#endif

    SerialMon.println("Descargando firmware...");
    debugLog("Descargando firmware...");
    mqtt.publish(topicState, "Updating");
    mqtt.publish(topicOta, "0");
    logNetworkInfo();

    // Una sola conexion pide TODO el archivo (Range "bytes=N-") y el tamano sale
    // del Content-Length de esa misma respuesta: antes se hacia un GET extra
    // solo para leer el tamano (otro handshake TLS completo). Si la conexion se
    // corta, se reanuda desde 'written' (bytes ya grabados en flash).
    const uint32_t STALL_TIMEOUT  = 20000; // ms sin datos = intento trabado
    const int MAX_FALLOS_SEGUIDOS = 8;     // reintentos SIN progreso antes de rendirse

    static uint8_t buffer[2048];
    size_t   total       = 0;
    size_t   written     = 0;    // SIEMPRE = bytes realmente grabados en flash (fuente de verdad)
    int      fallos      = 0;
    bool     updateStarted = false;
    uint32_t otaStart    = millis();
    uint32_t lastLog     = 0;
    int      lastStep    = 0;    // ultimo multiplo de 5% publicado por MQTT

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    auto abortar = [&](const char* motivo) {
        SerialMon.println(motivo);
        if (updateStarted) Update.abort();
        mbedtls_sha256_free(&sha);
        http.stop();
        debugLog("OTA: %s", motivo); // la conexion OTA ya esta cerrada
    };
    // true = hay que rendirse
    auto fallo = [&](const char* msg) -> bool {
        fallos++;
        SerialMon.printf("%s (reintento %d/%d)\n", msg, fallos, MAX_FALLOS_SEGUIDOS);
        if (fallos > MAX_FALLOS_SEGUIDOS) {
            abortar("Demasiados fallos seguidos, abortando OTA");
            return true;
        }
        delay(3000);
        return false;
    };

    while (total == 0 || written < total)
    {
        yield();
        http.stop();
        delay(200);

        String rangeHeader = "bytes=" + String((unsigned long)written) + "-";

        http.beginRequest();
        int err = http.get(OTA_PATH_BIN);
        if (err == 0) {
            http.sendHeader("Range", rangeHeader);
            http.endRequest();
        }
        int status = (err == 0) ? http.responseStatusCode() : err;

        // Si el servidor no respeta el Range (200 en vez de 206 para un pedido
        // que no arranca en el byte 0) no se puede reanudar: mejor frenar que corromper.
        if (written > 0 && status == 200)
        {
            abortar("El servidor no soporta Range (HTTP 200 en vez de 206)");
            return false;
        }
        if (status != 206 && status != 200)
        {
            if (fallo("Pedido fallo")) return false;
            continue;
        }

        // Hay que descartar los headers antes de leer el body.
        http.skipResponseHeaders();
        long cl = http.contentLength(); // bytes que faltan en ESTA respuesta
        if (cl <= 0)
        {
            if (fallo("Content-Length invalido")) return false;
            continue;
        }

        if (total == 0)
        {
            total = written + (size_t)cl;
            SerialMon.printf("Firmware: %u bytes\n", (unsigned)total);
            if (!Update.begin(total))
            {
                abortar("No hay espacio suficiente para OTA");
                return false;
            }
            updateStarted = true;
        }
        else if (written + (size_t)cl != total)
        {
            abortar("El archivo cambio en el servidor durante la descarga");
            return false;
        }

        size_t   pedido   = total - written;
        size_t   recibido = 0;
        uint32_t lastData = millis();
        bool     stall    = false;

        while (recibido < pedido)
        {
            int avail = http.available();

            if (avail > 0)
            {
                size_t want = min((size_t)avail, min(sizeof(buffer), pedido - recibido));
                int len = http.read(buffer, want);

                if (len > 0)
                {
                    // Un firmware ESP32 siempre arranca con 0xE9. Si no, es basura
                    // (p.ej. headers HTTP o una pagina de error): no seguir.
                    if (written == 0 && recibido == 0 && buffer[0] != 0xE9)
                    {
                        abortar("Imagen invalida: el primer byte no es 0xE9");
                        return false;
                    }

                    if (Update.write(buffer, len) != (size_t)len)
                    {
                        // Error real de flash, no de red: no tiene sentido reintentar.
                        SerialMon.print("ERROR escribiendo flash: ");
                        SerialMon.println(Update.errorString());
                        abortar("Error de escritura en flash");
                        return false;
                    }
                    mbedtls_sha256_update(&sha, buffer, len);

                    // 'written' avanza apenas se graba: un stall nunca duplica datos.
                    written  += len;
                    recibido += len;
                    lastData  = millis();
                    fallos    = 0;

                    // Serial cada 5 s (barato); MQTT solo al cruzar cada 5%.
                    int   percent = (int)((written * 100) / total);
                    float secs    = (millis() - otaStart) / 1000.0;
                    float kbps    = secs > 0 ? (written / 1024.0) / secs : 0.0;

                    if (millis() - lastLog >= 5000 || written == total)
                    {
                        lastLog = millis();
                        SerialMon.printf("OTA (%d%%)  %u/%u  | %.2f KB/s\n",
                                         percent, (unsigned)written, (unsigned)total, kbps);
                    }
                    if (percent / 5 > lastStep)
                    {
                        lastStep = percent / 5;
                        publishOtaProgress(percent, written, total, kbps);
                    }
                }
            }
            else if (!http.connected())
            {
                stall = true; // el servidor cerro antes de tiempo
                break;
            }
            else if (millis() - lastData > STALL_TIMEOUT)
            {
                stall = true;
                break;
            }
            else
            {
                delay(1);
            }
        }

        http.stop();
        delay(100);

        if (stall)
        {
            SerialMon.printf("Stall en byte %u\n", (unsigned)written);
            if (fallo("Descarga trabada")) return false;
            continue; // pide desde el 'written' ya actualizado
        }
    }

    http.stop();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char hex[65];
    hexOf(digest, sizeof(digest), hex);
    SerialMon.printf("SHA-256: %s\n", hex);

    if (expectedSha.length() == 64)
    {
        String esperado = expectedSha;
        esperado.toLowerCase();
        if (!esperado.equals(hex))
        {
            Update.abort();
            SerialMon.println("SHA-256 NO coincide: firmware descartado");
            debugLog("OTA: SHA-256 no coincide");
            return false;
        }
    }

    if (!Update.end())
    {
        SerialMon.print("Update Error: ");
        SerialMon.println(Update.errorString());
        debugLog("Update Error: %s", Update.errorString());
        return false;
    }

    if (!Update.isFinished())
    {
        SerialMon.println("Firmware incompleto");
        debugLog("Firmware incompleto");
        return false;
    }

    SerialMon.println("OTA finalizada correctamente");
    debugLog("OTA finalizada correctamente");
    markFirmwarePending(); // si el nuevo no se confirma, checkRollback() vuelve al anterior
    delay(1000);

    ESP.restart();

    return true;
}

void powerOnModem()
{
  pinMode(MODEM_POWER_KEY, OUTPUT);
  // SIM7070 power on
  digitalWrite(MODEM_POWER_KEY, HIGH);
  delay(100);
  digitalWrite(MODEM_POWER_KEY, LOW);
  delay(1100);
  digitalWrite(MODEM_POWER_KEY, HIGH);

  pinMode(MODEM_POWER_KEY, INPUT);
}

// UART del modem. El buffer RX de HardwareSerial es de 256 B por defecto y
// debe ampliarse ANTES de begin(): con CARECV de 1024 B a 921600 baud se
// pueden perder bytes -> descarga corrupta.
static void atBegin(uint32_t baud)
{
  SerialAT.end();
  SerialAT.setRxBufferSize(4096);
  SerialAT.begin(baud, SERIAL_8N1, 26, 27);
}

void InitUart(void){
  //Seteo de modo de pines
  pinMode(DATAIN,INPUT); //Pin 32 entrada del Maxbotix
  pinMode(RANGING,OUTPUT); //Pin 33 salida para marcar comienzo de lectura del Maxbotix

  // Set console baud rate
  SerialMon.begin(115200);
  delay(1000);
  atBegin(921600);
  delay(1000);
  SerialMaxB.begin(9600,SERIAL_8N1,DATAIN);
  delay(1000);
}

// Si el modem no contesta a 921600 (p.ej. modem nuevo o reseteado de fabrica,
// que arranca en autobaud/115200), lo reconfigura y guarda el baudrate.
static void modemBaudSync()
{
  if (modem.testAT(2000)) return;
  SerialMon.println("Modem no responde a 921600, probando 115200...");
  atBegin(115200);
  if (modem.testAT(2000)) {
    modem.sendAT("+IPR=921600");
    modem.waitResponse();
    delay(200);
    atBegin(921600);
    delay(300);
    modem.sendAT("&W"); // persistir
    modem.waitResponse();
    SerialMon.println("Baudrate del modem fijado a 921600");
  } else {
    atBegin(921600); // no era problema de baudrate: seguimos como siempre
  }
}

void InitSensors(){

  if (pressure.begin())
  SerialMon.println("BMP180 Inicio completado");
  else
  {
    // Hubo un problema
    SerialMon.println("BMP180 Error de inicio ");
    for (int i=0; i<10; i++){
      digitalWrite(12,HIGH);
      delay(1000);
      digitalWrite(12,LOW);
      delay(1000);
    }

  }
  return;
}

bool InitModem(void){

  powerOnModem();
  delay(2500);
  modemBaudSync();
  // Restart takes quite some time
  // To skip it, call init() instead of restart()
  SerialMon.println("Initializing modem... ");
  modem.restart();
  //modem.init();
  delay(6000);
  SerialMon.println("Configurando modem... ");
  modem.setPreferredMode(1); // 1 = LTE Cat-M1 (2 = NB-IoT, 3 = ambos)
  delay(3000);
  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);
  delay(1000);

  SerialMon.print("Waiting for network...");
  delay(3000);

  int intentos = 0;
  while (!modem.waitForNetwork(15000L))
  {
      intentos++;

      SerialMon.printf("Intento %d fallido\n", intentos);

      if (intentos >= NET_MAX_TRIES)
      {
          // Sin cobertura: no quedarse en un bucle infinito gastando bateria.
          SerialMon.println("Sin red: abandono y vuelvo a dormir");
          return false;
      }

      if (intentos % 2 == 0)
      {
          SerialMon.println("Reiniciando el modem...");
          modem.restart();
      }

      delay(5000);
  }

  SerialMon.println(" success");

  if (modem.isNetworkConnected()) { SerialMon.println("Network connected"); }
    #if TINY_GSM_USE_GPRS
    // GPRS connection parameters are usually set after network registration
    SerialMon.print(F("Connecting to "));
    SerialMon.print(apn);
    if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
      SerialMon.println(" fail");
      delay(10000);
      return true; // MQTTVerify() reintenta en el loop
    }
    SerialMon.println(" success");
    if (modem.isGprsConnected()) { SerialMon.println("GPRS connected"); }

    // Desactivar PSM y eDRX: si quedan habilitados, el modem puede
    // "dormirse" solo en medio de una transferencia larga (como la
    // descarga OTA) y quedarse sin recibir datos varios segundos/minutos.
    modem.sendAT("+CPSMS=0");
    modem.waitResponse();
    delay(200);
    modem.sendAT("+CEDRXS=0");
    modem.waitResponse();
    delay(200);
  #endif

  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
    Serial.print("Topic: ");
    Serial.println(topic);

    if (len >= sizeof(pendingCmd)) len = sizeof(pendingCmd) - 1;
    memcpy(pendingCmd, payload, len);
    pendingCmd[len] = '\0';
    cmdPending = true; // se ejecuta desde loop()
}

// ---------------------------------------------------------------------
// Sensores
// ---------------------------------------------------------------------
double getPressure() //Retorna la presion en mb, o NAN si falla
{
  char status;
  double T,P;

  status = pressure.startTemperature();
  if (status != 0)
  {
    // Esperar a que termine la medicion
    delay(status);

    status = pressure.getTemperature(T);
    if (status != 0)
    {
      // Parametro: oversampling 0..3 (3 = mayor resolucion, mas espera).
      status = pressure.startPressure(3);
      if (status != 0)
      {
        delay(status);

        status = pressure.getPressure(P,T);
        if (status != 0)
        {
          return(P);
        }
        else SerialMon.println("Error obteniendo informacion de presion\n");
      }
      else SerialMon.println("Error iniciando mediciones de presion\n");
    }
    else SerialMon.println("Error obteniendo mediciones de temperatura\n");
  }
  else SerialMon.println("Error iniciando mediciones de temperatura\n");
  return NAN;
}

bool readTemperature(double &t)
{
  char status = pressure.startTemperature();
  if (status == 0) return false;
  delay(status);
  return pressure.getTemperature(t) != 0;
}

// Maxbotix: paquete de 6 bytes "Rxxxx\r". Devuelve "d.ddd" (metros) en dist (>= 8 bytes).
// Devuelve false (y escribe "ERROR") si no llego un paquete valido.
bool SensorRead(char *dist){
    char tmp[8];

    for (int count = 0; count < 1000; count++)
    {
        digitalWrite(RANGING,HIGH);
        delayMicroseconds(200);
        digitalWrite(RANGING,LOW);

        int data = SerialMaxB.available();
        if (data == 6)
        {
            SerialMaxB.readBytes(tmp, 6);
            if (tmp[0] == 'R')
            {
                dist[0] = tmp[1];
                dist[1] = '.';
                dist[2] = tmp[2];
                dist[3] = tmp[3];
                dist[4] = tmp[4];
                dist[5] = '\0';
                return true;
            }
        }
        else if (data > 6)
        {
            // Llegaron mas bytes de los esperados (paquetes pegados): antes nunca
            // se vaciaba el buffer y se devolvia ERROR para siempre.
            while (SerialMaxB.available()) SerialMaxB.read();
        }
    }
    strcpy(dist, "ERROR");
    return false;
}

float batteryRead(char *bateria){
    uint16_t v = analogRead(ADC_PIN);
    float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (1100/ 1000.0);
    snprintf(bateria, 16, "%.2fV", battery_voltage);
    // Con USB conectado la lectura da 0 (el circuito de deteccion se desconecta).
    return battery_voltage;
}

void solarRead(char *solar) {
    uint32_t mv = analogReadMilliVolts(36) * 2;
    float voltage = mv / 1000.0;
    snprintf(solar, 16, "%.2fV", voltage);
}

void SensadoYenvio()
{
    int csq;
    char dist[8];
    char presion[16];
    char signal[8];
    char temperatura[16];
    char bateria[16];
    char solar[16];
    SerialMon.println("Lectura de datos");
    SerialMon.println("=====================");
    //Lectura de la presion
    pres = getPressure();
    SerialMon.print("Presion: ");
    SerialMon.print(pres);
    SerialMon.println(" mb ");
    delay(1000);
    //Lectura de temperatura
    bool tempOk = readTemperature(temp);
    if (!tempOk) temp = NAN;
    SerialMon.print("Grados: ");
    SerialMon.println(temp);
    delay(1000);
    //Lectura de distancia
    bool distOk = SensorRead(dist);
    SerialMon.print("Distancia: ");
    SerialMon.print(dist);
    SerialMon.println(" metros ");
    //Lectura de senial
    csq = modem.getSignalQuality();
    SerialMon.print("Senial: ");
    SerialMon.println(csq);
    SerialMon.println("=====================");
    //Estado de bateria
    float vbat = batteryRead(bateria);
    //Lectura de voltaje del panel solar
    solarRead(solar);
    //Conversion de datos
    itoa(csq,signal,10);
    //Envio de datos (los invalidos no se publican como si fueran validos)
    mqtt.publish(topicDistancia,dist); // "ERROR" si el sensor no respondio (como antes)
    if (!isnan(pres)) {
        snprintf(presion, sizeof(presion), "%f", pres);
        mqtt.publish(topicPresion,presion);
    }
    if (tempOk) {
        snprintf(temperatura, sizeof(temperatura), "%f", temp);
        mqtt.publish(topicTemperatura,temperatura);
    }
    mqtt.publish(topicSignal,signal);
    mqtt.publish(topicBateria,bateria);
    mqtt.publish(topicSolar,solar);
    mqtt.publish(topicFirmware,FIRMWARE_VERSION);
    publicarDatos(temp, pres, distOk ? atof(dist) : NAN, vbat);
}

void MQTTVerify(){
    // Make sure we're still registered on the network
  if (!modem.isNetworkConnected()) {
    SerialMon.println("Network disconnected");
    if (!modem.waitForNetwork(180000L, true)) {
      SerialMon.println(" fail");
      delay(10000);
      return;
    }
    if (modem.isNetworkConnected()) {
      SerialMon.println("Network re-connected");
    }
    }
    // and make sure GPRS/EPS is still connected
    if (!modem.isGprsConnected()) {
      SerialMon.println("GPRS disconnected!");
      SerialMon.print(F("Connecting to "));
      SerialMon.print(apn);
      if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
        SerialMon.println(" fail");
        delay(10000);
        return;
      }
      if (modem.isGprsConnected()) { SerialMon.println("GPRS reconnected"); }
    }

if (!mqtt.connected())
{
    SerialMon.println("====== MQTT disconnected ========");
    if (lastReconnectAttempt == 0 ||
        millis() - lastReconnectAttempt >= 10000UL)
    {
        lastReconnectAttempt = millis();

        if (mqttConnect())
        {
            SerialMon.println("====== MQTT Reconnected ========");
            lastReconnectAttempt = 0;
        }
    }
    delay(100);
}
}

// ---------------------------------------------------------------------
// Chequeo de version
// ---------------------------------------------------------------------
bool isNewerVersion(String nueva, String actual)
{
    int nMajor = 0, nMinor = 0, nPatch = 0;
    int aMajor = 0, aMinor = 0, aPatch = 0;

    // Si la version viene vacia o mal formada, NO actualizar
    // (antes quedaban variables sin inicializar -> OTA espuria).
    if (sscanf(nueva.c_str(), "%d.%d.%d", &nMajor, &nMinor, &nPatch) != 3)
        return false;
    if (sscanf(actual.c_str(), "%d.%d.%d", &aMajor, &aMinor, &aPatch) != 3)
        return false;

    if(nMajor > aMajor)
        return true;

    if(nMajor == aMajor && nMinor > aMinor)
        return true;

    if(nMajor == aMajor && nMinor == aMinor && nPatch > aPatch)
        return true;

    return false;
}

static void closeOtaConnection()
{
    http.stop();
    otaClient.stop();
    delay(100); // sino no se cierran sockets y se queda colgado el modem
}

static bool fetchVersionInfo(String& version, String& sha256)
{
    int err = http.get(OTA_PATH_VERSION);
    if (err != 0)
    {
        SerialMon.print("Error HTTP al pedir version: ");
        SerialMon.println(err);
        return false;
    }

    int httpCode = http.responseStatusCode();
    if (httpCode != 200)
    {
        SerialMon.print("Error HTTP: ");
        SerialMon.println(httpCode);
        return false;
    }

    String payload = http.responseBody();
    SerialMon.println("Respuesta:");
    SerialMon.println(payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error)
    {
        SerialMon.println("Error leyendo JSON");
        return false;
    }

    version = String(doc["version"] | "");
    sha256  = String(doc["sha256"] | "");
    return version.length() > 0;
}

// force = true: instala aunque la version no sea mas nueva (comando "OTA").
void checkForUpdate(bool force = false)
{
    SerialMon.println("Consultando actualizacion...");
    debugLog("Consultando actualizacion...");

    String versionNueva, sha256;
    bool ok = fetchVersionInfo(versionNueva, sha256);
    closeOtaConnection(); // el body ya se leyo completo: liberar el socket antes de seguir

    if (!ok) return;

    SerialMon.print("Version actual: ");
    SerialMon.println(FIRMWARE_VERSION);
    SerialMon.print("Version servidor: ");
    SerialMon.println(versionNueva);

    if (force || isNewerVersion(versionNueva, FIRMWARE_VERSION))
    {
        SerialMon.println("Hay una actualizacion!");
        debugLog("Hay una actualizacion! (%s)", versionNueva.c_str());
        downloadFirmware(sha256); // si sale bien reinicia y no vuelve
        closeOtaConnection();
    }
    else
    {
        SerialMon.println("Firmware actualizado.");
        debugLog("Firmware actualizado.");
    }
}

// ---------------------------------------------------------------------
// Comandos MQTT (se ejecutan desde loop, nunca dentro del callback)
// ---------------------------------------------------------------------
void processPendingCommand()
{
    if (!cmdPending) return;
    cmdPending = false;

    String comando(pendingCmd);
    comando.trim();
    SerialMon.print("Comando recibido: ");
    SerialMon.println(comando);

    if (comando == "OTA") {
        Serial.println("Iniciando OTA...");
        enviarCallMeBot();
        checkForUpdate(true);
    }
    else if (comando == "REBOOT") {
        Serial.println("Reiniciando...");
        debugLog("Reiniciando...");
        mqtt.loop();
        ESP.restart();
    }
    else if (comando.startsWith("SLEEP ")) {
        long minutos = comando.substring(6).toInt();
        if (minutos >= 1 && minutos <= 1440) {
            goToSleep((uint32_t)minutos);
        } else {
            debugLog("SLEEP invalido: %s", comando.c_str());
        }
    }
}

void setup() {
  pinMode(12,OUTPUT);
  pinMode(35, INPUT);
  InitUart();
  checkRollback();
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("Running partition: %s | Firmware %s\n", running->label, FIRMWARE_VERSION);

  InitSensors();
  analogReadResolution(12); // valido: 9..12 bits. batteryRead asume 4095
  snprintf(mqttClientId, sizeof(mqttClientId), "aquaman1-%08X", (unsigned int)(ESP.getEfuseMac() & 0xFFFFFFFFULL));

  // MQTT Broker setup
  mqtt.setServer(broker, 1883);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);

  if (!InitModem()) {
    goToSleep(SLEEP_MINUTES);
  }

  awakeStart = millis();
}

void loop() {
    static uint32_t lastPublish = 0;
    static bool     firstDone   = false;
    static bool     otaChecked  = false;

    mqtt.loop();
    processPendingCommand();

    // Publica al principio y despues cada PUBLISH_INTERVAL (antes: sin pausa,
    // ~20 ciclos de telemetria por despertar).
    if (!firstDone || millis() - lastPublish >= PUBLISH_INTERVAL)
    {
        MQTTVerify();
        if (mqtt.connected())
        {
            lastPublish = millis();
            digitalWrite(12,HIGH);
            mqtt.publish(topicState,"Awake");
            debugLog("voy a sensar y enviar datos");
            SensadoYenvio();
            debugLog("envie datos");
            digitalWrite(12,LOW);
            firstDone = true;

            if (!otaChecked)
            {
                otaChecked = true;
                debugLog("voy a checkear si hay actualizacion de firmware");
                checkForUpdate(false);
                mqtt.loop();
            }
        }
    }

    // --- Deep sleep ---
    // El ESP32 se apaga casi por completo y se despierta solo pasados
    // SLEEP_MINUTES minutos. Al despertar corre setup() entero.
    if (millis() - awakeStart >= AWAKE_INTERVAL)
    {
        goToSleep(SLEEP_MINUTES);
    }

    delay(50);
}
