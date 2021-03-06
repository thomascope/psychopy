/*
ioSync Sketch for Teensy 3.0 / 3.1

Copyright (C) 2013-2014 iSolver Software Solutions
Distributed under the terms of the GNU General Public License (GPL version 3 or any later version).

.. author:: Sol Simpson <sol@isolver-software.com>

- Maintains a 48 bit microsecond clock so rollover will only occur if MCU is running without reset for ~ 8.9 years.
- Handles serial requests from the host PC and sends necessary reply.
- If analog input events are enabled, handles reading analog input lines and streaming analog samples to Host PC
- If digital input events are enabled, handles reading digital input lines and sending a digital input event whenever the digital input byte value has changed.

ioSync assigns the Teensy 3 pins in a fixed usage mapping. See below for what that mapping is.

TODO:

- Allow some constants to be set by user. For example analog input related constants.
- Add support for setting per channel analog input thresholds which can be used for voice key or light key event detection.
- Allow setting digital inputs as INPUT or PULLUP_INPUTs
- Properly utilize (or remove) the 9th digital input bit.
- Switch digital and analog input reads to use interupts, running at 5000 Hz if possible. **BACKUP CURRENT CODE BEFORE WORKING ON THIS. 


*/

// Program control defines. Change based on desired usage of ioSync

// GENERATE_KEYBOARD_EVENT support:
//   To enable:
//     - Uncomment the below KEYBOARD define.
//     - Ensure Tools -> USB Type is set to "Serial + Keyboard + ....".
//     - Rebuild the iosync sketch and upload it.
//
//   To disable:
//     - Comment out the below KEYBOARD define.
//     - Ensure Tools -> USB Type is set to "Serial" only.
//     - Rebuild the iosync sketch and upload it.
//
//#define KEYBOARD

// Misc. Util functions

byte bytePow(byte x, byte p)
{
  if (p == 0) return 1;
  if (p == 1) return x;

  byte tmp = bytePow(x, p/2);
  if (p%2 == 0) return tmp * tmp;  
  else return x * tmp * tmp;
}

// PIN LABELS

// teensy 3 board led pin
#define LED 13

// Non USB UART RT and TX can be accessed using 'Serial1' object in Teensiduino
#define UART_RX RX1
#define UART_TX TX1

// give Teensy 3 Pin numbers for Digital Out
#define DO_0 2
#define DO_1 3
#define DO_2 4
#define DO_3 5
#define DO_4 25 // on bottom of T3
#define DO_5 26 // on bottom of T3
#define DO_6 27 // on bottom of T3
#define DO_7 28   // on bottom of T3

byte DOUT_PINS[8]={
  DO_0,DO_1,DO_2,DO_3,DO_4,DO_5,DO_6,DO_7};

// give Teensy 3 Pin numbers for Digital In
#define DI_0 6
#define DI_1 7
#define DI_2 8
#define DI_3 9
#define DI_4 29 // on bottom of T3
#define DI_5 30// on bottom of T3
#define DI_6 31 // on bottom of T3
#define DI_7 32 // on bottom of T3

byte DIN_PINS[8]={
  DI_0,DI_1,DI_2,DI_3,DI_4,DI_5,DI_6,DI_7};

// Update : now being used in PULLUP input mmode. ##currently Teensy has 9 DINs setup, so we have one extra stand alone. 
#define DIN_8 33

// SPI related pins
#define SPI_SS CS0 // pin 10 on T3, Device Select
#define SPI_MOSI DOUT //pin 11 on T3, SPI Data Output 
#define SPI_MISO DIN //pin 12 on T3, SPI Data Input
#define SPI_SCK SCK // pin 13 on 3, Clock

// give Teensy 3 Pin numbers for Analog In
#define AI_0 14
#define AI_1 15
#define AI_2 16
#define AI_3 17
#define AI_4 A10 // on bottom of T3
#define AI_5 A11 // on bottom of T3
#define AI_6 A12 // on bottom of T3
#define AI_7 A13 // on bottom of T3
byte AIN_PINS[8]={
  AI_0,AI_1,AI_2,AI_3,AI_4,AI_5,AI_6,AI_7};

