#include <EEPROM.h>
#include <Esp.h>
#include <Ticker.h>
//#include <gdbstub.h>
#include "main.h"
#include "temperature.h"
#include "generic.h"
#include "network.h"
#include "net_value.h"
#include "Queue.h"
#include "deviceinfo.h"
#include "sensor.h"
#include "util.h"
#include "debugprint.h"
#include "thingspeak.h"
#include "wdog.h"
#include "util.h"
#include "i2c_scanner.h"
#include "pcf8591.h"

extern int task_readpir(unsigned long now);
extern int task_acquire(unsigned long now);
extern int task_updatethingspeak(unsigned long now);
extern int task_flashled(unsigned long now);
extern int task_serialport_menu(unsigned long now);
extern int task_webServer(unsigned long now);

extern void printMenu(void);
extern void printExtendedMenu(void);
extern void printInfo(void);
extern void ConfigurePorts(void);

// -----------------------
// Custom configuration
// -----------------------
String ProgramInfo("Environment Sensor v0.19 Allan Inda 2016Sept04");

// Other
long count = 0;
// PIR Sensor
int PIRcount = 0;     // current PIR count
int PIRcountLast = 0; // last PIR count
unsigned long startup_millis = millis();

//----------------------------------------------------
// D0..D16, SDA, SCL defined by arduino.h
//
const uint8_t PIN_PIRSENSOR = D1;

// Reset and LED
const uint8_t PIN_SOFTRESET = D0;
const uint8_t PIN_BUILTIN_LED = BUILTIN_LED; // D0

// Port Pins
const uint8_t DIGITAL_PIN_1 = D2;
const uint8_t DIGITAL_PIN_2 = D3;
const uint8_t DIGITAL_PIN_3 = D6;
const uint8_t DIGITAL_PIN_4 = D8;
const uint8_t WATCHDOG_WOUT_PIN = D7; // toggle this pin for the external watchdog

const uint8_t ANALOG_PIN = A0; // this is the analog input pin on the ESP8266
const uint8_t I2C_SDA_PIN = 2; // Use number directly since arduino_pins.h for nodemcu has the SDA mapping wrong
const uint8_t I2C_SCL_PIN = 14; // Use number directly since arduino_pins.h for nodemcu has the SCL mapping wrong

const uint8_t ADS1115_A0 = 0;	// this is the analog input pin on the ADS1115 separate module
const uint8_t ADS1115_A1 = 1;	// this is the analog input pin on the ADS1115 separate module
const uint8_t ADS1115_A2 = 2;	// this is the analog input pin on the ADS1115 separate module
const uint8_t ADS1115_A3 = 3;	// this is the analog input pin on the ADS1115 separate module
//--------------------------------------------------

// LED on the ESP board
#define BUILTIN_LED_ON LOW
#define BUILTIN_LED_OFF HIGH

// Create Objects --- note the order of creation is important
DebugPrint debug;	// This must appear before Sensor and Device class declarations.

const int SENSOR_COUNT = MAX_PORTS; // should be at least equal to the value of MAX_PORTS
Sensor* sensors[SENSOR_COUNT] = { nullptr };

Device dinfo; // create AFTER Sensor declaration above
Queue myQueue;
bool outputTaskClock = false;
uint8_t outputTaskClockPin = BUILTIN_LED;

Adafruit_ADS1115 ads1115(0x4A); // SDA pin connected to ADDR pin for IC2 Address of 0x4A instead of 0x48
PCF8591 pcf8591;

//extern void pp_soft_wdt_stop();

///////////////////////////////////////////////////////////////////////////////////////////////
//lint -e{1784}   // suppress message about signature conflict between C++ and C
void loop(void) {
}

