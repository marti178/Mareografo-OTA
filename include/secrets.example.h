// Copiar a include/secrets.h y completar. secrets.h esta en .gitignore: NO subirlo.
#pragma once

// APN del operador
#define GPRS_APN  "apn.del.operador"
#define GPRS_USER ""
#define GPRS_PASS ""

// Broker MQTT
#define MQTT_BROKER "broker.ejemplo.com"
#define MQTT_USER   "usuario"
#define MQTT_PASS   "clave"

// CallMeBot (opcional: si no se define CALLMEBOT_APIKEY, no se envia WhatsApp)
// Telefono con codigo de pais, SIN el '+'
#define CALLMEBOT_PHONE  "5491100000000"
#define CALLMEBOT_APIKEY "000000"
