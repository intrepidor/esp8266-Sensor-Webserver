#include <Arduino.h>
#include "main.h"
#include "util.h"
#include "pcf8591.h"
#include "generic.h"

void GenericSensor::init(sensorModule m, SensorPins& p) {
	/* The generic sensor has two inputs, one is analog, the other digital. Each
	 * input corresponds to a different channel, 0 and 1 respectively. The analog
	 * value is read first as a raw value, then converted to a measured value
	 * using the equation  measured = offset + a*raw + b*raw^2 + c*raw^3.
	 */
	setModule(m);
	setPins(p);

	// Configure Sensor Parameters
	setStaleAge_ms(10000);	// timeout of the value if not update for 10 seconds.

	// These are the defaults.
	setCalEnable(TEMP_CAL_CHANNEL_ANALOG_OFFSET, false);
	setCalName(TEMP_CAL_CHANNEL_ANALOG_OFFSET, "Offset");
	setCalEnable(TEMP_CAL_CHANNEL_ANALOG_FIRST_ORDER, false);
	setCalName(TEMP_CAL_CHANNEL_ANALOG_FIRST_ORDER, "1st order Gain");
	setCalEnable(TEMP_CAL_CHANNEL_ANALOG_SECOND_ORDER, false);
	setCalName(TEMP_CAL_CHANNEL_ANALOG_SECOND_ORDER, "2nd order Gain");
	setCalEnable(TEMP_CAL_CHANNEL_ANALOG_THIRD_ORDER, false);
	setCalName(TEMP_CAL_CHANNEL_ANALOG_THIRD_ORDER, "3rd order Gain");

	setValueEnable(TEMP_VALUE_CHANNEL_ANALOG, false);
	setValueName(TEMP_VALUE_CHANNEL_ANALOG, "Analog");
	setValueEnable(TEMP_VALUE_CHANNEL_DIGITAL, false);
	setValueName(TEMP_VALUE_CHANNEL_DIGITAL, "Digital");

	if (m == sensorModule::analog || m == sensorModule::analog_digital
			|| m == sensorModule::Sharp_GP2Y10_DustSensor) {
		setCalEnable(TEMP_CAL_CHANNEL_ANALOG_OFFSET, true);
		setCalEnable(TEMP_CAL_CHANNEL_ANALOG_FIRST_ORDER, true);
		setCalEnable(TEMP_CAL_CHANNEL_ANALOG_SECOND_ORDER, true);
		setCalEnable(TEMP_CAL_CHANNEL_ANALOG_THIRD_ORDER, true);
		setValueEnable(TEMP_VALUE_CHANNEL_ANALOG, true);
	}

	if (m == sensorModule::digital || m == sensorModule::analog_digital) {
		setValueEnable(TEMP_VALUE_CHANNEL_DIGITAL, true);
	}

	if (m == sensorModule::taskclock) {
		// none are used
	}

	if (m == sensorModule::Sharp_GP2Y10_DustSensor) {
		setValueName(TEMP_VALUE_CHANNEL_ANALOG, "Dust mg/m^3");
	}

	// The pins used to interact with the sensor
	digital_pin = static_cast<uint8_t>(p.digital);
	analog_pin = static_cast<uint8_t>(p.analog);
}

String GenericSensor::getsInfo(String eol) {
	String s("Class: GenericSensor" + eol);

	s += ".digital_pin: (" + String(digital_pin) + ") " + GPIO2Arduino(digital_pin) + eol;
	s += ".analog_pin: (" + String(analog_pin) + ") " + GPIO2Arduino(analog_pin) + eol;
	s += ".started: ";
	if (started) {
		s += "true";
	}
	else {
		s += "false";
	}
	s += eol;
	if (started) {
		;
	}
	return s;
}

bool GenericSensor::acquire_setup(void) {
	if (started) {
		return acquire1();
	}
	else {
		if (getModule() == sensorModule::analog || getModule() == sensorModule::analog_digital) {
			DEBUGPRINTLN(DebugLevel::TIMINGS, String(millis()) + ", setup() " + String(analog_pin));
			started = true;
		}
		if (getModule() == sensorModule::digital || getModule() == sensorModule::analog_digital) {
			DEBUGPRINTLN(DebugLevel::TIMINGS, String(millis()) + ", setup() " + String(digital_pin));
			pinMode(digital_pin, INPUT);
			started = true;
		}
		if (getModule() == sensorModule::taskclock || getModule() == sensorModule::Sharp_GP2Y10_DustSensor) {
			DEBUGPRINTLN(DebugLevel::TIMINGS, String(millis()) + ", setup() " + String(digital_pin));
			pinMode(digital_pin, OUTPUT);
			digitalWrite(digital_pin, HIGH); // Turn off the LED
			started = true;
		}
		if (getModule() == sensorModule::off) {
			started = true;
		}
	}
	return true;
}

