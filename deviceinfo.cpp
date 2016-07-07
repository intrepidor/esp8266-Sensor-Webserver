/*
 * deviceinfo.cpp
 *
 *  Created on: Jan 28, 2016
 *      Author: Allan Inda
 */

#include <Arduino.h>
#include "deviceinfo.h"
#include "main.h"
#include "debugprint.h"
#include "sensor.h"

unsigned int validate_string(char* str, const char* const def, unsigned int size, int lowest, int highest) {
	bool ok = true;
	unsigned int n = 0; /*lint -e838 */
	if (str && def) {
		if (size < strlen(str)) {
			debug.println(DebugLevel::DEBUG,
					"ERROR: validate_string() string passed by pointer is shorter than the passed size value");
		}

		str[size - 1] = 0;
		for (n = 0; n < size - 1; n++) {
			if (str[n] < lowest || str[n] > highest) {
				ok = false;
				break;
			}
		}
		if (ok == false) {
			memset(str, 0, size);
			strncpy(str, def, size - 1);
			str[size - 1] = 0;
			return n;
		}
	}
	else {
		debug.println(DebugLevel::DEBUG,
				"ERROR: validate_string() value for either/both def* or str* is null");
	}
	return 0;
}

void Device::validateDatabase(void) {
	validate_string(db.device.name, "<not named>", sizeof(db.device.name), 32, 126);
	validate_string(db.thingspeak.apikey, "<no api key>", sizeof(db.thingspeak.apikey), 48, 95);
	validate_string(db.thingspeak.url, "<no url>", sizeof(db.thingspeak.url), 32, 126);
}

void Device::writeDefaultsToDatabase(void) {
	eraseDatabase();
	db.end_of_eeprom_signature = EEPROM_SIGNATURE; // special code used to detect if configuration structure exists
	setDeviceName("");
	setDeviceID(999);
	setThingspeakEnable(false);
	setThingspeakApikey("");
	setThingspeakURL("http://api.thingspeak.com");
	setThingspeakChannel("");
	setThingspeakIpaddr("184.106.153.149");
	db.debuglevel = 0;
	for (int i = 0; i < MAX_PORTS; i++) {
		setPortName(i, "");
		setPortMode(i, sensorModule::off);
		for (int j = 0; j < MAX_ADJ; j++) {
			setPortAdj(i, j, 0.0);
		}
	}
	// Do not set eeprom is dirty since these defaults are only temporary.
}

String Device::databaseToString(String eol) {
	String s;
	s = "config_version=" + String(db.end_of_eeprom_signature) + eol;
	s += "debugLevel=" + DebugPrint::convertDebugLevelToString(static_cast<DebugLevel>(getDebugLevel()))
			+ eol;
	s += "device.name=" + String(getDeviceName()) + eol;
	s += "device.id=" + String(getDeviceID()) + eol;
	s += "thingspeak.status=" + String(getThingspeakStatus()) + eol;
	s += "thingspeak.enable=" + getThingspeakEnableString() + eol;
	s += "TS_apikey=" + getThingspeakApikey() + eol;
	s += "thingspeak.url=" + getThingspeakURL() + eol;
	s += "thingspeak.channel=" + getThingspeakChannel() + eol;
	s += "thingspeak.ipaddr=" + getThingspeakIpaddr();
	for (int i = 0; i < getPortMax(); i++) {
		s += eol + "port[" + String(i) + "].name=" + getPortName(i) + ", mode=" + String(getModeStr(i));
//		s += String(", pin=") + String(db.port[i].pin); /// pin is not used
		for (int k = 0; k < MAX_ADJ; k++) {
			s += ", adj[" + String(k) + "]=";
			s += String(getPortAdj(i, k), DECIMAL_PRECISION);
		}
	}
	return s;
}

void Device::printInfo(void) {
	for (int i = 0; i < getPortMax(); i++) {
		debug.println(DebugLevel::ALWAYS, "Port#" + String(i) + ": " + getSensorName(getPortMode(i)));
	}
	debug.println(DebugLevel::ALWAYS, "");
}

void Device::setcDeviceName(const char* newname) {
	if (newname) {
		memset(db.device.name, 0, sizeof(db.device.name));
		strncpy(db.device.name, newname, sizeof(db.device.name) - 1);
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: setcDeviceName() - null value");
	}
}

static bool isValidPort(int portnum) {
	if (portnum >= 0 && portnum < MAX_PORTS) return true;
	return false;
}

const char* PROGMEM Device::getEnableStr() {
	if (db.thingspeak.enabled) return "checked";
	return " ";
}

