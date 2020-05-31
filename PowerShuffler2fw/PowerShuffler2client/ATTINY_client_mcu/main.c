/*
 * ATTINY_client_mcu.c
 *
 * Created: 5/14/2020 2:09:01 PM
 * Author : mike
 * Summary: This client MCU code as-is is designed to run on one or two 4.2V single cell Li-ion battery.
 *          The algorithm will determine which ports are connected to a battery, select the lowest voltage,
 *          take the voltage reading, turn on the charger, take the second voltage reading, then report the
 *          measured voltage minus the difference to the master MCU. Every few minutes it will stop the
 *          charger to re-check OCV of the batteries. If the voltage difference between two output ports are
 *          close, the timeout to re-check OCV will be cut in half.
 */

#define F_CPU 8000000UL // 8 MHz
#define TRIGGERTIMEOUTUS     255   // minimum delay in microseconds to trigger master MCU reading
#define TRANSMITDELAYUS      32    // delay in microseconds between transmitting edges (1 bit = 1 falling and 1 rising edge)
#define POWERDEBOUNCEDELAYMS 500   // power-on debounce delay must be less than 4 seconds due to watchdog timeout
#define ADCITERATIONS        4     // Number of readings to take an average
#define ADCREADDELAYMS       10    // Delay between ADC readings
#define MAXVOLTAGEADCVALUE   232   // ADC value of maximum battery voltage (about 4.19V battery)
#define MUXONDEBOUNCEDELAYMS 125   // Delay after switching on Battery Mux
#define RECHECKVOLTAGECOUNT  66    // Number of iterations before re-checking voltages (roughly 5min with a stopwatch)
#define RECHECKVOLTAGEFAST   RECHECKVOLTAGECOUNT/2 // Recheck voltages faster when two output voltages are very close to each other
#define CHARGERDEBOUNCEDELAYMS 500 // charger turn-on debounce delay
#define MUXOFFDEBOUNCEDELAYMS  500 // Delay after switching off Battery Mux
#define WAITFORMASTERDELAYMS 750   // Wait for master response delay before turning on charger (master takes about .5s to respond)
#define DISCHARGEWAITDELAYMS 250   // Wait for power lines to stabilize when charger turns off

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>

void initializeRegisters()
{
	// Clock frequency
	CCP    = 0xD8; // write the signature to enable CPU changes per datasheet
	CLKPSR = 0x00; // 0000 0000 for prescaler 1
	
	// Watchdog timer and Sleep Mode
	WDTCSR = 0x60; // 0110 0000 Interrupt Mode and 512k prescalar (about 4 seconds)
	SMCR   = 0x05; // 0000 0101 Power-Down mode

	// Ports and pins
	// PB0 = ADC0 output battery voltage
	// PB1 = Battery MUX output (default lo)
	// PB2 = Data Output, hi idle (default hi)
	// PB3 = Charger Disable (default hi disables charger)
	// PUEB  |= _BV(0); // for debugging, enable pull-up on ADC0
	PUEB  |= _BV(3); // enable pull-up on PB3 (charger disable)
	PORTB |= _BV(2); // ensure output PB2 is hi (not transmitting / idle)
	DDRB  |= _BV(1); // 0000 0010 for Battery MUX output pin PB1
	DDRB  |= _BV(2); // 0000 0100 for Data Output pin PB2
	// DDRB  |= _BV(3); // 0000 1000 for Charger Disable output pin PB3

	_delay_ms(POWERDEBOUNCEDELAYMS); // power-on debounce
	wdt_reset();

	// ADC
	ADCSRA = 0x83; // 1000 0011 enables ADC and set prescaler 8
	// ADCSRB = 0x00; // Single Conversion Mode
	// ADMUX = 0x00; // Select ADC0 on PB0
	DIDR0 = 0x0E; // 0000 1110 disables buffers on unused ADC pins for power savings

	// Interrupts
	SREG = 0x80; // 1000 0000 to globally enable interrupts
}

unsigned short isMuxOn() // return 0 if mux is off
{
	return (PORTB & _BV(1));
}

void turnOnMux()
{
	if(isMuxOn()) return;
	PORTB |= _BV(1); // 0000 0010 set PB1
	_delay_ms(MUXONDEBOUNCEDELAYMS); // wait for steady-state
}

void turnOffMux()
{
	if(!isMuxOn()) return;
	PORTB &= ~_BV(1); // 1111 1101 clear PB1
	_delay_ms(MUXOFFDEBOUNCEDELAYMS); // wait for full disconnect
}

void toggleMux()
{
	turnOffMux();
	turnOnMux();
}

unsigned short isChargerOn() // return 0 if charger is off
{
	return (DDRB & _BV(3));
}

void turnOnCharger()
{
	if(isChargerOn()) return;
	PUEB &= ~_BV(3); // Disable pull-up resistor to save power
	DDRB |= _BV(3);  // Enable output pin PB3 to lo
	_delay_ms(CHARGERDEBOUNCEDELAYMS);
}

void turnOffCharger()
{
	if(!isChargerOn()) return;
	PUEB |= _BV(3);  // Enable pull-up resistor to fully float PB3 hi
	DDRB &= ~_BV(3); // Set PB3 to float hi to turn off client.
	_delay_ms(DISCHARGEWAITDELAYMS);
}