//lint -e{1784}   // suppress message about signature conflict between C++ and C
void setup(void) {

// Setup GPIO
	pinMode(PIN_PIRSENSOR, INPUT_PULLUP); // Initialize the PIR sensor pin as an input
	pinMode(PIN_BUILTIN_LED, OUTPUT); // Initialize the BUILTIN_LED pin as an output

// Signal that setup is proceeding
	digitalWrite(PIN_BUILTIN_LED, BUILTIN_LED_ON);

// Setup Serial port
	Serial.begin(115200);

// Start the external Watchdog. This also configures the Software Watchdog.
//	// This device uses an external watchdog, so no need for the ESP version, and it will just
//	//  cause issues.
//	ESP.wdtDisable();
//	system_soft_wdt_stop();
//	pp_soft_wdt_stop();
//	reset_config();
	kickExternalWatchdog(); // The first kick calls setup and starts the external and software watchdogs.
	Serial.print(F("Starting ... "));
	Serial.println(millis());

// Start the Debugger
//	gdbstub_init();

// Finish any object initializations -- stuff that could not go into the constructors
	dinfo.init(); // this must be done before calling any other member functions

// Start EEPROM and setup the Persisted Database
	EEPROM.begin(1536);

	/* The SDA pin is supposed to be high when not used. If during startup the pin is held low,
	 * then erase the EEPROM.
	 */
	pinMode(I2C_SDA_PIN, INPUT);
	if (digitalRead(I2C_SDA_PIN) == 0) {
		Serial.println(F("SDA held low -- erasing the EEPROM"));
		dinfo.eraseEEPROM();
	}
	else {
		Serial.println(F("If the EEPROM is corrupt, hold SDA low during startup to erase it."));
	}
	kickAllWatchdogs();

	/* Copy persisted data from EEPROM into RAM */
	dinfo.restoreDatabaseFromEEPROM();
	/* Get DebugLevel from EEPROM
	 * Copy debug level from RAM, which was just copied from the EEPROM
	 */
	debug.setDebugLevel(static_cast<DebugLevel>(dinfo.getDebugLevel()));
	/* write it back to dinfo since debug would have validated it, and potentially changed it to fix errors.*/
	dinfo.setDebugLevel(static_cast<int>(debug.getDebugLevel()));
	/* if value is invalid, then fix it and rewrite the EEPROM */
	if (eeprom_is_dirty) {
		dinfo.saveDatabaseToEEPROM();
	}
	kickAllWatchdogs();

// Setup the WebServer
	WebInit(); // Calls WiFiManager to make sure connected to access point.
	kickAllWatchdogs();

// Configure ADS1115
	/* Some "drivers" may also call Wire.begin(). That is ok so long
	 * as they use the same pins for SDA and SCL. The Wire object is a
	 * global, and calling Wire.begin() multiple times does no harm.
	 * It's called here globally for the ADS1115 which supplies the ADC for the analog
	 * pin in each port.
	 */
#ifdef ARDUINO_ESP8266_ESP12
	Wire.begin(2, 14); // the pins for SDA and SCL in nodemcu/pins_arduino.h are wrong! So specify them directly here.
#else
	Wire.begin(); // does not work for ESP8266 due to bug in ESP8266_Arduino library
#endif

// Internal ADC1115 Configuration
	// The ADC input range (or gain) can be changed via the following
	// functions, but be careful never to exceed VDD +0.3V max, or to
	// exceed the upper and lower limits if you adjust the input range!
	// Setting these values incorrectly may destroy your ADC!
	//
	ads1115.setGain(GAIN_TWOTHIRDS);  // 2/3x gain +/- 6.144V  1 bit = 0.1875mV (default)
	// ads1115.setGain(GAIN_ONE);        // 1x gain   +/- 4.096V  1 bit = 0.125mV
	// ads1115.setGain(GAIN_TWO);        // 2x gain   +/- 2.048V  1 bit = 0.0625mV
	// ads1115.setGain(GAIN_FOUR);       // 4x gain   +/- 1.024V  1 bit = 0.03125mV
	// ads1115.setGain(GAIN_EIGHT);      // 8x gain   +/- 0.512V  1 bit = 0.015625mV
	// ads1115.setGain(GAIN_SIXTEEN);    // 16x gain  +/- 0.256V  1 bit = 0.0078125mV
	ads1115.begin();

// Configure the PCF8591 YL-40 Module if present. It may or may not be connected.
#ifdef PCF8591_FIXME
	pcf8591.disableDAC();
	pcf8591.setInputs(pcf8591_AnInput_Type::four_single_ended);
	pcf8591.disableAutoIncrementChannel();
	pcf8591.begin();
#endif

// Configure Sensors
	ConfigurePorts();
	kickAllWatchdogs();

// Configure Scheduler, scheduleFunction (function pointer, task name, start delay in ms, repeat interval in ms)
// WARNING: Tasks must run minimally every 30 seconds to prevent software watchdog timer resets
	// A lot of the times are prime numbers to reduce the chance of overlaps
	myQueue.scheduleFunction(task_readpir, "PIR", 501, 43);
	myQueue.scheduleFunction(task_acquire, "acquire", 997, 47);
	myQueue.scheduleFunction(task_updatethingspeak, "thingspeak", 1997, 1000); // 1 second resolution
	myQueue.scheduleFunction(task_flashled, "led", 251, 97);
	myQueue.scheduleFunction(task_serialport_menu, "menu", 1993, 503);
	myQueue.scheduleFunction(task_webServer, "webserver", 2999, 11);

// Print boot up information and menu
	printInfo();
	printMenu();

// Signal that setup is complete
	digitalWrite(PIN_BUILTIN_LED, BUILTIN_LED_OFF);

// Start the task scheduler
	setStartupComplete(); // tell the watchdog the long startup is done and to use normal watchdog timeouts
	kickAllSoftwareWatchdogs();
	Serial.println("Startup complete" + nl);
	bool pol = false;
	for (;;) {
		softwareWatchdog(); // must call minimally every 1.6s to avoid the external watchdog reseting the ESP8266
		myQueue.Run(millis());
		if (outputTaskClock) {
			if (pol) {
				digitalWrite(outputTaskClockPin, 0);
			}
			else {
				digitalWrite(outputTaskClockPin, 1);
			}
			pol = !pol;
		}
		//yield();
		optimistic_yield(10000);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
void ConfigurePorts(void) {
// Each port has access to a predefined subset of the following pin types, and for
//    each port, the pin assignments may differ, and some will be the same.
	SensorPins p;

// Loop through each of the ports
	for (int portNumber = 0; portNumber < dinfo.getPortMax(); portNumber++) {
		// Get the pins used for this port
		DEBUGPRINTLN(DebugLevel::TIMINGS, "portNumber=" + String(portNumber));
		switch (portNumber) {
			case 0: // port#0
				p.digital = DIGITAL_PIN_1;
				p.analog = ADS1115_A0;
				p.sda = I2C_SDA_PIN;
				p.scl = I2C_SCL_PIN;
				break;
			case 1: // port#1
				p.digital = DIGITAL_PIN_2;
				p.analog = ADS1115_A1;
				p.sda = I2C_SDA_PIN;
				p.scl = I2C_SCL_PIN;
				break;
			case 2: // port#2
				p.digital = DIGITAL_PIN_3;
				p.analog = ADS1115_A2;
				p.sda = I2C_SDA_PIN;
				p.scl = I2C_SCL_PIN;
				break;
			case 3: // port#3
				p.digital = DIGITAL_PIN_4;
				p.analog = ADS1115_A3;
				p.sda = I2C_SDA_PIN;
				p.scl = I2C_SCL_PIN;
				break;
			default:
				DEBUGPRINT(DebugLevel::ERROR, F("ERROR: ConfigurePorts() - port number out of range -"));
				DEBUGPRINTLN(DebugLevel::ERROR, portNumber);
				break;
		}

		// Figure out the configuration of the port
		//lint -e{26} suppress error false error about the static_cast
		for (int portType = 0; portType < static_cast<int>(sensorModule::END); portType++) {
			// loop through each of the port setting types until finding a match
			//lint -e{26} suppress error false error about the static_cast
			if (dinfo.getPortMode(portNumber) == static_cast<sensorModule>(portType)) {
				char name[20];
				strncpy(name, sensorList[static_cast<int>(portType)].name, 19);
				name[19] = 0;
				// found the setting
				DEBUGPRINTLN(DebugLevel::INFO,
						"Configuring Port#" + String(portNumber) + ", name= " + String(name) + ", type="
								+ getSensorModuleName(static_cast<sensorModule>(portType)));
				//lint -e{30, 142} suppress error due to lint not understanding enum classes
				if (portNumber >= 0 && portNumber < SENSOR_COUNT) {
//					for (int ii = 0; ii < MAX_ADJ; ii++) {
//						sensors[portNumber]->setCalEnable(ii, false);
//					}
					switch (portType) {
						case static_cast<int>(sensorModule::dht11):
							sensors[portNumber] = new TemperatureSensor();
							sensors[portNumber]->init(sensorModule::dht11, p);
							sensors[portNumber]->setName("DHT11");
							break;
						case static_cast<int>(sensorModule::dht22):
							sensors[portNumber] = new TemperatureSensor;
							sensors[portNumber]->init(sensorModule::dht22, p);
							sensors[portNumber]->setName("DHT22");
							break;
						case static_cast<int>(sensorModule::ds18b20):
							sensors[portNumber] = new TemperatureSensor;
							sensors[portNumber]->init(sensorModule::ds18b20, p);
							sensors[portNumber]->setName("DS18b20");
							break;
						case static_cast<int>(sensorModule::analog):
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::analog, p);
							sensors[portNumber]->setName("Analog");
							break;
						case static_cast<int>(sensorModule::digital):
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::digital, p);
							sensors[portNumber]->setName("Digital");
							break;
						case static_cast<int>(sensorModule::analog_digital):
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::analog_digital, p);
							sensors[portNumber]->setName("Analog+Digital");
							break;
						case static_cast<int>(sensorModule::htu21d_si7102):
							sensors[portNumber] = new TemperatureSensor;
							sensors[portNumber]->init(sensorModule::htu21d_si7102, p);
							sensors[portNumber]->setName("HTU21D/Si7102");
							break;
						case static_cast<int>(sensorModule::sonar):
							break;
						case static_cast<int>(sensorModule::sound):
							break;
						case static_cast<int>(sensorModule::reed):
							break;
						case static_cast<int>(sensorModule::hcs501):
							break;
						case static_cast<int>(sensorModule::hcsr505):
							break;
						case static_cast<int>(sensorModule::Sharp_GP2Y10_DustSensor):
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::Sharp_GP2Y10_DustSensor, p);
							sensors[portNumber]->setName("Sharp GP2Y10");
							break;
						case static_cast<int>(sensorModule::rain):
							break;
						case static_cast<int>(sensorModule::soil):
							break;
						case static_cast<int>(sensorModule::soundh):
							break;
						case static_cast<int>(sensorModule::methane):
							break;
						case static_cast<int>(sensorModule::gy68_BMP180):
							break;
						case static_cast<int>(sensorModule::gy30_BH1750FVI):
							break;
						case static_cast<int>(sensorModule::lcd1602):
							break;
						case static_cast<int>(sensorModule::rfid):
							break;
						case static_cast<int>(sensorModule::marquee):
							break;
						case static_cast<int>(sensorModule::taskclock):
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::taskclock, p);
							sensors[portNumber]->setName("taskclock");
							outputTaskClock = true;
							outputTaskClockPin = static_cast<uint8_t>(p.digital); // yes. it overwrites if already set in a different port
							break;
						case static_cast<int>(sensorModule::off):
						default:
							sensors[portNumber] = new GenericSensor();
							sensors[portNumber]->init(sensorModule::off, p);
							sensors[portNumber]->setName("off");
							break;
					} // switch (portType)
					  // If sensor object created, then set the ID number
					if (sensors[portNumber]) sensors[portNumber]->setID(portNumber);
				} // if (portNumber...)
				else {
					DEBUGPRINT(DebugLevel::ERROR, F("ERROR: ConfigurePort() - portNumber out of range - "));
					DEBUGPRINTLN(DebugLevel::ERROR, portNumber);
				}
			} // if (portMode ...)
		} // for (portType ...)

	} // for (portNumber ...)