// AIN_THRESHOLD_LEVELS are set by the user, specifying
// the AIN value at which a THRESHOLD_EVENT should be triggered.
// A THRESHOLD_EVENT is triggered each time the analog input for a 
// AIN line goes above , or falls, below, the threshold
unsigned int AIN_THRESHOLD_LEVELS[8]={0,0,0,0,0,0,0,0};

// Stores the threshold state for each AIN line.
// 0 == below threshold, 1 == above threshold.
// When the threshold state changes for an analog input line, 
// a threshold event is created.
byte AIN_THRESHOLD_STATES[8]={0,0,0,0,0,0,0,0};

const double AREF_INTERVAL_V = 1.2;
const double DIGITAL_ANALOG_16_STEP = 0.00001831054687;
const double DIGITAL_ANALOG_14_STEP = 0.0000732421875;
const double DIGITAL_ANALOG_12_STEP = 0.00029296875;
const double DIGITAL_ANALOG_10_STEP = 0.001171875;

#define AIN_RES 16
#define AIN_AVERAGING 16
#define AIN_REF EXTERNAL //INTERNAL //EXTERNAL
#define AIN_RATE 1000

// I2C pins
#define I2C_SCL SCL
#define I2C_SDA SDA

// PWM pins
#define PMW_0 20
#define PMW_1 21
#define PMW_2 22
#define PMW_3 23
const byte PMW_PINS[4]={
  PMW_0,PMW_1,PMW_2,PMW_3};


//-------------------------------------------------
// 48 bit usec timer used for time stamping

struct Usec{
  unsigned long usecs;
  unsigned int rolls;
  unsigned long prev_usecs;
  byte bytes[6];
};

Usec t3_usec_time;

void updateUsecTime(){
  // Update 48 bit usec timer
  t3_usec_time.usecs=micros();
  if(t3_usec_time.usecs<t3_usec_time.prev_usecs) // its rolled over
    t3_usec_time.rolls++; // increment 2 byte roll counter
  t3_usec_time.prev_usecs=t3_usec_time.usecs; 
  
  // update byte stream rep of current time
  t3_usec_time.bytes[0] = ( (t3_usec_time.usecs) >> 24) & 0xff; //event time bits 24..31
  t3_usec_time.bytes[1] = ( (t3_usec_time.usecs) >> 16) & 0xff; //event time bits 16..23
  t3_usec_time.bytes[2] = ( (t3_usec_time.usecs) >> 8) & 0xff; //event time bits 8..15
  t3_usec_time.bytes[3] = ( (t3_usec_time.usecs) & 0xff);    //event time bits 0..7  
  t3_usec_time.bytes[4] = ( (t3_usec_time.rolls) >> 8) & 0xff; //roll counter bits 40..47
  t3_usec_time.bytes[5] = ( (t3_usec_time.rolls) & 0xff);    //roll counter bits 32..39
  
}

//----------------------------------------------------
// Data Buffer for holding bytes to be sent via
// Serial USB to Host PC

#define TX_BYTE_BUF_SIZE 88

unsigned int tx_byte_buffer_index=0;
byte tx_byte_buffer[TX_BYTE_BUF_SIZE];

unsigned int byteBufferFreeSize(){
  return TX_BYTE_BUF_SIZE-tx_byte_buffer_index;
}

byte writeByteBufferToSerial(){
  byte bsent=Serial.write((byte*)tx_byte_buffer, tx_byte_buffer_index);
  tx_byte_buffer_index=0;
  return bsent;  
}

//------------------------------------
//
// Host PC Request Functionality

#define NULL_REQUEST 0
#define GET_USEC_TIME 1
#define SET_DIGITAL_OUT_PIN 2
#define SET_DIGITAL_OUT_STATE 3
#define GET_DIGITAL_IN_STATE 4
#define GET_AIN_CHANNELS 5
#define SET_T3_INPUTS_STREAMING_STATE 6
#define SYNC_TIME_BASE 7
#define RESET_STATE 8
#define GENERATE_KEYBOARD_EVENT 9
#define SET_ANALOG_THRESHOLDS 10
#define REQUEST_TYPE_COUNT 11

#define REQUEST_TX_HEADER_BYTE_COUNT 8

