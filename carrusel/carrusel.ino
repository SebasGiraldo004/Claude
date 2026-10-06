/*
 * Carrusel de 8 sectores (7 contenedores + 1 area de descarga)
 * Arduino UNO + driver DRV8825 + motor NEMA 16
 *
 * Ciclo:
 *   REPOSO -> MOVIENDO (lleva el contenedor N a descarga) -> ESPERANDO
 *   -> (flanco de subida en D2 o D3) -> REGRESANDO (mismo camino) -> REPOSO
 *
 * Posicion: el disco no tiene sensor de home, asi que la posicion se lleva
 * contando pasos y se guarda en EEPROM. Si se corta la luz con el motor en
 * movimiento, al arrancar se avisa de que la posicion no es fiable y hay que
 * recalibrar (ver comandos 'a', 'd', 'z').
 * Si mas adelante se agrega un sensor de home, poner USAR_SENSOR_HOME en 1.
 *
 * Comandos por Serial (115200, fin de linea cualquiera):
 *   1..7  llevar ese contenedor a la zona de descarga
 *   a / d mover el disco un poco adelante / atras (calibracion manual)
 *   z     fijar la posicion actual como inicio (sector 0 = descarga)
 *   h     buscar home con el sensor (solo si USAR_SENSOR_HOME = 1)
 *   ?     mostrar estado
 *
 * Requiere la libreria AccelStepper (Gestor de librerias del IDE).
 */

#include <AccelStepper.h>
#include <EEPROM.h>

// ---------------- Configuracion ----------------
const uint8_t PIN_STEP   = 4;
const uint8_t PIN_DIR    = 5;
const uint8_t PIN_ENABLE = 8;   // DRV8825 ENABLE es activo en LOW
const uint8_t PIN_INT0   = 2;   // interrupcion externa 0
const uint8_t PIN_INT1   = 3;   // interrupcion externa 1

const long PASOS_POR_VUELTA = 200;   // motor de 1.8 grados/paso
const long MICROPASOS       = 16;    // segun M0/M1/M2 del DRV8825
const uint8_t SECTORES      = 8;
const long PASOS_POR_SECTOR = PASOS_POR_VUELTA * MICROPASOS / SECTORES;  // 400

const float VELOCIDAD_MAX = 1600.0;  // micropasos/s
const float ACELERACION   = 1200.0;  // micropasos/s^2
const long  PASOS_AJUSTE  = MICROPASOS;  // paso de calibracion manual ('a'/'d')

const unsigned long ANTIRREBOTE_MS = 50;

// Sensor de home opcional (final de carrera, optico o Hall)
#define USAR_SENSOR_HOME 0
const uint8_t PIN_HOME      = 6;     // LOW = disco en home (con INPUT_PULLUP)
const float   VELOCIDAD_HOME = 400.0;

// ---------------- EEPROM ----------------
const int  EE_DIR    = 0;
const byte EE_FIRMA  = 0xC8;
struct EstadoGuardado {
  byte firma;
  byte enMovimiento;  // 1 si se corto la luz durante un movimiento
  long posicion;      // en micropasos
};

// ---------------- Estado ----------------
enum Estado { REPOSO, MOVIENDO, ESPERANDO, REGRESANDO };
Estado estado = REPOSO;
uint8_t contenedorActual = 0;
bool posicionFiable = true;

volatile bool interrupcionRecibida = false;
volatile unsigned long ultimaInterrupcion = 0;