sensorModule Device::getPortMode(int portnum) {
	if (isValidPort(portnum)) {
		return static_cast<sensorModule>(db.port[portnum].mode);
	}
	else {
		return sensorModule::off;
	}
}
void Device::setPortMode(int portnum, sensorModule _mode) {
	if (isValidPort(portnum)) {
		db.port[portnum].mode = _mode;
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::setPortMode() - invalid port");
	}
}

String Device::getModeStr(int portnum) {
	String s("");
	if (isValidPort(portnum)) {
		int m = static_cast<int>(db.port[portnum].mode);
		//lint -e{26} suppress since lint doesn't understand sensorModule C++11
		if (m >= 0 && m < static_cast<int>(sensorModule::END)) {
			s = String(String(m) + ":" + String(sensorList[m].name));
		}
		else {
			debug.print(DebugLevel::ERROR, nl + "ERROR: Device::getModeStr() - invalid mode");
		}
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::getModeStr() - invalid port");
	}
	return s;
}

void Device::setPortName(int portnum, String _n) {
	if (isValidPort(portnum)) {
		strncpy(db.port[portnum].name, _n.c_str(), sizeof(db.port[portnum].name) - 1);
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::setPortName() - invalid port");
	}
}

String Device::getPortName(int portnum) {
	if (isValidPort(portnum)) {
		return this->db.port[portnum].name;
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::getPortName() - invalid port");
	}
	return String("undefined");
}

double Device::getPortAdj(int portnum, int adjnum) {

	if (isValidPort(portnum)) {
		if (adjnum >= 0 && adjnum < getPortAdjMax()) {
			return this->db.port[portnum].adj[adjnum];
		}
		else {
			debug.println(DebugLevel::ERROR, nl + "ERROR: Device::getPortAdj() - invalid adj index");
		}
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::getPortAdj() - invalid port");
	}
	return 0.0;
}

void Device::setPortAdj(int portnum, int adjnum, double v) {
	if (isValidPort(portnum)) {
		if (adjnum >= 0 && adjnum < getPortAdjMax()) {
			db.port[portnum].adj[adjnum] = v;
		}
		else {
			debug.println(DebugLevel::ERROR, nl + "ERROR: Device::setPortAdj() - invalid adj index");
		}
	}
	else {
		debug.println(DebugLevel::ERROR, nl + "ERROR: Device::setPortAdj() - invalid port");
	}
}

String Device::getPortAdjName(int portnum, int adjnum) {
	if (portnum >= 0 && portnum < SENSOR_COUNT && adjnum >= 0 && adjnum < MAX_ADJ) {
		return sensors[portnum]->getCalName(adjnum);
	}
	return "Err";
#if 0
	// Get the Sensor type
	sensorModule sm = getPortMode(portnum);
	//lint -e{744} suppress informationals about missing default statements
	switch (sm) {
		case sensorModule::END:
		break;	// keep "Err"
		case sensorModule::off:
		n = "";
		break;
		case sensorModule::dht11:
		case sensorModule::dht22:
		switch (adjnum) {
			case TEMP_CAL_INDEX_TEMP_SLOPE:
			n = "Temp gain";
			break;
			case TEMP_CAL_INDEX_TEMP_OFFSET:
			n = "Temp offset";
			break;
			case TEMP_CAL_INDEX_HUMIDITY_SLOPE:
			n = "rH gain";
			break;
			case TEMP_CAL_INDEX_HUMIDITY_OFFSET:
			n = "rh offset";
			break;
		}
		break;
		case sensorModule::ds18b20:
		switch (adjnum) {
			case TEMP_CAL_INDEX_TEMP_SLOPE:
			n = "Temp gain";
			break;
			case TEMP_CAL_INDEX_TEMP_OFFSET:
			n = "Temp offset";
			break;
			case TEMP_CAL_INDEX_HUMIDITY_SLOPE:
			n = "not used";
			break;
			case TEMP_CAL_INDEX_HUMIDITY_OFFSET:
			n = "not used";
			break;
		}
		break;
		case sensorModule::sonar:
		case sensorModule::sound:
		case sensorModule::reed:
		case sensorModule::hcs501:
		case sensorModule::hcsr505:
		case sensorModule::dust:
		case sensorModule::rain:
		case sensorModule::soil:
		case sensorModule::soundh:
		case sensorModule::methane:
		case sensorModule::gy68:
		case sensorModule::gy30:
		case sensorModule::lcd1602:
		case sensorModule::rfid:
		case sensorModule::marquee:
		n = "FIXME";
		break;
	}
	return n;
#endif
}
