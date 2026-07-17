/**************************************************************
* Proyect: Mareografo V1
* Author: Cronon Tecnologia
* Date: 9/02/2023
* Proyect Manager: Fernando Agostino
* Proyect Leader: Martin Marotta
 **************************************************************/
//Define Version of TinyGSM
#define LINK_BIN "https://marti178.github.io/Mareografo-OTA/firmware.bin"
#define LINK_VERSION "https://marti178.github.io/Mareografo-OTA/version.json"
#define FIRMWARE_VERSION "1.0.4"
// Select your modem:
#define TINY_GSM_MODEM_SIM7070 true
// Set serial 
#define SerialMon Serial
#define SerialMaxB Serial1
#define SerialAT Serial2
#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false

// Your GPRS credentials, if any
const char apn[]      = "igprs.claro.com.ar";
const char gprsUser[] = "clarogrps";
const char gprsPass[] = "clarogprs999";

// MQTT details
const char* broker = "mqtt.cronon.com.ar";
const char* topicDistancia      = "Aquaman1/Distancia(m)";
const char* topicInit      = "Aquaman1/init";
const char* topicTemperatura = "Aquaman1/Temperatura";
const char* topicPresion = "Aquaman1/Presion(hPa)";
const char* topicSignal = "Aquaman1/Signal";
const char* topicBateria = "Aquaman1/Bateria(mA)";
const char* topicAquaman1 = "Aquaman1/Aquaman1";

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Arduino.h>
#include<SFE_BMP180.h>

#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>

#include <Update.h>
#include <esp_ota_ops.h>

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
TinyGsmClient client(modem,0);
PubSubClient  mqtt(client);
TinyGsmClient test(modem, 1);

TinyGsmClientSecure otaClient(modem,2);
    HttpClient http(
        otaClient,
        "marti178.github.io",
        443
    );

#define MODEM_POWER_KEY 4
#define DATAIN 32
#define RANGING 33

uint32_t lastReconnectAttempt = 0;
SFE_BMP180 pressure;
double temp;
double pres;

void debugLog(const char* format, ...)
{
    char buffer[256];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    SerialMon.println(buffer);

    if (mqtt.connected())
        mqtt.publish("mareografo/debug", buffer);
}