byte request_tx_byte_length[REQUEST_TYPE_COUNT]={
  0,
  REQUEST_TX_HEADER_BYTE_COUNT,
  REQUEST_TX_HEADER_BYTE_COUNT,
  REQUEST_TX_HEADER_BYTE_COUNT+2,
  REQUEST_TX_HEADER_BYTE_COUNT+1,
  REQUEST_TX_HEADER_BYTE_COUNT+sizeof(AIN_PINS)*2,
  REQUEST_TX_HEADER_BYTE_COUNT,
  REQUEST_TX_HEADER_BYTE_COUNT,
  REQUEST_TX_HEADER_BYTE_COUNT,
  REQUEST_TX_HEADER_BYTE_COUNT+2,
  REQUEST_TX_HEADER_BYTE_COUNT, 
};

void NullHandlerRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleUsecTimeRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleSetDigitalOutPinRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleSetDigitalOutStateRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleGetDigitalInStateRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleGetAnalogInChannelsRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleEnableInputStreamingRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleSyncTimebaseRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleResetStateRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleGenerateKeyboardEventRx(byte request_type,byte request_id,byte request_rx_byte_count);
void handleSetAnalogThresholdsEventRx(byte request_type,byte request_id,byte request_rx_byte_count);

void (*requestHandlerFP[REQUEST_TYPE_COUNT])(byte request_type,byte request_id,byte request_rx_byte_count)={
  NullHandlerRx,
  handleUsecTimeRx,
  handleSetDigitalOutPinRx,
  handleSetDigitalOutStateRx,
  handleGetDigitalInStateRx,
  handleGetAnalogInChannelsRx,
  handleEnableInputStreamingRx,
  handleSyncTimebaseRx,
  handleResetStateRx,
  handleGenerateKeyboardEventRx,
  handleSetAnalogThresholdsEventRx
};

elapsedMicros sinceLastSerialTx;
elapsedMicros sinceLastInputRead;
#define MAX_TX_BUFFERING_INTERVAL 700

//---------------------------------------
// Host PC Request Processor, sets Request Tx reply
// header bytes and calls necesssary
// Host PC Request Handlers.

unsigned int handleHostSerialRequests(){
  char rx_info[3];
  byte rtype,rid,rx_count,usec_time_start_byte_index;

  if (Serial.available() >=3) {  
    Serial.readBytes(rx_info,3);
    rtype=rx_info[0];
    rid=rx_info[1];
    rx_count=rx_info[2];

    updateUsecTime();
    
    tx_byte_buffer[tx_byte_buffer_index]=rid;
    tx_byte_buffer[tx_byte_buffer_index+1]=request_tx_byte_length[rtype];
    tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
    tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
    tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
    tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
    tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
    tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];
    tx_byte_buffer_index=tx_byte_buffer_index+8;

    requestHandlerFP[rtype](rtype,rid,rx_count-8);
  }  
  return 0;  
}

//---------------------------------------
// Host PC Request Handlers
// for different Request types

void NullHandlerRx(byte request_type,byte request_id,byte request_rx_byte_count){
  //nothing to do here
}

//------------------------

void handleUsecTimeRx(byte request_type,byte request_id,byte request_rx_byte_count){
  //nothing to do here
}

//------------------------

void handleSyncTimebaseRx(byte request_type,byte request_id,byte request_rx_byte_count){
  //force immediate tx of response.
  sinceLastSerialTx = sinceLastSerialTx + MAX_TX_BUFFERING_INTERVAL;
}

//------------------------

void handleSetDigitalOutStateRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  SET_DIGITAL_OUT_STATE: Sets all 8 digital out pins using the bit pattern provided of the digital output lines.
   
   RX bytes ( 4 ):
   0: SET_DIGITAL_OUT_STATE
   1: Request ID
   2: Rx Byte Count
   3: 8 dout pins state (as 1 byte, 0 - 256)
   
   TX Bytes ( 7 ):
   0: Request ID
   1: Tx Byte Count
   2 - 7: usec time that pin was set.    
   */

  byte new_dout_state=Serial.read();

  int i=0;
  for (byte mask = 00000001; mask>0; mask <<= 1) { //iterate through bit mask
    digitalWrite(DOUT_PINS[i],new_dout_state & mask); // set 1
    i++;
  }
}

//------------------------

void handleSetDigitalOutPinRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  SET_DIGITAL_OUT_PIN: Sets one of the digital output lines.
   
   RX bytes ( 5 ):
   0: SET_DIGITAL_OUT_PIN
   1: Request ID
   2: Rx Byte Count
   3: pin number (0-7)
   4: pin state (0 == LOW, 1==HIGH)
   
   
   TX Bytes ( 7 ):
   0: Request ID
   1: Tx Byte Count
   2 - 7: usec time that pin was set.    
   */
  char pin_value[2]={
    0,0  }; // pin number, Pin state
  Serial.readBytes(pin_value,2);

  //  if (pin_value[1]==0)
  //    digitalWrite(DOUT_PINS[pin_value[0]],LOW); // set 0
  //  else
  //    digitalWrite(DOUT_PINS[pin_value[0]],HIGH); // set 1
  digitalWrite(DOUT_PINS[pin_value[0]],pin_value[1]);
}

//------------------------
#ifdef KEYBOARD
  IntervalTimer resetKey1;
  volatile byte reset_key1_active = 0;
  
  void setResetKey1Active(void) {
    reset_key1_active=1;
  }

void handleGenerateKeyboardEventRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  GENERATE_KEYBOARD_EVENT: Generates a USB keyboard event on the Host PC.
   
   RX bytes ( 5 ):
   0: SET_DIGITAL_OUT_PIN
   1: Request ID
   2: Rx Byte Count
   3: send_char (0-255)
   4: press_duration in msec (100 msec increments)
   
   
   TX Bytes ( 7 ):
   0: Request ID
   1: Tx Byte Count
   2 - 7: usec time that pin was set.    
   */
  unsigned int usec_duration;
  char key_event_info[2]={
    0,0}; // char to send, 8 bit msec duration (100 msec incremnents)
  Serial.readBytes(key_event_info,2);
  
  usec_duration=(unsigned int)(((byte)key_event_info[1])*100000);
  Keyboard.set_key1(KEY_V);  
  Keyboard.send_now();
  resetKey1.begin(setResetKey1Active, usec_duration);
}
#else
void handleGenerateKeyboardEventRx(byte request_type,byte request_id,byte request_rx_byte_count){
  char key_event_info[2]={0,0}; // char to send, 8 bit msec duration (100 msec incremnents)
  Serial.readBytes(key_event_info,2);
}
#endif

//------------------------

void handleGetDigitalInStateRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  GET_DIGITAL_IN_STATE: Gets all 8 digital in pins.
   
   RX bytes ( 3 ):
   0: GET_DIGITAL_IN_STATE
   1: request ID
   2: Rx Byte Count
   
   TX Bytes ( 9 ):
   0: GET_DIGITAL_IN_STATE
   1: size of response in bytes    
   2 - 7: usec time that pin was set.
   8: digital input line value as a byte between 0 and 255    
   */
  byte din_value=0;
  for (int i=0;i<sizeof(DIN_PINS);i++)
    din_value+=bytePow(2,i)*digitalRead(DIN_PINS[i]);

  tx_byte_buffer[tx_byte_buffer_index]=din_value;
  tx_byte_buffer_index+=1; 
}

//------------------------

void handleGetAnalogInChannelsRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  GET_AIN_CHANNELS: Gets 1 - 8 values from the analog inputs
  
  RX bytes ( 3 ):
   0: GET_DIGITAL_IN_STATE
   1: request ID
   2: Rx Byte Count
   
   TX Bytes ( 25 ):
   0: Request ID
   1: Tx Byte Count
   2 - 7: usec time that pin was set.    
   8 - 24: 8 * 16 bit analog input values
   */
  unsigned int v=0;
  for (int i=0;i<sizeof(AIN_PINS);i++){
    v=analogRead(AIN_PINS[i]);
    tx_byte_buffer[tx_byte_buffer_index+8+i*2] = (v >> 8) & 0xff; //ain bits 8..15
    tx_byte_buffer[tx_byte_buffer_index+9+i*2] = (v & 0xff);    //ain time bits 0..7  
    }
  tx_byte_buffer_index+=16; 
}


//------------------------

void handleSetAnalogThresholdsEventRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  SET_ANALOG_THRESHOLDS: Sets the threshold value for each analog input line.
  
  RX bytes ( 19 ):
   0: SET_ANALOG_THRESHOLDS
   1: request ID
   2: Rx Byte Count
   3-19: 8 * 16 bit analog input threshold values

   TX Bytes ( 8 ):
   0: Request ID
   1: Tx Byte Count
   2 - 7: usec time that pin was set.    
   */
  char threshold_data[sizeof(AIN_PINS)*2];   
  Serial.readBytes(threshold_data,sizeof(AIN_PINS)*2);

  for (int i=0;i<sizeof(AIN_PINS);i++){
    
    AIN_THRESHOLD_STATES[i]= 0;
    AIN_THRESHOLD_LEVELS[i]=(unsigned int)((threshold_data[i*2] << 8) + threshold_data[i*2+1]);
    }

}

//------------------------

byte digital_input_streaming_enabled=0;
byte analog_input_streaming_enabled=0;
byte threshold_event_streaming_enabled=0;

void handleEnableInputStreamingRx(byte request_type,byte request_id,byte request_rx_byte_count){
  char inputStreaming[3]={
    0,0,0  };
  Serial.readBytes(inputStreaming,3);
  digital_input_streaming_enabled=inputStreaming[0];
  analog_input_streaming_enabled=inputStreaming[1];
  threshold_event_streaming_enabled=inputStreaming[2];
}

//---------------------------------------
//
// IntervalTimer for reading digital and analog inputs
#define INPUT_LINES_READ_RATE AIN_RATE

byte  last_digital_input_state=0;
byte current_digital_input_state=0;

//DIN_8 is set as a PULLUP. 
byte  last_pullup_input_state=1;
byte current_pullup_input_state=1;

void inputLineReadTimerCallback(void){
  if (digital_input_streaming_enabled>0){
      addDigitalEventToByteBuffer(); // check for digital input change event
  }

  if (analog_input_streaming_enabled>0 || threshold_event_streaming_enabled>0){
    // Add Analog Input Event
    addAnalogEventToByteBuffer();
  }
}

//----------------------------------------------------
// T3 Generated Events

#define DIGITAL_INPUT_EVENT 1
#define ANALOG_INPUT_EVENT 2
#define THRESHOLD_EVENT 3

#define EVENT_TX_HEADER_COUNT 8
#define DIGITAL_EVENT_TX_BYTE_COUNT 9
#define ANALOG_EVENT_TX_BYTE_COUNT 24
#define THRESHOLD_EVENT_TX_BYTE_COUNT 24

byte addDigitalEventToByteBuffer(){
    // Check for digital input state changes
    current_pullup_input_state=digitalRead(DIN_8);
    if (current_pullup_input_state!=last_pullup_input_state){
      updateUsecTime();
      if (byteBufferFreeSize()<DIGITAL_EVENT_TX_BYTE_COUNT)
        return 0;
      tx_byte_buffer[tx_byte_buffer_index]=DIGITAL_INPUT_EVENT;
      tx_byte_buffer[tx_byte_buffer_index+1]=DIGITAL_EVENT_TX_BYTE_COUNT;
      tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
      tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
      tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
      tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
      tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
      tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];
      tx_byte_buffer[tx_byte_buffer_index+8]=current_pullup_input_state;
      last_pullup_input_state=current_pullup_input_state;       
      tx_byte_buffer_index=tx_byte_buffer_index+DIGITAL_EVENT_TX_BYTE_COUNT;
    }
    else{
        current_digital_input_state=0;
        for (byte i=0;i<sizeof(DIN_PINS);i++)
          current_digital_input_state+=(bytePow(2,i)*digitalRead(DIN_PINS[i]));
    
        updateUsecTime();
          
        if (current_digital_input_state!=last_digital_input_state){
          if (byteBufferFreeSize()<DIGITAL_EVENT_TX_BYTE_COUNT)
            return 0;
          tx_byte_buffer[tx_byte_buffer_index]=DIGITAL_INPUT_EVENT;
          tx_byte_buffer[tx_byte_buffer_index+1]=DIGITAL_EVENT_TX_BYTE_COUNT;
          tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
          tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
          tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
          tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
          tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
          tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];
          tx_byte_buffer[tx_byte_buffer_index+8]=current_digital_input_state;
          last_digital_input_state=current_digital_input_state;       
          tx_byte_buffer_index=tx_byte_buffer_index+DIGITAL_EVENT_TX_BYTE_COUNT;
        }
    }
}

