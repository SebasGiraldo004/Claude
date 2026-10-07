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
//    1. Configuracion MICROPROCESADOR ESP32          (listo)
//         GENERACION DE VARIABLES + CONFIGURACION DE PUERTOS + INTERRUPCIONES
//    2. Creacion de funciones                        (listo)
//         CARGA - DESCARGA - MOVIMIENTO DE CONTENEDORES
//    3. Creacion de main                             (listo)
//
//  Requiere: Arduino IDE + paquete "esp32" de Espressif (version 2.x o 3.x)
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
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Paquete esp32 version 3.x
  timerMotor = timerBegin(FRECUENCIA_TIMER_HZ);           // timer a 1 MHz
  timerAttachInterrupt(timerMotor, &isrTimerMotor);       // asocia la ISR
  timerAlarm(timerMotor, PERIODO_MEDIO_PASO_US, true, 0); // repetitiva
#else
  // Paquete esp32 version 2.x (reloj APB de 80 MHz / 80 = 1 MHz)
  timerMotor = timerBegin(0, 80, true);
  timerAttachInterrupt(timerMotor, &isrTimerMotor, true);
  timerAlarmWrite(timerMotor, PERIODO_MEDIO_PASO_US, true);
  timerAlarmEnable(timerMotor);
#endif
}

void mostrarAyuda();  // definida en la seccion 3

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
  mostrarAyuda();
}


// =====================================================================
//  2. FUNCIONES
// =====================================================================
//  Convencion de posiciones (en divisiones, sentido horario positivo):
//      0 = Zona de reposo, 1 = Contenedor 1, ..., 7 = Contenedor 7
//  "Mover a la division N" significa dejar esa division SOBRE la descarga.
//  Si el contenedor que llega no es el correcto (llega el 7 en vez del 1),
//  intercambiar SENTIDO_HORARIO y SENTIDO_ANTIHORARIO en la seccion 1.2.


// ---------------------------------------------------------------------
//  2.1  MOVIMIENTO
// ---------------------------------------------------------------------

// Lee la posicion actual de forma segura (la ISR tambien la modifica)
long leerPosicionActual() {
  portENTER_CRITICAL(&muxMotor);
  long pos = posicionActual;
  portEXIT_CRITICAL(&muxMotor);
  return pos;
}

// Inicia un movimiento de 'pasos' micropasos. Signo + horario, - antihorario.
// No bloquea: la ISR del timer genera los pulsos.
void iniciarMovimiento(long pasos) {
  if (pasos == 0) return;

  int dir = (pasos > 0) ? 1 : -1;
  digitalWrite(PIN_DIR, (dir > 0) ? SENTIDO_HORARIO : SENTIDO_ANTIHORARIO);
  delayMicroseconds(5);  // DRV8825: DIR debe estar estable antes de STEP

  portENTER_CRITICAL(&muxMotor);
  direccionActual     = dir;
  nivelPinStep        = false;
  micropasosRestantes = (pasos > 0) ? pasos : -pasos;
  motorEnMovimiento   = true;
  portEXIT_CRITICAL(&muxMotor);
}

// Espera (bloqueando) a que termine el movimiento en curso
void esperarFinMovimiento() {
  while (motorEnMovimiento) {
    delay(1);
  }
  // Mantiene la posicion dentro de una vuelta: 0 .. MICROPASOS_POR_VUELTA-1
  portENTER_CRITICAL(&muxMotor);
  posicionActual = ((posicionActual % MICROPASOS_POR_VUELTA) + MICROPASOS_POR_VUELTA)
                   % MICROPASOS_POR_VUELTA;
  portEXIT_CRITICAL(&muxMotor);
}

// Lleva la division indicada (0..7) sobre la descarga por el camino mas corto
void moverADivision(int division) {
  long objetivo = (long)division * MICROPASOS_POR_DIVISION;
  long delta    = objetivo - leerPosicionActual();

  // Camino mas corto: delta queda entre -media vuelta y +media vuelta
  delta %= MICROPASOS_POR_VUELTA;
  if (delta >  MICROPASOS_POR_VUELTA / 2) delta -= MICROPASOS_POR_VUELTA;
  if (delta < -MICROPASOS_POR_VUELTA / 2) delta += MICROPASOS_POR_VUELTA;

  iniciarMovimiento(delta);
  esperarFinMovimiento();
}

// Lleva el contenedor N (1..7) sobre la descarga
bool moverAContenedor(int contenedor) {
  if (contenedor < 1 || contenedor > NUM_CONTENEDORES) {
    Serial.printf("Error: contenedor %d no existe (1..%d)\n", contenedor, NUM_CONTENEDORES);
    return false;
  }
  Serial.printf("Moviendo contenedor %d a la descarga...\n", contenedor);
  moverADivision(contenedor);
  return true;
}

// Regresa la zona de reposo sobre la descarga (estado inicial)
void volverAReposo() {
  Serial.println("Volviendo a ZONA DE REPOSO...");
  moverADivision(POSICION_REPOSO);
  Serial.println("En reposo.");
}