bool downloadFirmware()
{
    SerialMon.println("Descargando firmware...");

    http.get(LINK_BIN);

    int statusCode = http.responseStatusCode();

    if (statusCode != 200)
    {
        SerialMon.print("HTTP Error: ");
        SerialMon.println(statusCode);
        http.stop();
        return false;
    }

    int contentLength = http.contentLength();

    if (contentLength <= 0)
    {
        SerialMon.println("Content-Length inválido");
        http.stop();
        return false;
    }

    SerialMon.print("Firmware: ");
    SerialMon.print(contentLength);
    SerialMon.println(" bytes");
    mqtt.disconnect();
    client.stop();      // el cliente MQTT
    SerialMon.println("===== INFO RED =====");
    
    SerialMon.print("Operador: ");
    SerialMon.println(modem.getOperator());

    SerialMon.print("Señal (CSQ): ");
    SerialMon.println(modem.getSignalQuality());

    SerialMon.print("Modem: ");
    SerialMon.println(modem.getModemInfo());

    SerialMon.print("Conectado a red: ");
    SerialMon.println(modem.isNetworkConnected() ? "SI" : "NO");

    SerialMon.print("GPRS conectado: ");
    SerialMon.println(modem.isGprsConnected() ? "SI" : "NO");

    SerialMon.print("IP: ");
    SerialMon.println(modem.localIP());

    SerialMon.println("====================");
    SerialMon.println("===== INFO CELDA =====");

    modem.sendAT("+CPSI?");
    String resp;
    modem.waitResponse(5000, resp);

    SerialMon.println(resp);
    SerialMon.println("======================");

    if (!Update.begin(contentLength))
    {
        SerialMon.println("No hay espacio suficiente para OTA");
        http.stop();
        return false;
    }

    uint8_t buffer[1512];

    size_t written = 0;
    int lastPercent = -1;


    uint32_t otaStartTime = millis();

    uint32_t lastDataTime = millis();

    while (written < contentLength)
    {
        int available = http.available();

        if (available > 0)
        {
            int toRead = min((int)sizeof(buffer), available);

            uint32_t tRead = millis();
            int len = http.read(buffer, toRead);

            if (len > 0)
            {
                lastDataTime = millis();

                size_t writtenFlash = Update.write(buffer, len);

                if (writtenFlash != (size_t)len)
                {
                    SerialMon.println("ERROR escribiendo flash");
                    Update.abort();
                    http.stop();
                    return false;
                }

                written += len;

                int percent = (written * 100) / contentLength;

                if (percent != lastPercent)
                {
                    lastPercent = percent;

                    float speedKB = (written / 1024.0) /
                                    ((millis() - otaStartTime) / 1000.0);

                    SerialMon.printf(
                        "OTA %d%% | %d/%d bytes | %.2f KB/s\n",
                        percent,
                        written,
                        contentLength,
                        speedKB
                    );
                    SerialMon.println(" ");
                    debugLog(
                    "OTA %d%% | %d/%d bytes | %.2f KB/s",
                    percent,
                    written,
                    contentLength,
                    speedKB
                    );
                }

            }
        }
        else
        {
            // Si pasan 30 segundos sin recibir nada, abortar
            if (millis() - lastDataTime > 300000)
            {
                SerialMon.println("TIMEOUT: 3000 segundos sin datos");

                Update.abort();
                http.stop();

                return false;
            }
            // Esperar un poco más entre consultas evita saturar el canal AT
            // del módem durante las pausas reales de red.
            delay(100);
            continue;
        }

        delay(1);
    }

    /*while (written < contentLength)
    {
        int available = http.available();

        if (available)
        {
            int len = http.readBytes(buffer, min((int)sizeof(buffer), available));
            Serial.printf("len=%d\n", len);
            if (len > 0)
            {
                if (Update.write(buffer, len) != (size_t)len)
                {
                    SerialMon.println("Error escribiendo flash");
                    Update.abort();
                    http.stop();
                    return false;
                }

                written += len;

                int percent = (written * 100) / contentLength;

                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    SerialMon.printf("OTA %d%%\n", percent);
                }
            }
        }

        delay(1);
    }*/

    http.stop();

    if (!Update.end())
    {
        SerialMon.print("Update Error: ");
        SerialMon.println(Update.errorString());
        return false;
    }

    if (!Update.isFinished())
    {
        SerialMon.println("Firmware incompleto");
        return false;
    }

    SerialMon.println("OTA finalizada correctamente");
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
}