byte addAnalogEventToByteBuffer(){
  
  if (byteBufferFreeSize()<(ANALOG_EVENT_TX_BYTE_COUNT*2))
    return 0;
    
  unsigned int ain_readings[sizeof(AIN_PINS)];
  for (int i=0;i<sizeof(AIN_PINS);i++)
    ain_readings[i]=analogRead(AIN_PINS[i]);

  updateUsecTime();

  if (analog_input_streaming_enabled>0){
    tx_byte_buffer[tx_byte_buffer_index]=ANALOG_INPUT_EVENT;
    tx_byte_buffer[tx_byte_buffer_index+1]=ANALOG_EVENT_TX_BYTE_COUNT;
    tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
    tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
    tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
    tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
    tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
    tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];
  
    tx_byte_buffer_index=tx_byte_buffer_index+8;

    for (int i=0;i<sizeof(AIN_PINS);i++){
      tx_byte_buffer[tx_byte_buffer_index+(i*2)] = (byte)(ain_readings[i] >> 8) & 0xff; //event bits 8..15
      tx_byte_buffer[tx_byte_buffer_index+1+(i*2)] = (byte)(ain_readings[i] & 0xff);    //event bits 0..7  
    }
  
    tx_byte_buffer_index=tx_byte_buffer_index+sizeof(AIN_PINS)*2;
  }
  
  if (threshold_event_streaming_enabled){
    byte thresh_event_created=0;
    int threshold_triggered_values[sizeof(AIN_PINS)];
    for (int i=0;i<sizeof(AIN_PINS);i++){
      threshold_triggered_values[i]=0;
      if (AIN_THRESHOLD_LEVELS[i]>0){
        if (ain_readings[i] >= AIN_THRESHOLD_LEVELS[i] && AIN_THRESHOLD_STATES[i]==0){
          AIN_THRESHOLD_STATES[i]=1;
          threshold_triggered_values[i]=ain_readings[i]-AIN_THRESHOLD_LEVELS[i];
          thresh_event_created=1;
        }
        else if (ain_readings[i] < AIN_THRESHOLD_LEVELS[i] && AIN_THRESHOLD_STATES[i]==1){
          AIN_THRESHOLD_STATES[i]=0;
          threshold_triggered_values[i]=ain_readings[i]- AIN_THRESHOLD_LEVELS[i];
          thresh_event_created=1;
        }  
      }
    }
    if (thresh_event_created == 1)
      addThresholdEventToByteBuffer(threshold_triggered_values);

    }  
}

byte addThresholdEventToByteBuffer(int *threshold_triggered_values){
  /*
  Func doc string TBC
  */

  tx_byte_buffer[tx_byte_buffer_index]=THRESHOLD_EVENT;
  tx_byte_buffer[tx_byte_buffer_index+1]=THRESHOLD_EVENT_TX_BYTE_COUNT;
  tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
  tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
  tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
  tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
  tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
  tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];

  tx_byte_buffer_index=tx_byte_buffer_index+8;
  //threshold_triggered_values
  for (int i=0;i<sizeof(AIN_PINS);i++){
    tx_byte_buffer[tx_byte_buffer_index+(i*2)] = (byte)(threshold_triggered_values[i] >> 8) & 0xff; //event bits 8..15
    tx_byte_buffer[tx_byte_buffer_index+1+(i*2)] = (byte)(threshold_triggered_values[i] & 0xff);    //event bits 0..7  
  }

  tx_byte_buffer_index=tx_byte_buffer_index+sizeof(AIN_PINS)*2;
}

//----------------------------------
// Initializers for various areas of functionality.

void initDigitalOutputs(){
  for (int i=0;i<sizeof(DOUT_PINS);i++){
    pinMode(DOUT_PINS[i], OUTPUT);
    digitalWrite(DOUT_PINS[i], LOW);
  }  
}
 
void initDigitalInputs(){
  for (int i=0;i<sizeof(DIN_PINS);i++){
    pinMode(DIN_PINS[i], INPUT);
  }
  pinMode(DIN_8, INPUT_PULLUP);
}

