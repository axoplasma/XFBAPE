const char* versionstring = "v3.0.0";
const char compile_date[] = __DATE__ " " __TIME__;

// the following defines will overwrite the standard arduino-defines from https://github.com/arduino/ArduinoCore-samd/blob/84c09b3265e2a8b548a29b141f0c9281b1baf154/variants/mkrvidor4000/variant.h
//#define I2S_INTERFACES_COUNT 1 // SAMD21 has two interfaces
//#define I2S_DEVICE 0 // select if I2S device 0 or 1
//#define I2S_CLOCK_GENERATOR 3 // select the correct clock for I2S-interface
//#define PIN_I2S_SD 21 // pin for SerialData (= A6)
//#define PIN_I2S_SCK 02 // pin for bit-clock (= D2)
//#define PIN_I2S_FS 03 // pin for FrameSelect / Wordclock (= D3)

#define USE_DISPLAY       1      // enables a SSD1308 display connected to I2C
#define USE_XTOUCH        0      // support for XTouch via Ethernet (currently in Alpha-state and needs more work!)
#define XTOUCH_COUNT      1      // number of XTouch-Devices

// includes for FPGA
#include <wiring_private.h>
#include "jtag.h"
#include "fpga.h"
#include <Ethernet.h>
#if USE_XTOUCH == 1
  #include <EthernetUdp.h>
#endif
#include <SPI.h>
#include "Ticker.h"

// definitions for I2C EEPROM
#include "Wire.h"
#include "I2C_eeprom.h"
#define EEPROM_Address 0x50

/*
Arduino SAMD21 SERCOM usage:
SERCOM0: Serial1  <- X32
SERCOM1: Unused   <- 
SERCOM2: Unused   <- 
SERCOM3: I2C      <- Display
SERCOM4: SPI      <- W5500 Ethernet
SERCOM5: Serial   <- USB
*/
#define SerialX32 Serial1

// includes for Serial2 to communicate with NINA. As Serial2 is not within the scope of Arduino,
// we have to create it using the SERCOM-system of the SAMD21
#define PIN_SERIAL2_TX       (0ul)                // Pin description number for PIO_SERCOM on D0
#define PIN_SERIAL2_RX       (1ul)                // Pin description number for PIO_SERCOM on D1
#define PAD_SERIAL2_TX       (UART_TX_PAD_0)      // SERCOM pad 0 TX
#define PAD_SERIAL2_RX       (SERCOM_RX_PAD_1)    // SERCOM pad 1 RX
Uart Serial2(&sercom3, PIN_SERIAL2_RX, PIN_SERIAL2_TX, PAD_SERIAL2_RX, PAD_SERIAL2_TX);


/*
#define PIN_SERIAL3_TX       (4ul)                // Pin description number for PIO_SERCOM on D0
#define PIN_SERIAL3_RX       (5ul)                // Pin description number for PIO_SERCOM on D1
#define PAD_SERIAL3_TX       (UART_TX_PAD_2)      // SERCOM pad 0 TX
#define PAD_SERIAL3_RX       (SERCOM_RX_PAD_3)    // SERCOM pad 1 RX
Uart Serial3(&sercom4, PIN_SERIAL3_RX, PIN_SERIAL3_TX, PAD_SERIAL3_RX, PAD_SERIAL3_TX);
*/
bool passthroughNINA = false;
#define X32_RINGBUF_LEN 32 //longest command seems to be 22 chars
uint8_t x32RingBuffer[X32_RINGBUF_LEN];
uint16_t x32RingBufferPointer = 0;
uint8_t x32AliveCounter = 59; // preload to 5 seconds (for ticker with 85ms)
String x32AliveCommand = "*8BE#";
bool x32Playback = false;
bool x32Debug = false;
uint8_t x32NumberOfCards = 2;
uint8_t x32currentCardSelection = 0;
uint32_t x32CardSize[2] = {31163136, 15581568}; // one 32GB and one 16GB

struct{ // don't change order of struct! Just add variables or replace with same size!!!
  uint16_t Version = 0;
  IPAddress ip;
  IPAddress xtouchip[XTOUCH_COUNT];
} eeprom_config;

struct{
  uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
} config;
I2C_eeprom eeprom(EEPROM_Address, I2C_DEVICESIZE_24LC16);