// Copy the calibration data to the sensors
	CopyCalibrationDataToSensors();
}

void CopyCalibrationDataToSensors(void) {
// Note: This routine assumes the SENSOR_COUNT and getPortMax() are identical values
	for (int idx = 0; idx < SENSOR_COUNT && idx < dinfo.getPortMax(); idx++) {
		if (sensors[idx] /* make sure it exists*/) {
			// copy each of the multiple calibration values into the sensor
			for (int i = 0; i < getSensorCalCount() && i < dinfo.getPortAdjMax(); i++) {
				sensors[idx]->setCal(i, static_cast<float>(dinfo.getPortAdj(idx, i)));
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////

String getsDeviceInfo(String eol) {
	String s(ProgramInfo + eol);
	s += "Hostname: " + WiFi.hostname() + eol;
	s += "Device MAC: " + WiFi.macAddress() + eol;
	s += "Device IP: " + localIPstr() + eol;
	s += "AP MAC: " + WiFi.BSSIDstr() + eol;
	s += "SSID: " + WiFi.SSID() + eol;
//	s += "psd: " + WiFi.psk() + eol; // wifi password
	s += "RSSI: " + String(WiFi.RSSI()) + " dBm" + eol;
	s += "ESP8266_Device_ID=" + String(dinfo.getDeviceID()) + eol;
	s += "Friendly Name: " + String(dinfo.getDeviceName()) + eol;
	s += "Temperature Units: " + String(getTempUnits(dinfo.isFahrenheit())) + eol;
	s += "Uptime: " + getTimeString(millis() - startup_millis) + eol;
	return s;
}

String getsSensorInfo(String eol) {
	String s("");
	for (int sensor_number = 0; sensor_number < dinfo.getPortMax() && sensor_number < SENSOR_COUNT;
			sensor_number++) {
		s += eol + "Port #" + String(sensor_number);
		s += eol + "Type: " + getSensorModuleName(dinfo.getPortMode(sensor_number));
		s += eol + sensors[sensor_number]->getsInfo(eol);
	}
	return s;
}

///////////////////////////////////////////////////////////////////////////////////////////////
void printInfo(void) {
// Print useful Information
	Serial.println(getsDeviceInfo(nl));
	Serial.println(getsThingspeakInfo(nl));
	Serial.println(F("== Port Information =="));
	dinfo.printInfo();
}

///////////////////////////////////////////////////////////////////////////////////////////////
int task_readpir(unsigned long now) {
	wdog_timer[static_cast<int>(taskname_pir)] = now; // kick the software watchdog
	if (digitalRead(PIN_PIRSENSOR)) {
		PIRcount++;
	}
	return 0;
}

int task_acquire(unsigned long starting_time) {
	wdog_timer[static_cast<int>(taskname_acquire)] = starting_time; // kick the software watchdog

	static int next_sensor_to_acquire = 0;
	static const unsigned long minimum_work_per_this_slice_ms = 13; // keep working until at least this much time is elapsed.
	unsigned long ending_time = starting_time + minimum_work_per_this_slice_ms;
	bool no_time_left = false;

	while (no_time_left == false) {
		if (next_sensor_to_acquire >= SENSOR_COUNT) {
			next_sensor_to_acquire = 0;
		}
		DEBUGPRINT(DebugLevel::TIMINGS, String(millis()) + " sensors[" + String(next_sensor_to_acquire));

		sensors[next_sensor_to_acquire]->acquire();
		next_sensor_to_acquire++;

		// Determine if there is more time left in the time slide for this task.
		if (millis() > ending_time) {
			no_time_left = true;
			DEBUGPRINTLN(DebugLevel::TIMINGS, "DONE");
		}
	}
	return 0;
}

int task_updatethingspeak(unsigned long now) {
	wdog_timer[static_cast<int>(taskname_thingspeak)] = now; // kick the software watchdog
	if (now > (last_thingspeak_update_time_ms + dinfo.getThingspeakUpdatePeriodMS())) {
		last_thingspeak_update_time_ms = now;
		if (dinfo.getThingspeakEnable()) {
			updateThingspeak();
		}
		PIRcountLast = PIRcount;
		PIRcount = 0; // reset counter for starting a new period
	}
	return 0;
}

int task_flashled(unsigned long now) {
	wdog_timer[static_cast<int>(taskname_led)] = now; // kick the software watchdog
	static uint8_t current_state = 0;
	if (current_state == 0) {
		digitalWrite(BUILTIN_LED, BUILTIN_LED_ON);
		current_state = 1;
	}
	else {
		digitalWrite(BUILTIN_LED, BUILTIN_LED_OFF);
		current_state = 0;
	}
	return 0;
}

void printMenu(void) {
	Serial.println(F("MENU ------------------------------"));
	Serial.println(F("?  show this menu"));
	Serial.println(F("i  show High-level configuration"));
	Serial.println(F("t  show Thingspeak Channel Settings"));
	Serial.println(F("c  show calibration values"));
	Serial.println(F("m  show measured values"));
	Serial.println(F("rR show chart of values (raw)"));
	Serial.println(F("sS show chart of values"));
	Serial.println(F("w  show web URLs"));
	Serial.println(F("p  push Thingspeak Channel Settings"));
	Serial.println(F("z  Extended menu"));
	Serial.println("");
}

void printExtendedMenu(void) {
	Serial.println(F("MENU EXTENDED ---------------------"));
	Serial.println(F("A  show Sensor debug info"));
	Serial.println(F("B  show reason for last reset"));
	Serial.println(F("C  write defaults to configuration memory"));
	if (DEBUGPRINT_ENABLED) { //lint !e774
		Serial.print("dD  [" + debug.getDebugLevelString());
		Serial.println(F("] Debug level for logging to serial port"));
	}
	Serial.println(F("E  show data structure in EEPROM"));
	Serial.println(F("I  show ESP information"));
	Serial.println(F("M  show data structure in RAM"));
	Serial.println(F("P  read pin values"));
	Serial.println(F("T  test PCF8591"));
	Serial.println(F("V  Scan I2C Bus"));
	Serial.println(F("W  Test Watchdog (block here forever)"));
	Serial.println(F("X  EspClass::reset()"));
	Serial.println(F("Y  EspClass::restart()"));
	Serial.println(F("Z  show size of datatypes"));
	Serial.println("");
}

int task_serialport_menu(unsigned long now) {
	wdog_timer[static_cast<int>(taskname_menu)] = now; // kick the software watchdog
	count++;
	static bool need_new_heading = true;
	static bool raw_need_new_heading = true;
	static bool T_need_new_heading = true;
	static bool continueS = false;
	static bool continueR = false;
	static bool continueT = false;
	char ch = 0;

// Get next keystroke
	if ((continueS == false && continueR == false && continueT == false) || Serial.available()) {
		ch = static_cast<char>(Serial.read());
		continueS = false;
		continueR = false;
		continueT = false;
	}
	else {
		if (continueS) ch = 's';
		else if (continueR) ch = 'r';
		else if (continueT) ch = 'T';
		else {
			continueS = false;
			continueR = false;
			continueT = false;
		}
	}

// Determine if new heading is needed
	if (ch != 's') {
		need_new_heading = true;
	}
	if (ch != 'r') {
		raw_need_new_heading = true;
	}
	if (ch != 'T') {
		T_need_new_heading = true;
	}
	if (ch > 0) {
// Process the keystroke
		switch (ch) {
			case '?':
				printMenu();
				break;
			case 'S':
				continueS = true;
				break;
			case 'R':
				continueR = true;
				break;
			case 'z':
				printExtendedMenu();
				break;
			case 'i':
				printInfo();
				break;
			case 't':
				Serial.println(getsThingspeakChannelInfo(nl));
				break;
			case 'p':
				Serial.println("--- Push Thingspeak Channel Settings to Server");
				Serial.println(ThingspeakPushChannelSettings(nl, false));
				break;
			case 'r':
				// Display the heading
				if (raw_need_new_heading) {
					raw_need_new_heading = false;
					Serial.print(F("COUNT   | PIR "));
					for (int s = 0; s < SENSOR_COUNT; s++) {
						if (sensors[s]) {
							for (int v = 0; v < getSensorValueCount(); v++) {
								if (sensors[s]->getValueEnable(v)) {
									Serial.print(F("| "));
									Serial.print(
											padEndOfString("Raw/" + sensors[s]->getValueName(v), 12, ' ',
													true));
								}
							}
						}
					}
					Serial.println("");
				}
				// Display the Data
				Serial.print("#" + padEndOfString(String(count), 6, ' ') + " | ");
				Serial.print(padEndOfString(String(PIRcount), 4, ' '));
				for (int s = 0; s < SENSOR_COUNT; s++) {
					if (sensors[s]) {
						for (int v = 0; v < getSensorValueCount(); v++) {
							if (sensors[s]->getValueEnable(v)) {
								Serial.print(F("| "));
								Serial.print(
										padEndOfString(
												String(
														String(sensors[s]->getRawValue(v)) + "/"
																+ String(sensors[s]->getValue(v))), 12, ' ',
												true));
							}
						}
					}
				}
				Serial.println("");
				break;
			case 's':
				// Display the heading
				if (need_new_heading) {
					need_new_heading = false;
					Serial.print(F("COUNT   | PIR "));
					for (int s = 0; s < SENSOR_COUNT; s++) {
						if (sensors[s]) {
							Serial.print("| ");
							for (int v = 0; v < getSensorValueCount(); v++) {
								if (sensors[s]->getValueEnable(v)) {
									Serial.print(padEndOfString(sensors[s]->getValueName(v), 6, ' ', true));
								}
							}
						}
					}
					Serial.println("");
				}
				// Display the Data
				Serial.print("#" + padEndOfString(String(count), 6, ' ') + " | ");
				Serial.print(padEndOfString(String(PIRcount), 4, ' '));
				for (int s = 0; s < SENSOR_COUNT; s++) {
					if (sensors[s]) {
						Serial.print(F("| "));
						for (int v = 0; v < getSensorValueCount(); v++) {
							if (sensors[s]->getValueEnable(v)) {
								Serial.print(padEndOfString(String(sensors[s]->getValue(v)), 6, ' ', true));
							}
						}
					}
				}
				Serial.println("");
				break;
			case 'c':
				Serial.println(F("=== Calibration Data ==="));
				for (int i = 0; i < SENSOR_COUNT; i++) {
					if (sensors[i]) {
						sensors[i]->printCals();
					}
				}
				break;
			case 'm':
				Serial.println(F("=== Value Data ==="));
				for (int i = 0; i < SENSOR_COUNT; i++) {
					if (sensors[i]) {
						sensors[i]->printValues();
					}
				}
				break;
			case 'w':
				Serial.println(WebPrintInfo(nl));
				break;
///// Extended Menu ////
			case 'C':
				dinfo.eraseEEPROM();
				Serial.print(nl);
				Serial.println(F("EEPROM Erased."));
				dinfo.writeDefaultsToDatabase();
				Serial.print(nl);
				Serial.println(F("Defaults written to Database."));
				dinfo.saveDatabaseToEEPROM();
				Serial.print(nl);
				Serial.println(F("Database written to EEPROM."));
				break;
			case 'D':
				if (DEBUGPRINT_ENABLED) { //lint !e774
					dinfo.setDebugLevel(static_cast<int>(debug.decrementDebugLevel()));
					Serial.println("Debug Level set to: " + debug.getDebugLevelString());
					if (eeprom_is_dirty) dinfo.saveDatabaseToEEPROM();
				}
				else {
					Serial.println(F("Debug statements not in binary"));
				}
				break;
			case 'd':
				if (DEBUGPRINT_ENABLED) { //lint !e774
					dinfo.setDebugLevel(static_cast<int>(debug.incrementDebugLevel()));
					Serial.println("Debug Level set to: " + debug.getDebugLevelString());
					if (eeprom_is_dirty) dinfo.saveDatabaseToEEPROM();
				}
				else {
					Serial.println(F("Debug statements not in binary"));
				}
				break;
			case 'E': // Read the EEPROM into the RAM data structure, then dump the contents
				Serial.print(nl);
				Serial.println(F("=== Data in EEPROM ==="));
				dinfo.restoreDatabaseFromEEPROM();
				Serial.println(dinfo.databaseToString(nl.c_str()));
				Serial.println("");
				break;
			case 'M': // Just dump the contents of the RAM data structure
				Serial.print(nl);
				Serial.println(F("=== Data in RAM ==="));
				Serial.println(dinfo.databaseToString(nl.c_str()));
				Serial.println("");
				break;
			case 'P':
				Serial.println(F("=== Current Port pin values ==="));
				for (int portNumber = 0; portNumber < dinfo.getPortMax(); portNumber++) {
					int d = sensors[portNumber]->getDigitalPin();
					int a = sensors[portNumber]->getAnalogPin();
					if (a < 0 || a > 3) a = 0;
					Serial.println("Port #" + String(portNumber));
					Serial.print("  Digital (" + String(d) + "):\t");
					if (digitalRead(static_cast<uint8_t>(d))) Serial.println("HIGH");
					else Serial.println("LOW");
					Serial.print("  Analog  (" + String(a) + "):\t");
					float ar = ads1115.readADC_SingleEnded(static_cast<uint8_t>(a));
					Serial.print(String(ar) + " or ");
					Serial.println(String(ar * ads1115.getVoltsPerCount() * 1000) + " mV");
				}
				Serial.println(F("=== PCF8591 8-bit ADC (if present) ==="));
				for (int p = 0; p < PCF8591_ADC_MAX_CHANNELS; p++) {
					int d = pcf8591.readADC(p);
					Serial.print("A" + String(p) + ":\t");
					Serial.print(d);
					Serial.print(" or ");
					Serial.println(String(d * pcf8591.getVoltsPerCount() * 1000) + " mV");
				}
				break;
			case 'T':
				if (T_need_new_heading) Serial.println("=== PCF8591 Test ===");
				test_pcf8591();
				T_need_new_heading = false;
				continueT = true;
				break;
			case 'I':
				Serial.print(F("CycleCount:\t\t"));
				Serial.println(String(ESP.getCycleCount()));

				Serial.print(F("Vcc:\t\t\t"));
				Serial.println(String(ESP.getVcc()));

				Serial.print(F("FreeHeap:\t\t0x"));
				Serial.println(ESP.getFreeHeap(), HEX);

				Serial.print(F("ChipId:\t\t\t0x"));
				Serial.println(ESP.getChipId(), HEX);

				Serial.print(F("SdkVersion:\t\t"));
				Serial.println(String(ESP.getSdkVersion()));

				Serial.print(F("BootVersion:\t\t"));
				Serial.println(String(ESP.getBootVersion()));

				Serial.print(F("BootMode:\t\t"));
				Serial.println(String(ESP.getBootMode()));

				Serial.print(F("CpuFreqMHz:\t\t"));
				Serial.println(String(ESP.getCpuFreqMHz()));

				Serial.print(F("FlashChipId:\t\t0x"));
				Serial.println(ESP.getFlashChipId(), HEX);

				Serial.print(F("FlashChipRealSize[Mbit]:0x"));
				Serial.println(ESP.getFlashChipRealSize(), HEX);

				Serial.print(F("FlashChipSize [MBit]:\t0x"));
				Serial.println(ESP.getFlashChipSize(), HEX);

				Serial.print(F("FlashChipSpeed [Hz]:\t"));
				Serial.println(String(ESP.getFlashChipSpeed()));

				Serial.print(F("FlashChipMode:\t\t"));
				switch (ESP.getFlashChipMode()) {
					case FM_QIO:
						Serial.println(F("QIO"));
						break;
					case FM_QOUT:
						Serial.println(F("QOUT"));
						break;
					case FM_DIO:
						Serial.println(F("DIO"));
						break;
					case FM_DOUT:
						Serial.println(F("DOUT"));
						break;
					case FM_UNKNOWN:
					default:
						Serial.println(F("Unknown"));
						break;
				}
				Serial.print(F("FlashChipSizeByChipId:\t0x"));
				Serial.println(ESP.getFlashChipSizeByChipId(), HEX);

				Serial.print(F("SketchSize [Bytes]:\t0x"));
				Serial.println(ESP.getSketchSize(), HEX);

				Serial.print(F("FreeSketchSpace [Bytes]:0x"));
				Serial.println(ESP.getFreeSketchSpace(), HEX);

				Serial.print(F("sizeof(dinfo) [Bytes]:\t0x"));
				Serial.println(dinfo.getDatabaseSize(), HEX);

				Serial.println("");
				break;
			case 'B':
				Serial.print(F("Last Reset --> "));
				Serial.println(ESP.getResetInfo() + nl);
				break;
			case 'A':
				Serial.print(nl);
				Serial.println(F("Sensor Debug Information"));
				Serial.println(getsSensorInfo(nl));
				break;
			case 'V':
				scanI2CBus();
				break;
			case 'W':
				Serial.println(F("Looping here forever - Software Watchdog for Menu should trip ... "));
				for (;;) {
					yield();
				} // loop forever so the software watchdog for this task trips, but yield to the ESP OS
				break;
			case 'X':
				Serial.println(F("Calling EspClass::reset()"));
				ESP.reset();
				break;
			case 'Y':
				Serial.println(F("Calling EspClass::restart()"));
				ESP.restart();
				break;
			case 'Z':
				Serial.println(F("Datatype sizes in bytes:"));
				Serial.print(F("bool:        "));
				Serial.println(sizeof(bool));
				Serial.print(F("char:        "));
				Serial.println(sizeof(char));
				Serial.print(F("wchar_t:     "));
				Serial.println(sizeof(wchar_t));
				Serial.print(F("short:       "));
				Serial.println(sizeof(short));
				Serial.print(F("int:         "));
				Serial.println(sizeof(int));
				Serial.print(F("long:        "));
				Serial.println(sizeof(long));
				Serial.print(F("long long:   "));
				Serial.println(sizeof(long long));
				Serial.print(F("float:       "));
				Serial.println(sizeof(float));
				Serial.print(F("double:      "));
				Serial.println(sizeof(double));
				Serial.print(F("long double: "));
				Serial.println(sizeof(long double));
				Serial.print(F("char*:       "));
				Serial.println(sizeof(char*));
				break;
			default:
				//Serial.println("Unknown command: " + String(ch));
				break;
		}
	}
	return 0;
}

int task_webServer(unsigned long now) {
	wdog_timer[static_cast<int>(taskname_webserver)] = now; // kick the software watchdog
	WebWorker();
	return 0;
}