bool GenericSensor::acquire1(void) {
	if (started) {
		if (getModule() == sensorModule::analog || getModule() == sensorModule::analog_digital) {
			uint16_t a = ads1115.readADC_SingleEnded(analog_pin);

			// raw, non-corrected, values
			setRawAnalog(static_cast<float>(a));
			double a1 = a * ads1115.getVoltsPerCount();
			double a2 = a1 * a1;
			double a3 = a2 * a1;

			a1 = getCal(TEMP_CAL_CHANNEL_ANALOG_OFFSET) + a1 * getCal(TEMP_CAL_CHANNEL_ANALOG_FIRST_ORDER)
					+ a2 * getCal(TEMP_CAL_CHANNEL_ANALOG_SECOND_ORDER)
					+ a3 * getCal(TEMP_CAL_CHANNEL_ANALOG_THIRD_ORDER);

			// Calibration corrected values
			setAnalog(static_cast<float>(a1));
		}
		if (getModule() == sensorModule::digital || getModule() == sensorModule::analog_digital) {
			int d = digitalRead(digital_pin);
			if (d == 0) {
				setDigital(0);
			}
			else {
				setDigital(1);
			}
		}
		if (getModule() == sensorModule::Sharp_GP2Y10_DustSensor) {
			/*
			 * To take a reading:
			 * 1. Turn on the IR LED by outputting a LOW on the digital pin.
			 * 2. Wait 0.28ms.
			 * 3. Read the analog output. Finish this step within 0.04ms.
			 * 4. Turn off the IR LED.
			 * Note: The IR LED should be on for 0.32 +/- 0.02 ms.
			 * 5. Convert the analog voltage into a measurement of dust
			 *    density as mg/m^3 using predetermined formulas.
			 * 6. Don't read the sensor faster than once per 10ms
			 */
			pcf8591.config(2);	// preconfigure the channel for the ADC.
			const size_t BYTES_TO_READ = 5;

			/* Turn on the LED, then read 5 samples from the ADC. The first sample
			 *    represents data from a prior cycle, so discard it. Take the remaining
			 *    samples and find the highest value. That will be the result.
			 */
			digitalWrite(digital_pin, LOW); // power on the LED
			/* The requestFrom is a complete operation. It sends the Start, IC2Addr, Read bit,
			 *    does the specified number of reads, then sends the stop. You access the results
			 *    by making repeated calls to Wire.read(). The call to Wire.read() does not
			 *    actually do the read, but retrieves a value already read and stored in a buffer.
			 */
			Wire.requestFrom(pcf8591.getAddr(), BYTES_TO_READ, true /* send stop */); // takes 600us(5 bytes)
			digitalWrite(digital_pin, HIGH);

			// Discard the first sample, and find the maximum value of the other samples.
			int a = 0, b = 0;
			int sum = 0;
			float avg = 0;
			DEBUGPRINT(DebugLevel::SHARPGP2Y10, "GP2Y10 A2: ");
			for (int i = 0; i < BYTES_TO_READ; i++) {
				b = Wire.read();
				if (i > 0) {
					if (b > a) a = b;
					sum += b;
					DEBUGPRINT(DebugLevel::SHARPGP2Y10, String(b) + " ");
				}
			}
			avg = sum / static_cast<float>(BYTES_TO_READ - 1);
//			if (avg < 1) avg = 1;
			DEBUGPRINTLN(DebugLevel::SHARPGP2Y10,
					": max/avg/ratio:\t" + String(a) + "\t " + String(sum) + "\t" + String(avg) + "\t"
							+ String(avg / a));

#if(0)
			int a = 0, b=0;
			digitalWrite(digital_pin, LOW); // power on the LED
			for (int i = 0; i < 5; i++) {
				b = pcf8591.readADC(); // takes about 700uS
				digitalWrite(digital_pin, HIGH);
				digitalWrite(digital_pin, LOW);
//				if (b > a) a = b;
			}
			digitalWrite(digital_pin, HIGH); // Turn off the LED
#endif

#if(0)
			int a = 0;
			digitalWrite(digital_pin, LOW); // power on the LED
			delayMicroseconds(280);// Wait for the output sample to present itself. 0.28ms per data sheet

			// Make a marker for timing with the scope
			digitalWrite(digital_pin, HIGH);
			digitalWrite(digital_pin, LOW);

			// Measure the result
			a = pcf8591.readADC(2);// takes about 700uS
			digitalWrite(digital_pin, HIGH);// Turn off the LED
#endif

			// convert to millivolts
			double a0 = static_cast<double>(a) * pcf8591.getVoltsPerCount();

			// convert to dust density and store as raw value
			// linear equation from Chris Nafis http://www.howmuchsnow.com/arduino/airquality/
			double a1 = a0; // (0.17 * a0 - 0.1) * 1000;
			setRawAnalog(static_cast<float>(a1));

			// Apply the user calibrations
			double a2 = a1 * a1; // precalculate square term
			double a3 = a2 * a1; // precalculate cubed term

			a1 = getCal(TEMP_CAL_CHANNEL_ANALOG_OFFSET) + a1 * getCal(TEMP_CAL_CHANNEL_ANALOG_FIRST_ORDER)
					+ a2 * getCal(TEMP_CAL_CHANNEL_ANALOG_SECOND_ORDER)
					+ a3 * getCal(TEMP_CAL_CHANNEL_ANALOG_THIRD_ORDER);

			// Calibration corrected values
			setAnalog(static_cast<float>(a1));
			delay(9); // don't do more than 1 per 10ms
		}
		if (getModule() == sensorModule::taskclock) {
			return true; // do nothing
		}
	}
	return true;
}

bool GenericSensor::acquire2(void) {
	return acquire_setup();
}