void startADC()
{
	ADCSRA |= 0x40; // 0100 0000 set ADSC to start single conversion mode
}

unsigned short adcInProgress()
{
	return ADCSRA & 0x40; // 0100 0000 returns 0 if adc is done, non-zero otherwise.
}

unsigned short adcValue()
{
	// 0   = 0.0V
	// 255 = 4.6V as measured
	// Each step is 0.01804V
	return (unsigned short)ADCL;
}

unsigned short getAdcValueBusyWait()
{
	startADC();
	while(adcInProgress()); // busy waits until ADC conversion is done
	return adcValue();
}

void transmitOneTick()
{
	PORTB &= ~_BV(2);
	_delay_us(TRANSMITDELAYUS);
	PORTB |= _BV(2);
	_delay_us(TRANSMITDELAYUS);
}

void triggerRead()
{
	PORTB &= ~_BV(2);
	_delay_us(TRIGGERTIMEOUTUS); // hold lo for minimum trigger count
	PORTB |= _BV(2);
	_delay_us(TRANSMITDELAYUS);
}

void outputDataError() // holds the output lo for duration of power-on
{
	PORTB &= ~_BV(2);
}

unsigned int getAdcValueBusyWaitWithAveraging()
{
	unsigned int adcvalue = 0;
	for(int i=0; i<ADCITERATIONS; i++)
	{
		if(i) _delay_ms(ADCREADDELAYMS); // Skip delaying the first reading, then delay in milliseconds
		adcvalue += (unsigned int)getAdcValueBusyWait();
	}

	return adcvalue / ADCITERATIONS; // average ADC value
}

short transmitValue(unsigned int adcvalue) // return 0 if success, 1 if error.
{
	if(adcvalue >= MAXVOLTAGEADCVALUE || adcvalue == 0) // Over-voltage or grounded ADC input, although unlikely to be grounded during operation
	{
		outputDataError();
		return 1;
	}
	else
	{
		triggerRead(); // trigger the master MCU to start reading
		for(unsigned int j=0; j < adcvalue; j++)
		{
			transmitOneTick(); // transmit 1 bit at a time
		}
	}
	return 0;
}

ISR(WDT_vect)
{
	// do nothing and wake up MCU
}

// LED status behavior and description: There are no LED for this client MCU
int main(void)
{
	// variables used to select battery, calculate offset, and store voltage values
	int adcFloatingValue, adcInitialValue0, adcInitialValue1, adcvalue, recheckMaxCount;
	initializeRegisters(); // includes a sleep delay for power-on debounce

	while(1)
	{
		wdt_reset();
		turnOnMux();
		adcInitialValue0 = getAdcValueBusyWait();  // get first (or currently selected) battery voltage
		turnOffMux();
		adcFloatingValue = getAdcValueBusyWait();  // get floating voltage (should be 0)
		turnOnMux();                               // toggle mux without waiting for turn-off delay
		adcInitialValue1 = getAdcValueBusyWait();  // get second battery voltage
		if(adcInitialValue1 <= adcFloatingValue || // toggle mux if second battery voltage is floating
		   (adcInitialValue0 < adcInitialValue1 && // or if first voltage is lower than second
		    adcInitialValue0 > adcFloatingValue))  //       and not floating
		{
			toggleMux();
		}

		// Get the voltage difference between inputs and adjust recheck max count. Smaller voltage difference, faster timeout.
		recheckMaxCount = adcInitialValue0 - adcInitialValue1;
		recheckMaxCount = (recheckMaxCount > 1 || recheckMaxCount < -1) ? RECHECKVOLTAGECOUNT : RECHECKVOLTAGEFAST;

		adcInitialValue0 = getAdcValueBusyWaitWithAveraging(); // Get more accurate OCV of selected battery
		if(adcInitialValue0 >= MAXVOLTAGEADCVALUE || adcInitialValue0 == 0) // Over-voltage or grounded ADC input
		{
			outputDataError();
			continue; // skip the rest of the while-loop and start over
		}

		wdt_reset();
		transmitValue(adcInitialValue0);
		_delay_ms(WAITFORMASTERDELAYMS); // wait and see if master MCU decides to turn off client MCU based on OCV value.
		turnOnCharger();
		adcInitialValue0 = getAdcValueBusyWaitWithAveraging() - adcInitialValue0; // get voltage offset after charger has turned on

		for(int i=1; i <= recheckMaxCount; i++)
		{
			wdt_reset();
			adcvalue = getAdcValueBusyWaitWithAveraging() - adcInitialValue0; // subtract offset from ADC reading

			// exit the loop immediately upon error or on the last iteration due to communications timing constraints
			if(transmitValue(adcvalue) || i == recheckMaxCount) break;
			ADCSRA &= 0x7F; // disable ADC
			PRR = 0x03; // turn off ADC and timers to save power
			wdt_reset();
			sleep_mode();
			PRR = 0x00; // restore power
			ADCSRA |= 0x80; // enable ADC
		}
		turnOffCharger();
	}
}

