/*
* PokeBall.c
*
* Created: 02.07.19 14:19:49
* Author : micha
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/cpufunc.h>
//#include <avr/fuse.h>
// 8 MHz / 8
#define F_CPU 1000000UL
#include <util/delay.h>

FUSES =
{
	.extended =0xff,
	.high = 0xdf,
	.low = 0x42,
};



// watchdog 120 ms
#define CYCLETIME (1000/120)

/* Fuses
ULPOSCSEL = ULPOSC_32KHZ
BODPD = BOD_DISABLED
BODACT = BOD_DISABLED
SELFPRGEN = [ ]
RSTDISBL = [ ]
DWEN = [ ]
SPIEN = [X]
WDTON = [ ]
EESAVE = [ ]
BODLEVEL = <none selected>
CKDIV8 = [X]
CKOUT = [ ]
SUT_CKSEL = INTRCOSC_8MHZ_6CK_16CK_16MS

EXTENDED = 0xFF (valid)
HIGH = 0xDF (valid)
LOW = 0x42 (valid)


*/

// SW-HW Interface

// Button (6)
#define PIN_BUTTON _BV(PINA7)

// new Debug PINs A4/A5/A6 (9, 8, 7)
// ADC3 (10) 1.2V (cpu core?)
// ADC11 (2) 5.0V (led/vib voltage)

enum ballState_t {
    BALL_RESET=0,              // power on reset, reset button
    BALL_OFF=1,                // Core off
    BALL_ON=2,                 // core on, normal operation
    BALL_AUTOCATCH_WAIT=3,     // autocatch start, button must released
    BALL_AUTOCATCH_PRESSED=4,  // 5v on, button press
    BALL_AUTOCATCH_STANDBY=5,  // 5v off, release button
    BALL_AUTOCATCH_STANDBY2=6, // 5v measure, release button
    BALL_DEBUG_SYNC=7          // for debugging
} ballState;

inline void pressButton() {
    DDRA|=PIN_BUTTON;
}

inline void releaseButton() {
    DDRA&=~PIN_BUTTON;
}

inline void initButton() {
    releaseButton();
    // prepare low level
    PORTA&=~PIN_BUTTON;
}

inline void initDebugPins() {
    // Pin PA0/1/2 debug pins
    DDRA|=_BV(DDRA4)|_BV(DDRA5)|_BV(DDRA6);
}

inline void setDebugPins(enum ballState_t value) {
    PORTA=(PORTA&~(_BV(DDRA4)|_BV(DDRA5)|_BV(DDRA6)))|(((char)value&7)<<DDRA4);
}

inline void setPowerSavingOn() {
    set_sleep_mode(SLEEP_MODE_PWR_DOWN); // set power down mode, only watchdog
    power_all_disable();
    DIDR0=(char)~(PIN_BUTTON);
    DIDR1=0;
}

inline  char readButton() {
    return !(PINA&PIN_BUTTON);
}

void startComparator() {
    power_adc_enable();
    //ADMUXA=3; // ADC3 (core)
    ACSR0A=_BV(ACPMUX2); // 
    ACSR0B=_BV(ACNMUX0); // p=bandgap, n= adc muxer
    _delay_us(100.0);  // todo does a timer interrupt saving power?
}

inline void stopComparator() {
    power_adc_disable();
	ACSR0A=_BV(ACD0);
}

 inline char readCore() {
    ADMUXA=3; // ADC3 (core)
    _NOP(); // wait for comparator
    _NOP();
    _NOP();
    return!(ACSR0A&_BV(ACO0));
}

 inline char read5V() {
    ADMUXA=11; // ADC11 (5v for leds and vib)
    _NOP(); // wait for comparator
    _NOP();
    _NOP();
    return!(ACSR0A&_BV(ACO0));
}



ISR(WDT_vect) {
    static char stateTimer=CYCLETIME;
	char isCore;
	char is5V;
	char isButton;
	
    WDTCSR |= _BV(WDIE);
    setDebugPins(BALL_DEBUG_SYNC); // all debug pins up

    setDebugPins(BALL_DEBUG_SYNC);
    
	startComparator();  // read inputs
	isCore=readCore();
	is5V=read5V();
	isButton=readButton();
	stopComparator();
	
    setDebugPins(BALL_RESET); // all debug pins down

    switch(ballState) {
    case BALL_RESET:
        if(stateTimer) {
            if(!isButton) {
                stateTimer--;
            }
        } else {
            ballState=BALL_OFF;
        }
        break;
    case BALL_OFF:
        if(isCore) {
            ballState=isButton?BALL_AUTOCATCH_WAIT:BALL_ON;
            stateTimer=CYCLETIME;
        }
        break;
    case BALL_ON:
        if(!isCore) {
            ballState=BALL_OFF;
        } else {
            if(stateTimer) {  // manage periodic wake ups, every minute for ~600 ms
                stateTimer--;
                if(isButton) {
                    ballState=BALL_AUTOCATCH_WAIT;
                    stateTimer=CYCLETIME;
                }
            }
        }
        break;
    case BALL_AUTOCATCH_WAIT:
        if(!isCore) {
            ballState=BALL_OFF;
        } else {
            if(isButton) { // button MUST released for connection
                stateTimer=CYCLETIME;
            } else {
                if(stateTimer) {
                    stateTimer--;
                } else {
                    ballState=BALL_AUTOCATCH_STANDBY;
                }
            }
        }
        break;
    case BALL_AUTOCATCH_STANDBY:
        if(!isCore) {
            ballState=BALL_OFF;
        } else {
	        if(is5V) { //Vib&LEDs on?
		        ballState=BALL_AUTOCATCH_PRESSED;
			    pressButton();
				stateTimer=CYCLETIME;
			}
		}
        break;
    case BALL_AUTOCATCH_PRESSED:
        if(!is5V) {
            ballState=BALL_AUTOCATCH_STANDBY;
            releaseButton();
        }else{
			// limit to 1 s
			if(stateTimer){
				stateTimer--;
			}else{
				releaseButton();
			}
		}
        break;
    default:
        // something going wrong
        releaseButton();
        for(;;); // wait until watchdog reset
        break;
    }

    setDebugPins(ballState);
}

inline void initWatchdog() {
    MCUSR &= ~(1<<WDRF);
    cli();
    WDTCSR |= _BV(WDE)|_BV(WDIE);
    WDTCSR = WDTO_120MS;
    // enable watchdog interrupt only mode
    WDTCSR |= _BV(WDIE);
}


int main(void) {

    initWatchdog();
    initDebugPins();
    initButton();

    setPowerSavingOn();

    ballState=BALL_RESET;  // state after reset

    sleep_enable();
    sei();

    for(;;) {
        sleep_cpu(); // Snorlax is my guru
    }

}