void InitUart(void){
  //Seteo de modo de pines
  pinMode(DATAIN,INPUT); //Pin 32 entrada del Maxbotix
  pinMode(RANGING,OUTPUT); //Pin 33 salida para marcar comienzo de lectura del Maxbotix

  // Set console baud rate
  SerialMon.begin(115200);
  delay(1000);
  SerialAT.begin(921600, SERIAL_8N1, 26, 27);
  delay(1000);
  SerialMaxB.begin(9600,SERIAL_8N1,DATAIN);
  delay(1000);
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

void InitModem(void){
  
  //powerOnModem();
  // Restart takes quite some time
  // To skip it, call init() instead of restart()
  SerialMon.println("Initializing modem... ");
  modem.restart();
  //modem.init();
  delay(6000); 
  SerialMon.println("Configurando modem... ");
  modem.setPreferredMode(1); //Mode NB-Iot lo 
  delay(3000);
  // Fijar baudrate (sin autobaud) — lo repetimos en cada boot, no dependemos de &W
  /*modem.sendAT("+IPR=921600");
  modem.waitResponse();
  delay(200);
  // Recién ahora subimos el UART local
  SerialAT.updateBaudRate(921600);
  delay(500);*/
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

      if (intentos >= 2)
      {
          SerialMon.println("Reiniciando el módem...");
          modem.restart();
          intentos = 0;
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
      return;
    }
    SerialMon.println(" success");
    if (modem.isGprsConnected()) { SerialMon.println("GPRS connected"); }
  #endif

  return;
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  SerialMon.print("Message arrived [");
  SerialMon.print(topic);
  SerialMon.print("]: ");
  SerialMon.write(payload, len);
  SerialMon.println();

  // Only proceed if incoming message's topic matches
  if (String(topic) == topicAquaman1) {
    
  }
}

boolean mqttConnect() {
  SerialMon.print("Connecting to ");
  SerialMon.print(broker);
  boolean status = mqtt.connect(broker, "aquaman1", "P0s31d0n1");

  if (status == false) {
    SerialMon.println(" fail");
    SerialMon.print("MQTT state: ");
    SerialMon.println(mqtt.state());
    SerialMon.println("Probando TCP...");
    
  return false;
  }

  SerialMon.println(" success");
  mqtt.publish(topicInit, "Aquaman1 started");
  mqtt.subscribe(topicAquaman1);
  return mqtt.connected();
}

double getPressure() //Retorna la presion en mb
{
  char status;
  double T,P,p0,a;

  status = pressure.startTemperature();
  if (status != 0)
  {
    // Wait for the measurement to complete:

    delay(status);

    // Retrieve the completed temperature measurement:
    // Note that the measurement is stored in the variable T.
    // Use '&T' to provide the address of T to the function.
    // Function returns 1 if successful, 0 if failure.

    status = pressure.getTemperature(T);
    if (status != 0)
    {
      // Start a pressure measurement:
      // The parameter is the oversampling setting, from 0 to 3 (highest res, longest wait).
      // If request is successful, the number of ms to wait is returned.
      // If request is unsuccessful, 0 is returned.

      status = pressure.startPressure(3);
      if (status != 0)
      {
        // Wait for the measurement to complete:
        delay(status);

        // Retrieve the completed pressure measurement:
        // Note that the measurement is stored in the variable P.
        // Use '&P' to provide the address of P.
        // Note also that the function requires the previous temperature measurement (T).
        // (If temperature is stable, you can do one temperature measurement for a number of pressure measurements.)
        // Function returns 1 if successful, 0 if failure.

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
  return 1;
}

void SensorRead(char *dist){

    /* Funcion que devuelve ERROR si en el puerto serie no recibio la suficiente cantidad de datos 
    establecida (6) para leer, dado que todavia no llego el paquete completo de los
    datos del Sensor o llego con errores (sin R al comienzo)

    Recibe Char* de 6 lugares.

    */
    
    char * distancia=NULL;
    int data=0;
    int count=0;
    int flag=1;

    while (flag==1 and count<1000)
    {
    
    digitalWrite(RANGING,HIGH);
    delayMicroseconds(200);
    digitalWrite(RANGING,LOW);
    data=SerialMaxB.available();
    if (data==6)
    {
    distancia=(char *)malloc(data*sizeof(char));
    SerialMaxB.read(distancia,data);
    if ((*distancia=='R')){
    *(distancia+(data*sizeof(char)-1))='\0';
    *(distancia)=*(distancia+1);
    *(distancia+1*sizeof(char))='.';
    strcpy(dist,distancia);
    free(distancia);
        flag=0;
        return ;
    }
    else{
        distancia=(char *)malloc(data*sizeof(char));
        SerialMaxB.read(distancia,data);
        free(distancia);
        flag=1;
        count++;
    }
    }else{
        flag=1;
        count++; //
    }
    }
    strcpy(dist,"ERROR");
    
    } 

void SensadoYenvio()
{
    int csq;
    double a;
    char dist[6];
    char presion[16];
    char signal[6];
    char temperatura[16];
    char bateria[10];
    int bat;
    int Time=0;
    SerialMon.println("Lectura de datos");
    SerialMon.println("=====================");
    //Lectura de la presion
    pres= getPressure();
    SerialMon.print("Presion: ");
    SerialMon.print(pres);
    SerialMon.println(" mb ");  
    delay(1000);
    //Lectura de temperatura
    Time=pressure.startTemperature();
    delay(Time);
    pressure.getTemperature(temp);
    SerialMon.print("Grados: ");
    SerialMon.println(temp);
        delay(1000);
    //Lectura de distancia
    SensorRead(dist);
    SerialMon.print("Distancia: ");
    SerialMon.print(dist);
    SerialMon.println(" metros "); 
    //Lectura de senial
    csq = modem.getSignalQuality();
    SerialMon.print("Senial: ");
    SerialMon.println(csq);
    SerialMon.println("=====================");
    //Listo estado de bateria
    bat=analogRead(35);
    bat=bat*2;
    //Conversion de datos
    sprintf(presion,"%f",pres);
    sprintf(temperatura,"%f",temp);
    itoa(csq,signal,10);
    itoa(bat,bateria,10);
    //Envio de datos
    mqtt.publish(topicDistancia,dist);
    mqtt.publish(topicPresion,presion);
    mqtt.publish(topicTemperatura,temperatura);
    mqtt.publish(topicSignal,signal);
    mqtt.publish(topicBateria,bateria);

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
    return;

}

}

bool isNewerVersion(String nueva, String actual)
{
    int nMajor, nMinor, nPatch;
    int aMajor, aMinor, aPatch;

    sscanf(nueva.c_str(), "%d.%d.%d", &nMajor, &nMinor, &nPatch);
    sscanf(actual.c_str(), "%d.%d.%d", &aMajor, &aMinor, &aPatch);


    if(nMajor > aMajor)
        return true;

    if(nMajor == aMajor && nMinor > aMinor)
        return true;

    if(nMajor == aMajor && nMinor == aMinor && nPatch > aPatch)
        return true;

    return false;
}

void checkForUpdate()
{
    //TinyGsmClientSecure otaClient(modem);

    SerialMon.println("Consultando actualización...");

    http.get(LINK_VERSION);


    int httpCode = http.responseStatusCode();


    if (httpCode == 200)
    {
        String payload = http.responseBody();

        SerialMon.println("Respuesta:");
        SerialMon.println(payload);


        JsonDocument doc;

        DeserializationError error = deserializeJson(doc, payload);

        if(error)
        {
            SerialMon.println("Error leyendo JSON");
            return;
        }


        String versionNueva = doc["version"];


        SerialMon.print("Version actual: ");
        SerialMon.println(FIRMWARE_VERSION);

        SerialMon.print("Version servidor: ");
        SerialMon.println(versionNueva);


        if(isNewerVersion(versionNueva, FIRMWARE_VERSION))
        {
            SerialMon.println("Hay una actualización!");
           downloadFirmware();

        }
        else
        {
            SerialMon.println("Firmware actualizado.");
        }
    }
    else
    {
        SerialMon.print("Error HTTP: ");
        SerialMon.println(httpCode);
    }
    SerialMon.print("Cerrando conexión HTTP...");
    http.stop();       // si existe en tu versión
    otaClient.stop();  // este seguro existe
    delay(10000); // sino no se cierran sockets y se queda colgado el modem
}


void setup() {
  pinMode(12,OUTPUT);
  //pinMode(35, INPUT);
  //powerOnModem();
  InitUart();
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("Running partition: %s\n", running->label);

  InitSensors();
  InitModem();
  
  // MQTT Broker setup
  mqtt.setServer(broker, 1883);
  mqtt.setCallback(mqttCallback);
  

}

void loop() {
    SerialMon.println("empezando LOOP");
    digitalWrite(12,HIGH);
    MQTTVerify();
    debugLog("Firmware version: %s", FIRMWARE_VERSION);
    mqtt.publish("mareografo/debug", "voy a sensar y enviar datos");
    SensadoYenvio();
    mqtt.publish("mareografo/debug", "envié datos");
    digitalWrite(12,LOW);
    mqtt.publish("mareografo/debug", "voy a checkear si hay actualizacion de firmware");
    checkForUpdate();

  mqtt.loop();
}
