#include "config.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "operacional.h"
#include "operaPagina.h"
#include "operaBLE.h"
#include "operaRFID.h"

config_t configuracao = {0};

xQueueHandle listaLiberarML;

TaskHandle_t taskRFIDHandle = NULL;

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    pinMode( PINO_RELE, OUTPUT );
    digitalWrite(PINO_RELE,!RELE_ON);

    pinMode( PINO_STATUS, OUTPUT );
    digitalWrite( PINO_STATUS, !LED_STATUS_ON);

    pinMode( PINO_SENSOR_FLUSO, INPUT);
    
    // Inicia porta para debug
    #ifdef debug_debug
        Serial.begin(115200);
        delay(3000);
        unsigned long tF = millis() + 5000UL;
        while ((!Serial)&&(millis() < tF)){
            yield();
        }
        Serial.println();
        Serial.println(F("[SETUP] Iniciando Maquina")); 
    #endif

    // Efetua a leitura da configuração gravada na EEPROM
    leConfiguracao();
        
    #ifdef USAR_ESP32_UART_BLE
        setupBLE();
    #endif

    listaLiberarML = xQueueCreate(1,sizeof(uint32_t));

    #ifdef USAR_RFID
        xTaskCreate(taskRFID, "taskRFID", 4096, NULL, 3, &taskRFIDHandle);
    #endif

    // FIX STATUS=8: taskLiberaML movida para Core 1 (APP_CPU_NUM).
    // O BLE Bluedroid roda na Core 0 (PRO_CPU). Quando taskLiberaML rodava
    // tambem na Core 0, o loop while() com vTaskDelay(50ms) impedia que o
    // stack BLE processasse os LL keep-alive packets com prioridade suficiente,
    // causando o Connection Supervision Timeout (status=8) no Android.
    // Com taskLiberaML na Core 1, o BLE tem a Core 0 exclusivamente.
    xTaskCreatePinnedToCore(taskLiberaML, "taskLiberaML", 8192, NULL, 3, NULL, 1);

}

void loop() {
    // Apaga tarefa loop, pois não será utilizada
    vTaskDelete(NULL);
}



/*
Arduino Water flow meter
YF- S201 Hall Effect Water Flow Sensor
Water Flow Sensor output processed to read in litres/hour

volatile int flow_frequency; // Measures flow sensor pulses
unsigned int l_hour; // Calculated litres/hour
unsigned char flowsensor = 2; // Sensor Input
unsigned long currentTime;
unsigned long cloopTime;
void flow () // Interrupt function
{
   flow_frequency++;
}
void setup()
{
   pinMode(flowsensor, INPUT);
   digitalWrite(flowsensor, HIGH); // Optional Internal Pull-Up
   Serial.begin(9600);
   attachInterrupt(0, flow, RISING); // Setup Interrupt
   sei(); // Enable interrupts
   currentTime = millis();
   cloopTime = currentTime;
}
void loop ()
{
   currentTime = millis();
   // Every second, calculate and print litres/hour
   if(currentTime >= (cloopTime + 1000))
   {
      cloopTime = currentTime; // Updates cloopTime
      // Pulse frequency (Hz) = 7.5Q, Q is flow rate in L/min.
      l_hour = (flow_frequency * 60 / 7.5); // (Pulse frequency x 60 min) / 7.5Q = flowrate in L/hour
      flow_frequency = 0; // Reset Counter
      Serial.print(l_hour, DEC); // Print litres/hour
      Serial.println(" L/hour");
   }
}
   */