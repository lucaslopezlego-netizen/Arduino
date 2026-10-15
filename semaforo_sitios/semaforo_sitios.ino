// Semáforo de sitios caídos para ESP32
//
// Revisa periódicamente una lista de URLs (http o https):
//   - Todos arriba  -> LED verde encendido, rojo apagado.
//   - Alguno caído  -> LED rojo encendido, verde apagado y 3 pitidos.
//   - Conectando WiFi -> ambos LEDs parpadean.
// Si el que se cae es tu propio internet, no marca ningún sitio como caído:
// reconecta y vuelve a intentar.
//
// Las credenciales WiFi van en secrets.h (copia secrets.example.h).

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"

// ===================== CONFIGURACIÓN =====================

// Lista de sitios a revisar. Agrega o quita líneas aquí.
const char* SITIOS[] = {
  "https://www.google.com",
  "https://github.com",
};

// Cada cuánto se revisan los sitios (5 minutos).
const unsigned long INTERVALO_REVISION_MS = 5UL * 60UL * 1000UL;

// Timeout de cada petición HTTP.
const uint16_t TIMEOUT_HTTP_MS = 10000;

// Espera antes de reintentar un sitio que falló.
const unsigned long ESPERA_REINTENTO_MS = 3000;

// Si se cayó tu internet, cada cuánto se vuelve a intentar el ciclo.
const unsigned long REINTENTO_SIN_INTERNET_MS = 30000;

// Máximo de redirecciones a seguir por sitio.
const int MAX_REDIRECCIONES = 10;

// URLs muy confiables que se usan solo para saber si TU internet funciona.
const char* SONDAS_INTERNET[] = {
  "http://connectivitycheck.gstatic.com/generate_204",
  "http://www.msftconnecttest.com/connecttest.txt",
};

// Pines
const int PIN_LED_VERDE = 25;
const int PIN_LED_ROJO  = 26;
const int PIN_BUZZER    = 27;

// =========================================================

const size_t NUM_SITIOS = sizeof(SITIOS) / sizeof(SITIOS[0]);
const size_t NUM_SONDAS = sizeof(SONDAS_INTERNET) / sizeof(SONDAS_INTERNET[0]);
const int ERROR_DEMASIADAS_REDIRECCIONES = -100;
const int ERROR_URL_INVALIDA = -101;

enum Estado { ESTADO_DESCONOCIDO, ESTADO_TODOS_ARRIBA, ESTADO_ALGUNO_CAIDO };
Estado estadoActual = ESTADO_DESCONOCIDO;

unsigned long proximaRevision = 0;

// ---------- LEDs y buzzer ----------

void ponerLeds(bool verde, bool rojo) {
  digitalWrite(PIN_LED_VERDE, verde ? HIGH : LOW);
  digitalWrite(PIN_LED_ROJO, rojo ? HIGH : LOW);
}

void aplicarEstado() {
  switch (estadoActual) {
    case ESTADO_TODOS_ARRIBA: ponerLeds(true, false); break;
    case ESTADO_ALGUNO_CAIDO: ponerLeds(false, true); break;
    default:                  ponerLeds(false, false); break;
  }
}

void pitar(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(200);
    digitalWrite(PIN_BUZZER, LOW);
    if (i < veces - 1) delay(200);
  }
}

// ---------- WiFi ----------

// Conecta (o reconecta) al WiFi. Bloquea hasta lograrlo, con ambos LEDs parpadeando.
void asegurarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Conectando a WiFi \"%s\"", WIFI_SSID);
  bool encendido = false;
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
      encendido = !encendido;
      ponerLeds(encendido, encendido);
      Serial.print('.');
      delay(250);
    }
  }
  Serial.printf("\nWiFi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  aplicarEstado();
}

// ---------- HTTP ----------

bool esCodigoArriba(int codigo) {
  return codigo >= 200 && codigo <= 399;
}

// Resuelve el valor de un header Location (absoluto o relativo) contra la URL actual.
String resolverUrl(const String& base, const String& location) {
  if (location.startsWith("http://") || location.startsWith("https://")) return location;

  int finEsquema = base.indexOf("://");
  String esquema = base.substring(0, finEsquema);
  if (location.startsWith("//")) return esquema + ":" + location;

  int inicioRuta = base.indexOf('/', finEsquema + 3);
  String origen = inicioRuta < 0 ? base : base.substring(0, inicioRuta);
  if (location.startsWith("/")) return origen + location;

  // Ruta relativa: se cuelga del "directorio" de la URL actual.
  String ruta = inicioRuta < 0 ? "/" : base.substring(inicioRuta);
  int finQuery = ruta.indexOf('?');
  if (finQuery >= 0) ruta = ruta.substring(0, finQuery);
  return origen + ruta.substring(0, ruta.lastIndexOf('/') + 1) + location;
}

