// =====================================================================
//  DISPENSADOR DE PASTILLAS - Capstone 2
//  Microcontrolador : ESP32-S3
//  Motor            : NEMA 17 (200 pasos/vuelta, 1.8 grados/paso)
//  Driver           : DRV8825
//  Transmision      : directa (motor acoplado al cilindro)
//
//  Cilindro de 8 divisiones (45 grados cada una):
//      - Zona de reposo (posicion 0, queda sobre la descarga en el inicio)
//      - Contenedor 1 a la izquierda del reposo ... Contenedor 7 a la derecha
//
//  PASOS A SEGUIR
//    1. Configuracion MICROPROCESADOR ESP32          <-- ESTAMOS AQUI
//         GENERACION DE VARIABLES + CONFIGURACION DE PUERTOS + INTERRUPCIONES
//    2. Creacion de funciones
//         CARGA - DESCARGA - MOVIMIENTO DE CONTENEDORES
//    3. Creacion de main
//
//  Requiere: Arduino IDE + paquete "esp32" de Espressif version 3.x
// =====================================================================


// =====================================================================
//  1.1  CONFIGURACION DE PUERTOS (pines del ESP32-S3 -> DRV8825)
// =====================================================================
// Se evitan los pines de arranque (0, 3, 45, 46), USB (19, 20)
// y los de memoria flash/PSRAM (26-37).
const uint8_t PIN_STEP   = 4;   // DRV8825 STEP  : un pulso = un (micro)paso
const uint8_t PIN_DIR    = 5;   // DRV8825 DIR   : sentido de giro
const uint8_t PIN_ENABLE = 6;   // DRV8825 EN    : activo en BAJO (LOW = motor energizado)
const uint8_t PIN_M0     = 7;   // DRV8825 M0  \.
const uint8_t PIN_M1     = 15;  // DRV8825 M1   |-> seleccion de micropasos
const uint8_t PIN_M2     = 16;  // DRV8825 M2  /
// Nota: en el DRV8825 los pines RESET y SLEEP deben ir a 3.3 V (puenteados).


// =====================================================================
//  1.2  GENERACION DE VARIABLES
// =====================================================================

// ---- Constantes mecanicas ----
const int PASOS_POR_VUELTA   = 200;  // NEMA 17 a paso completo
const int MICROPASOS         = 16;   // 1/16 configurado con M0, M1, M2
const int DIVISIONES         = 8;    // 7 contenedores + zona de reposo
const int NUM_CONTENEDORES   = 7;

const long MICROPASOS_POR_VUELTA    = (long)PASOS_POR_VUELTA * MICROPASOS;      // 3200
const long MICROPASOS_POR_DIVISION  = MICROPASOS_POR_VUELTA / DIVISIONES;       // 400

const int POSICION_REPOSO = 0;       // division 0 = zona de reposo

// ---- Sentido de giro ----
// Si el motor gira al reves de lo esperado, intercambiar estos valores.
const uint8_t SENTIDO_HORARIO     = HIGH;
const uint8_t SENTIDO_ANTIHORARIO = LOW;

// ---- Velocidad ----
// El timer interrumpe cada PERIODO_MEDIO_PASO_US microsegundos y en cada
// interrupcion cambia el estado del pin STEP (alto -> bajo -> alto ...).
// Un micropaso completo = 2 interrupciones.
//   500 us -> 1000 micropasos/s -> 0.4 s por division (~19 RPM)
const uint32_t FRECUENCIA_TIMER_HZ     = 1000000;  // 1 tick = 1 us
const uint32_t PERIODO_MEDIO_PASO_US   = 500;

// ---- Tiempos ----
const unsigned long TIEMPO_DESCARGA_MS = 2000;     // 2 s sobre la descarga