void initAnalogInputs(){
  for (int i=0;i<sizeof(AIN_PINS);i++){
    pinMode(AIN_PINS[i], INPUT);
  }
  // What should be used as the analog reference source.
  // Options: 
  //  DEFAULT: ??
  //  INTERNAL:  1.0 ±0.3V (0.97 to 1.03 V) (source http://www.pjrc.com/teensy/K20P64M50SF0.pdf)
  //  EXTERNAL: Use the input applied to the AGND. See here from some considerations if this is used. http://forum.pjrc.com/threads/23585-AREF-is-making-me-lose-my-hair 
  analogReference(AIN_REF);

  // Analog input bit resolution 10 - 16 bits are supported
  analogReadRes(AIN_RES);

  // HW Analog Input Sample Averaging. 1 = No Averaging to 32 = average 32 samples in HW, max value is 32
  analogReadAveraging(AIN_AVERAGING);


}

void initUsec48(){
  t3_usec_time.prev_usecs=micros();
  t3_usec_time.usecs=t3_usec_time.prev_usecs;
  t3_usec_time.rolls=0;
}

//----------------------------------
// Teensy 3 Init, called each time the microcontroller is reset or powered on.

void setup()
{
  Serial.begin(115200);
  initUsec48();
  initDigitalOutputs();
  initDigitalInputs();
  initAnalogInputs();
  
  sinceLastInputRead=0;
  sinceLastSerialTx=0;
}

//---------------------------------------
// Main loop()
// Repeatedly called while microcontroller is running.

void loop()
{
  if (sinceLastInputRead>=INPUT_LINES_READ_RATE){
    inputLineReadTimerCallback();   
    sinceLastInputRead=sinceLastInputRead-INPUT_LINES_READ_RATE;
  }

  handleHostSerialRequests();

  #ifdef KEYBOARD
    byte reset_key1_copy; // holds a copy of the reset_key1_active
    noInterrupts();
    reset_key1_copy = reset_key1_active;
    interrupts();  
    if (reset_key1_copy == 1){
      Keyboard.set_key1(0);
      Keyboard.send_now();
      reset_key1_active=0;
      resetKey1.end();
     }
  #endif
  
  if ( tx_byte_buffer_index>0 && (byteBufferFreeSize()<24 || sinceLastSerialTx>=MAX_TX_BUFFERING_INTERVAL) ){
    writeByteBufferToSerial();
    Serial.flush();
    Serial.send_now();
    sinceLastSerialTx = sinceLastSerialTx - MAX_TX_BUFFERING_INTERVAL;
  }

}

//------------------------

void handleResetStateRx(byte request_type,byte request_id,byte request_rx_byte_count){
  /*
  */
  sinceLastInputRead=0;
  sinceLastSerialTx=0;

  initDigitalOutputs();  

  analog_input_streaming_enabled=0;
  threshold_event_streaming_enabled=0;
  digital_input_streaming_enabled=0;
  last_digital_input_state=0;
  current_digital_input_state=0;
  for (byte i=0;i<sizeof(DIN_PINS);i++)
      current_digital_input_state+=(bytePow(2,i)*digitalRead(DIN_PINS[i]));
  last_digital_input_state=current_digital_input_state;
  
  for (byte i=0;i<sizeof(AIN_PINS);i++){
    AIN_THRESHOLD_LEVELS[i]=0;
    AIN_THRESHOLD_STATES[i]=0;
  }
  
  initUsec48();
  updateUsecTime();
  
  tx_byte_buffer_index=0;
  tx_byte_buffer[tx_byte_buffer_index]=request_id;
  tx_byte_buffer[tx_byte_buffer_index+1]=request_tx_byte_length[request_type];
  tx_byte_buffer[tx_byte_buffer_index+2]=t3_usec_time.bytes[0];
  tx_byte_buffer[tx_byte_buffer_index+3]=t3_usec_time.bytes[1];
  tx_byte_buffer[tx_byte_buffer_index+4]=t3_usec_time.bytes[2];
  tx_byte_buffer[tx_byte_buffer_index+5]=t3_usec_time.bytes[3];
  tx_byte_buffer[tx_byte_buffer_index+6]=t3_usec_time.bytes[4];
  tx_byte_buffer[tx_byte_buffer_index+7]=t3_usec_time.bytes[5];
  tx_byte_buffer_index=tx_byte_buffer_index+8;
  
}