// Hace GET a la URL siguiendo redirecciones (también entre http y https).
// Devuelve el código HTTP final, o un número negativo si hubo error de conexión.
// Las redirecciones se siguen a mano porque HTTPClient no puede cambiar de http a https.
int obtenerCodigo(String url) {
  for (int salto = 0; salto <= MAX_REDIRECCIONES; salto++) {
    WiFiClient clientePlano;
    WiFiClientSecure clienteSeguro;
    HTTPClient http;

    http.setConnectTimeout(TIMEOUT_HTTP_MS);
    http.setTimeout(TIMEOUT_HTTP_MS);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("Mozilla/5.0 (ESP32 semaforo-sitios)");
    const char* headers[] = {"Location"};
    http.collectHeaders(headers, 1);

    bool iniciado;
    if (url.startsWith("https://")) {
      // Solo verificamos disponibilidad, no la identidad del servidor.
      clienteSeguro.setInsecure();
      clienteSeguro.setHandshakeTimeout(TIMEOUT_HTTP_MS / 1000);
      iniciado = http.begin(clienteSeguro, url);
    } else {
      iniciado = http.begin(clientePlano, url);
    }
    if (!iniciado) return ERROR_URL_INVALIDA;

    int codigo = http.GET();
    String location = http.header("Location");
    http.end();

    if (codigo >= 300 && codigo <= 399 && location.length() > 0) {
      url = resolverUrl(url, location);
      continue;
    }
    return codigo;
  }
  return ERROR_DEMASIADAS_REDIRECCIONES;
}

String describirCodigo(int codigo) {
  if (codigo == ERROR_DEMASIADAS_REDIRECCIONES) return "demasiadas redirecciones";
  if (codigo == ERROR_URL_INVALIDA) return "URL invalida";
  if (codigo < 0) return HTTPClient::errorToString(codigo);
  return String("HTTP ") + codigo;
}

// ¿Funciona el internet de esta red? Se pregunta a sitios muy confiables.
bool hayInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  for (size_t i = 0; i < NUM_SONDAS; i++) {
    if (esCodigoArriba(obtenerCodigo(SONDAS_INTERNET[i]))) return true;
  }
  return false;
}

// ---------- Ciclo de revisión ----------

// Revisa todos los sitios. Devuelve false si el ciclo se canceló por falta de internet propio.
bool revisarSitios() {
  asegurarWiFi();
  Serial.printf("\n=== Revisando %u sitio(s) ===\n", (unsigned)NUM_SITIOS);

  int caidos = 0;
  for (size_t i = 0; i < NUM_SITIOS; i++) {
    const char* url = SITIOS[i];
    unsigned long inicio = millis();
    int codigo = obtenerCodigo(url);
    bool reintentado = false;

    if (!esCodigoArriba(codigo)) {
      Serial.printf("  ...  %s fallo (%s), reintentando en 3 s\n", url, describirCodigo(codigo).c_str());
      delay(ESPERA_REINTENTO_MS);
      inicio = millis();
      codigo = obtenerCodigo(url);
      reintentado = true;
    }

    if (!esCodigoArriba(codigo) && !hayInternet()) {
      Serial.println("!!! Sin internet propio: no se marca ningun sitio como caido. Reconectando...");
      WiFi.disconnect();
      return false;
    }

    bool arriba = esCodigoArriba(codigo);
    if (!arriba) caidos++;
    Serial.printf("  [%s] %-24s %s (%lu ms%s)\n",
                  arriba ? "ARRIBA" : "CAIDO ",
                  describirCodigo(codigo).c_str(), url,
                  millis() - inicio, reintentado ? ", tras reintento" : "");
  }

  if (caidos == 0) {
    Serial.println("Resultado: todos los sitios arriba -> LED VERDE");
    estadoActual = ESTADO_TODOS_ARRIBA;
    aplicarEstado();
  } else {
    Serial.printf("Resultado: %d sitio(s) caido(s) -> LED ROJO + 3 pitidos\n", caidos);
    estadoActual = ESTADO_ALGUNO_CAIDO;
    aplicarEstado();
    pitar(3);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nSemaforo de sitios caidos");

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  ponerLeds(false, false);
  digitalWrite(PIN_BUZZER, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  asegurarWiFi();
}

void loop() {
  // Si el WiFi se cae entre revisiones, reconectar (con LEDs parpadeando).
  asegurarWiFi();

  if ((long)(millis() - proximaRevision) >= 0) {
    bool completo = revisarSitios();
    proximaRevision = millis() + (completo ? INTERVALO_REVISION_MS : REINTENTO_SIN_INTERNET_MS);
    if (completo) {
      Serial.printf("Proxima revision en %lu s\n", INTERVALO_REVISION_MS / 1000);
    }
  }
  delay(100);
}