// Sin sensor de home: declara la posicion actual como zona de reposo.
// Usar despues de alinear el cilindro a mano.
void definirReposoAqui() {
  portENTER_CRITICAL(&muxMotor);
  posicionActual = (long)POSICION_REPOSO * MICROPASOS_POR_DIVISION;
  portEXIT_CRITICAL(&muxMotor);
  Serial.println("Posicion actual definida como ZONA DE REPOSO.");
}


// ---------------------------------------------------------------------
//  2.2  DESCARGA
// ---------------------------------------------------------------------

// Lleva un contenedor a la descarga y espera TIEMPO_DESCARGA_MS.
// No regresa al reposo (asi se pueden encadenar varios contenedores).
void descargarContenedor(int contenedor) {
  if (!moverAContenedor(contenedor)) return;
  Serial.printf("Descargando contenedor %d (%lu ms)...\n", contenedor, TIEMPO_DESCARGA_MS);
  delay(TIEMPO_DESCARGA_MS);
  Serial.printf("Contenedor %d descargado.\n", contenedor);
}


// ---------------------------------------------------------------------
//  2.3  CARGA  (cola de pedidos)
// ---------------------------------------------------------------------

// Agrega un contenedor a la cola del pedido
bool cargarEnCola(int contenedor) {
  if (contenedor < 1 || contenedor > NUM_CONTENEDORES) {
    Serial.printf("Ignorado: contenedor %d no existe (1..%d)\n", contenedor, NUM_CONTENEDORES);
    return false;
  }
  if (cantidadEnCola >= TAMANO_COLA) {
    Serial.println("Error: la cola de pedidos esta llena.");
    return false;
  }
  colaContenedores[cantidadEnCola++] = contenedor;
  return true;
}

// Descarga todos los contenedores de la cola en orden y vuelve al reposo
void ejecutarCola() {
  if (cantidadEnCola == 0) {
    Serial.println("No hay contenedores en la cola.");
    return;
  }
  Serial.printf("Ejecutando pedido de %d contenedor(es)\n", cantidadEnCola);
  for (int i = 0; i < cantidadEnCola; i++) {
    descargarContenedor(colaContenedores[i]);
  }
  cantidadEnCola = 0;
  volverAReposo();
  Serial.println("Pedido terminado.");
}


// =====================================================================
//  3. MAIN
// =====================================================================
//  Ordenes por el Monitor Serie (115200 baudios, final de linea "Nueva linea"):
//      2,5,7   -> descarga los contenedores 2, 5 y 7 y vuelve al reposo
//      3       -> descarga solo el contenedor 3 y vuelve al reposo
//      0       -> vuelve a la zona de reposo
//      + / -   -> ajuste fino (1/8 de division) para alinear a mano
//      R       -> define la posicion actual como zona de reposo
//      ?       -> ayuda y posicion actual

const long MICROPASOS_AJUSTE = MICROPASOS_POR_DIVISION / 8;  // 50 micropasos

String lineaSerial = "";

void mostrarAyuda() {
  Serial.println();
  Serial.println("----------- ORDENES -----------");
  Serial.println("  2,5,7  descargar varios contenedores");
  Serial.println("  3      descargar un contenedor");
  Serial.println("  0      volver a zona de reposo");
  Serial.println("  + / -  ajuste fino para alinear");
  Serial.println("  R      fijar posicion actual como reposo");
  Serial.println("  ?      ayuda");
  Serial.printf("Posicion actual: %ld micropasos (division %.2f)\n",
                leerPosicionActual(),
                (float)leerPosicionActual() / MICROPASOS_POR_DIVISION);
  Serial.println("-------------------------------");
}

// Interpreta una linea completa recibida por serial
void procesarOrden(String orden) {
  orden.trim();
  if (orden.length() == 0) return;

  char c = toupper(orden.charAt(0));

  if (c == '?') { mostrarAyuda();       return; }
  if (c == 'R') { definirReposoAqui();  return; }
  if (c == '+') { iniciarMovimiento( MICROPASOS_AJUSTE); esperarFinMovimiento(); return; }
  if (c == '-') { iniciarMovimiento(-MICROPASOS_AJUSTE); esperarFinMovimiento(); return; }
  if (orden == "0") { volverAReposo();  return; }

  // Lista de contenedores separados por coma o espacio: "2,5,7" / "2 5 7"
  cantidadEnCola = 0;
  int numero = 0;
  bool hayNumero = false;
  for (unsigned int i = 0; i <= orden.length(); i++) {
    char ch = (i < orden.length()) ? orden.charAt(i) : ',';
    if (isDigit(ch)) {
      numero = numero * 10 + (ch - '0');
      hayNumero = true;
    } else if (ch == ',' || ch == ' ' || ch == ';') {
      if (hayNumero) cargarEnCola(numero);
      numero = 0;
      hayNumero = false;
    } else {
      Serial.printf("Orden no valida: \"%s\". Escriba ? para ayuda.\n", orden.c_str());
      cantidadEnCola = 0;
      return;
    }
  }
  ejecutarCola();
}

void loop() {
  // Lee caracteres hasta completar una linea (Enter)
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineaSerial.length() > 0) {
        Serial.printf("> %s\n", lineaSerial.c_str());
        procesarOrden(lineaSerial);
        lineaSerial = "";
      }
    } else if (lineaSerial.length() < 64) {
      lineaSerial += ch;
    }
  }
}