AccelStepper motor(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

// ---------------- Interrupciones ----------------
void isrExterna() {
  unsigned long ahora = millis();
  if (ahora - ultimaInterrupcion >= ANTIRREBOTE_MS) {
    interrupcionRecibida = true;
  }
  ultimaInterrupcion = ahora;
}

// ---------------- EEPROM ----------------
void guardarEstado(bool enMovimiento) {
  EstadoGuardado e = { EE_FIRMA, (byte)enMovimiento, motor.currentPosition() };
  EEPROM.put(EE_DIR, e);  // put() solo escribe los bytes que cambian
}

void cargarEstado() {
  EstadoGuardado e;
  EEPROM.get(EE_DIR, e);
  if (e.firma != EE_FIRMA) {
    Serial.println(F("EEPROM vacia: se asume que el disco esta en inicio."));
    motor.setCurrentPosition(0);
    guardarEstado(false);
    return;
  }
  motor.setCurrentPosition(e.posicion);
  if (e.enMovimiento) {
    posicionFiable = false;
    Serial.println(F("AVISO: se perdio la energia durante un movimiento."));
    Serial.println(F("La posicion no es fiable: calibre con 'a'/'d' y 'z'."));
  }
}

// ---------------- Movimiento ----------------
void moverA(long destino) {
  guardarEstado(true);
  digitalWrite(PIN_ENABLE, LOW);
  motor.moveTo(destino);
}

bool irAContenedor(uint8_t n) {
  if (n < 1 || n >= SECTORES) {
    Serial.println(F("Contenedor invalido (1..7)."));
    return false;
  }
  if (estado != REPOSO) {
    Serial.println(F("Ocupado: espere a que el disco vuelva al inicio."));
    return false;
  }
  if (!posicionFiable) {
    Serial.println(F("Posicion no fiable: calibre primero con 'z'."));
    return false;
  }
  contenedorActual = n;
  estado = MOVIENDO;
  Serial.print(F("Llevando contenedor "));
  Serial.print(n);
  Serial.println(F(" a descarga..."));
  moverA(n * PASOS_POR_SECTOR);
  return true;
}

#if USAR_SENSOR_HOME
void buscarHome() {
  Serial.println(F("Buscando home..."));
  digitalWrite(PIN_ENABLE, LOW);
  motor.setSpeed(-VELOCIDAD_HOME);
  long limite = PASOS_POR_VUELTA * MICROPASOS + PASOS_POR_SECTOR;
  while (digitalRead(PIN_HOME) == HIGH && limite-- > 0) {
    motor.runSpeed();
  }
  if (digitalRead(PIN_HOME) == HIGH) {
    Serial.println(F("ERROR: no se encontro el sensor de home."));
    posicionFiable = false;
    return;
  }
  motor.setCurrentPosition(0);
  posicionFiable = true;
  guardarEstado(false);
  Serial.println(F("Home encontrado."));
}
#endif

// ---------------- Serial ----------------
void imprimirEstado() {
  static const char* const nombres[] = { "REPOSO", "MOVIENDO", "ESPERANDO", "REGRESANDO" };
  Serial.print(F("Estado: "));
  Serial.print(nombres[estado]);
  Serial.print(F(" | contenedor: "));
  Serial.print(contenedorActual);
  Serial.print(F(" | posicion: "));
  Serial.print(motor.currentPosition());
  Serial.print(F(" micropasos | fiable: "));
  Serial.println(posicionFiable ? F("si") : F("no"));
}

void leerSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c >= '1' && c <= '7') {
    irAContenedor(c - '0');
    return;
  }

  switch (c) {
    case 'a':
    case 'd':
      if (estado != REPOSO) { Serial.println(F("Solo en REPOSO.")); break; }
      digitalWrite(PIN_ENABLE, LOW);
      motor.runToNewPosition(motor.currentPosition() + (c == 'a' ? PASOS_AJUSTE : -PASOS_AJUSTE));
      break;
    case 'z':
      if (estado != REPOSO) { Serial.println(F("Solo en REPOSO.")); break; }
      motor.setCurrentPosition(0);
      posicionFiable = true;
      guardarEstado(false);
      Serial.println(F("Posicion actual fijada como inicio."));
      break;
#if USAR_SENSOR_HOME
    case 'h':
      if (estado == REPOSO) buscarHome();
      break;
#endif
    case '?':
      imprimirEstado();
      break;
  }
}

// ---------------- Programa ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENABLE, OUTPUT);
  digitalWrite(PIN_ENABLE, LOW);  // motor energizado: sostiene la posicion

  motor.setMaxSpeed(VELOCIDAD_MAX);
  motor.setAcceleration(ACELERACION);

  pinMode(PIN_INT0, INPUT);  // la senal externa debe tener su propio pull-down
  pinMode(PIN_INT1, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_INT0), isrExterna, RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_INT1), isrExterna, RISING);

#if USAR_SENSOR_HOME
  pinMode(PIN_HOME, INPUT_PULLUP);
  buscarHome();
#else
  cargarEstado();
#endif

  Serial.println(F("Carrusel listo. Envie 1..7 para descargar, ? para estado."));
}

void loop() {
  motor.run();
  leerSerial();

  switch (estado) {
    case REPOSO:
      break;

    case MOVIENDO:
      if (motor.distanceToGo() == 0) {
        guardarEstado(false);
        noInterrupts();
        interrupcionRecibida = false;  // ignorar senales previas a la llegada
        interrupts();
        estado = ESPERANDO;
        Serial.println(F("En descarga. Esperando interrupcion externa..."));
      }
      break;

    case ESPERANDO:
      if (interrupcionRecibida) {
        interrupcionRecibida = false;
        estado = REGRESANDO;
        Serial.println(F("Interrupcion recibida. Regresando al inicio..."));
        moverA(0);  // mismo camino, en sentido contrario
      }
      break;

    case REGRESANDO:
      if (motor.distanceToGo() == 0) {
        guardarEstado(false);
        contenedorActual = 0;
        estado = REPOSO;
        Serial.println(F("En inicio."));
      }
      break;
  }
}
