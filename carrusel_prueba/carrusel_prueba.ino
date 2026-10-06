/*
 * PRUEBA del carrusel (sin interrupciones y sin librerias)
 * Arduino UNO + DRV8825 + NEMA 16, conectando solo STEP y DIR.
 *
 * Secuencia:
 *   1. Avanza un sector: el contenedor 1 queda en descarga, espera 1 s.
 *   2. Avanza otro sector: contenedor 2, espera 1 s ... hasta el 7.
 *   3. Espera 30 s.
 *   4. Regresa por el mismo camino hasta el inicio y vuelve a empezar.
 *
 * Con M0, M1 y M2 en LOW el DRV8825 trabaja en paso completo:
 *   200 pasos por vuelta / 8 sectores = 25 pasos por sector.
 */

const uint8_t PIN_STEP = 4;
const uint8_t PIN_DIR  = 5;

const int PASOS_POR_VUELTA = 200;  // motor de 1.8 grados/paso, paso completo
const int SECTORES         = 8;
const int CONTENEDORES     = 7;
const int PASOS_POR_SECTOR = PASOS_POR_VUELTA / SECTORES;  // 25

const unsigned int RETARDO_PASO_US = 4000;  // 250 pasos/s; subirlo si el motor vibra sin girar
const unsigned long ESPERA_CONTENEDOR_MS = 1000;
const unsigned long ESPERA_FINAL_MS      = 30000;

void darPasos(int pasos, bool adelante) {
  digitalWrite(PIN_DIR, adelante ? HIGH : LOW);
  delayMicroseconds(5);  // tiempo de preparacion de DIR para el DRV8825
  for (int i = 0; i < pasos; i++) {
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(RETARDO_PASO_US);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  Serial.println(F("Prueba del carrusel: inicio en 2 s..."));
  delay(2000);
}

void loop() {
  for (int c = 1; c <= CONTENEDORES; c++) {
    darPasos(PASOS_POR_SECTOR, true);
    Serial.print(F("Contenedor "));
    Serial.print(c);
    Serial.println(F(" en descarga"));
    delay(ESPERA_CONTENEDOR_MS);
  }

  Serial.println(F("Fin de la secuencia, esperando 30 s..."));
  delay(ESPERA_FINAL_MS);

  Serial.println(F("Regresando al inicio por el mismo camino..."));
  darPasos(CONTENEDORES * PASOS_POR_SECTOR, false);
  delay(ESPERA_CONTENEDOR_MS);
}