// Ethernet-objects
EthernetServer server(80);
EthernetServer cmdserver(5025);

#if USE_DISPLAY == 1
  #include "Wire.h"

  // includes for display
  #include "Bitmaps.h"
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #define SCREEN_WIDTH 128 // OLED display width, in pixels
  #define SCREEN_HEIGHT 64 // OLED display height, in pixels
  #define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
  #define SCREEN_ADDRESS 0x3C // this address does not fit to the address on the PCB!!!
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  uint8_t currentDisplayLine = 0;
#endif

#if USE_XTOUCH == 1
  uint16_t XCtlWatchdogCounter = 20; // preload to 2 seconds
#endif

String TOC;
uint8_t tocEntries = 3;
uint8_t tocCounter = 0;

struct {
  String title = "Standby...";
  uint32_t time;
  uint32_t duration;
  uint8_t progress;
  float volumeMain;
  uint8_t balanceMain;
  float volumeSub;
  float volumeCh[32]; // currently there is no communication between NINA and SAMD for this value
  float volumeAnalog;
  float volumeCard;
  float frequencyLowPass;
  float frequencyHighPass;
  uint8_t adcGain;
  float gateThreshold;
  float limitThreshold;
  uint8_t audioStatusInfo;

  String MainCtrlVersion;
  String FPGAVersion;
}playerinfo;

// some defines for NINA update mode
#define NINA_PIO27 11
#define NINA_RESET_N 31
unsigned long baud = 115200;
int rts = -1;
int dtr = -1;
bool firmwareUpdateMode = false;

// general variables
uint32_t refreshCounter = 0;

#if USE_XTOUCH == 1
  EthernetUDP XCtlUdp[XTOUCH_COUNT];

  uint8_t XCtl_Probe[8] = {0xF0, 0x00, 0x20, 0x32, 0x58, 0x54, 0x00, 0xF7};
  uint8_t XCtl_ProbeResponse[8] = {0xF0, 0x00, 0x20, 0x32, 0x58, 0x54, 0x01, 0xF7};
  uint8_t XCtl_ProbeB[18] = {0xF0, 0x00, 0x00, 0x66, 0x58, 0x01, 0x30, 0x31, 0x35, 0x36, 0x34, 0x30, 0x36, 0x36, 0x37, 0x34, 0x30, 0xF7};
  uint8_t XCtl_ProbeC[18] = {0xF0, 0x00, 0x00, 0x66, 0x58, 0x01, 0x30, 0x31, 0x35, 0x36, 0x34, 0x30, 0x36, 0x33, 0x44, 0x35, 0x36, 0xF7};
  uint8_t XCtl_IdlePacket[7] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7};

  struct sXctlScribblePad{
    String topText = "TopText";
    String botText = "BotText";
    uint8_t color = 7; // 0=BLACK, 1=RED, 2=GREEN, 3=YELLOW, 4=BLUE, 5=PINK, 6=CYAN, 7=WHITE
    bool inverted = false;
  };

  struct sXCtlChannel{
    bool faderNeedsUpdate = true;
    bool faderTouched = false;
    uint16_t faderPosition = 0; // 0...16383
    uint16_t faderPositionHW = 0; // 0...16383
    uint8_t meterLevel = 0; // 0...8
    uint8_t dialLevel = 128; // 0..255 ->0..12

    uint8_t rec = 0; // 0=OFF, 1=ON, 2=FLASHING
    uint8_t solo = 0; // 0=OFF, 1=ON, 2=FLASHING
    uint8_t mute = 0; // 0=OFF, 1=ON, 2=FLASHING
    uint8_t select = 0; // 0=OFF, 1=ON, 2=FLASHING
  };

  struct sXCtlOptions{
    bool showValues = true;
  };

  struct sXCtl{
    IPAddress ip;
    uint8_t channelOffset = 0;
    bool forceUpdate = false;

    sXCtlOptions options;
    sXctlScribblePad scribblePad[33];
    sXCtlChannel channel[33]; // 33=masterFader
    uint8_t jogDialValue = 0;
    char segmentDisplay[12];
    uint8_t buttonLightOn[103];
  }XCtl[XTOUCH_COUNT];
#endif