// ---- Variables compartidas con la interrupcion (volatile) ----
volatile long  micropasosRestantes = 0;     // pasos que faltan del movimiento actual
volatile long  posicionActual      = 0;     // posicion absoluta en micropasos
volatile int   direccionActual     = 1;     // +1 horario, -1 antihorario
volatile bool  motorEnMovimiento   = false; // true mientras el timer genera pulsos
volatile bool  nivelPinStep        = false; // estado actual del pin STEP

// Proteccion para leer/escribir variables compartidas con la ISR
portMUX_TYPE muxMotor = portMUX_INITIALIZER_UNLOCKED;

// Puntero al timer de hardware
hw_timer_t *timerMotor = NULL;

// ---- Cola de pedidos (varios contenedores por orden serial) ----
const int TAMANO_COLA = 16;
int colaContenedores[TAMANO_COLA];
int cantidadEnCola = 0;


// =====================================================================
//  1.3  INTERRUPCIONES
// =====================================================================
// Rutina de interrupcion del timer: genera los pulsos STEP sin bloquear
// el programa principal. Debe ser corta y estar en IRAM.
void IRAM_ATTR isrTimerMotor() {
  portENTER_CRITICAL_ISR(&muxMotor);

  if (micropasosRestantes > 0) {
    nivelPinStep = !nivelPinStep;
    digitalWrite(PIN_STEP, nivelPinStep);

    // El DRV8825 avanza en el flanco de subida; contamos al bajar el pulso
    if (!nivelPinStep) {
      micropasosRestantes--;
      posicionActual += direccionActual;
    }
  } else {
    // Movimiento terminado
    nivelPinStep = false;
    digitalWrite(PIN_STEP, LOW);
    motorEnMovimiento = false;
  }

  portEXIT_CRITICAL_ISR(&muxMotor);
}


// =====================================================================
//  CONFIGURACION (se ejecuta una vez al encender)
// =====================================================================
void configurarPuertos() {
  pinMode(PIN_STEP,   OUTPUT);
  pinMode(PIN_DIR,    OUTPUT);
  pinMode(PIN_ENABLE, OUTPUT);
  pinMode(PIN_M0,     OUTPUT);
  pinMode(PIN_M1,     OUTPUT);
  pinMode(PIN_M2,     OUTPUT);

  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_DIR,  SENTIDO_HORARIO);

  // Micropasos 1/16 en el DRV8825:  M0 = LOW, M1 = LOW, M2 = HIGH
  digitalWrite(PIN_M0, LOW);
  digitalWrite(PIN_M1, LOW);
  digitalWrite(PIN_M2, HIGH);

  // Motor energizado: mantiene el torque y la posicion del cilindro
  digitalWrite(PIN_ENABLE, LOW);
}

void configurarInterrupciones() {
  timerMotor = timerBegin(FRECUENCIA_TIMER_HZ);           // timer a 1 MHz
  timerAttachInterrupt(timerMotor, &isrTimerMotor);       // asocia la ISR
  timerAlarm(timerMotor, PERIODO_MEDIO_PASO_US, true, 0); // repetitiva
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  configurarPuertos();
  configurarInterrupciones();

  // Sin sensor de home: al encender se asume que la ZONA DE REPOSO
  // esta alineada manualmente sobre el compartimiento de descarga.
  posicionActual = (long)POSICION_REPOSO * MICROPASOS_POR_DIVISION;

  Serial.println("=== Dispensador de pastillas ===");
  Serial.printf("Micropasos por vuelta   : %ld\n", MICROPASOS_POR_VUELTA);
  Serial.printf("Micropasos por division : %ld\n", MICROPASOS_POR_DIVISION);
  Serial.println("Configuracion lista. Posicion inicial = ZONA DE REPOSO");
}


// =====================================================================
//  2. FUNCIONES  (CARGA - DESCARGA - MOVIMIENTO)  -> siguiente paso
// =====================================================================


// =====================================================================
//  3. MAIN  -> siguiente paso
// =====================================================================
void loop() {
}
