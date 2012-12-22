// alarm.c  --  alarm functionality
//
//    PD2               alarm switch
//


#include <avr/io.h>           // for using avr register names
#include <avr/pgmspace.h>     // for accessing data in program memory
#include <avr/eeprom.h>       // for accessing data in eeprom
#include <avr/power.h>        // for enabling/disabling microcontroller modules
#include <util/delay_basic.h> // for the _delay_loop_1() macro


#include "alarm.h"
#include "pizo.h"    // for sounding the alarm
#include "time.h"    // alarm must sound at appropriate time
#include "system.h"  // alarm behavior depends on power source
#include "mode.h"    // mode updated on alarm state changes
#include "usart.h"   // for debugging output
#include "display.h" // for ensuring display is enabled


// extern'ed alarm data
volatile alarm_t alarm;


// default alarm times
uint8_t ee_alarm_hours[ALARM_COUNT] EEMEM = {
    [0 ... ALARM_COUNT - 1] = ALARM_DEFAULT_HOUR
};

uint8_t ee_alarm_minutes[ALARM_COUNT] EEMEM = {
    [0 ... ALARM_COUNT - 1] = ALARM_DEFAULT_MINUTE
};

uint8_t ee_alarm_days[ALARM_COUNT] EEMEM = {
    [0 ... ALARM_COUNT - 1] = ALARM_DEFAULT_DAYS
};

// volume range, and snooze timeout
uint8_t ee_alarm_snooze_time EEMEM = ALARM_DEFAULT_SNOOZE_TIME;
uint8_t ee_alarm_volume_min  EEMEM = ALARM_DEFAULT_VOLUME_MIN;
uint8_t ee_alarm_volume_max  EEMEM = ALARM_DEFAULT_VOLUME_MAX;
uint8_t ee_alarm_ramp_time   EEMEM = ALARM_DEFAULT_RAMP_TIME;


// initialize alarm after system reset
void alarm_init(void) {
    // load alarms
    for(uint8_t i = 0; i < ALARM_COUNT; ++i) alarm_loadalarm(i);

    // load alarm configuration and ensure reasonable values
    alarm.snooze_time  = eeprom_read_byte(&ee_alarm_snooze_time) % 31;
    alarm.ramp_time    = eeprom_read_byte(&ee_alarm_ramp_time )  % 61;
    alarm.volume_max   = eeprom_read_byte(&ee_alarm_volume_max)  % 10;
    alarm.volume_min   = eeprom_read_byte(&ee_alarm_volume_min);

    // convert snooze time from minutes to seconds
    alarm.snooze_time *= 60;

    if(alarm.volume_min > alarm.volume_max) {
	alarm.volume_min = alarm.volume_min;
    }

    pizo_setvolume((alarm.volume_min + alarm.volume_max) >> 1, 0);

    // prevents divide-by-zero error later on
    if(!alarm.ramp_time) alarm.ramp_time = 1;

    // calculate time.ramp_int
    alarm_newramp();

    // configure pins for low-power mode
    alarm_sleep();
}


// initialize alarm after sleep
void alarm_wake(void) {
    // configure alarm switch pin
    DDRD  &= ~_BV(PD2); // set as input pin
    PORTD |=  _BV(PD2); // enable pull-up resistor

    // give system time to update PIND
    _delay_loop_1(2);

    // set initial alarm status from alarm switch
    if(PIND & _BV(PD2)) {
	alarm.status |= ALARM_SET;
    } else {
	if(alarm.status & ALARM_SOUNDING) pizo_alarm_stop();
	alarm.status &= ~ALARM_SET & ~ALARM_SOUNDING & ~ALARM_SNOOZE;
    }

    if(alarm.status & ALARM_SOUNDING) {
	// lower volume which may be raised above
	// volume_max during sleep mode
	alarm.volume = alarm.volume_max;
	pizo_setvolume(alarm.volume, 0);
    }
}


// prepare alarm for sleep
void alarm_sleep(void) {
    // clamp alarm switch pin to ground
    PORTD &= ~_BV(PD2); // disable pull-up resistor
    DDRD  |=  _BV(PD2); // set as ouput, clamped to ground

    if(alarm.status & ALARM_SOUNDING) {
	// disable progressive alarm to save power
	alarm.volume = alarm.volume_max;

	// compensate for reduced volume from lower-voltage battery
	if(alarm.volume < 10) ++alarm.volume;

	// finally set the volume
	pizo_setvolume(alarm.volume, 0);
    }
}


// sound alarm at the correct time if alarm is set
void alarm_tick(void) {
    // will be set to TRUE if alarm should be triggered
    uint8_t is_alarm_trigger = FALSE;

    // check if alarm should be triggered
    for(uint8_t i = 0; i < ALARM_COUNT; ++i) {
	if((alarm.days[i] & ALARM_ENABLED)
		&& time.hour == alarm.hours[i]
		&& time.minute == alarm.minutes[i]
		&& time.second == 0
		&& (alarm.days[i] & _BV(time_dayofweek(time.year, time.month,
			    			       time.day)))) {
	    is_alarm_trigger = TRUE;
	}
    }

    // sound alarm if alarm should be triggered
    if(is_alarm_trigger) {
	if(system.status & SYSTEM_SLEEP) {
	    // briefly waking the alarm will update
	    // alarm.status from the alarm switch
	    alarm_wake();
	    alarm_sleep();
	}

	// sound alarm if alarm is set
	if(alarm.status & ALARM_SET) {
	    alarm.alarm_timer = 0;
	    alarm.status |= ALARM_SOUNDING;

	    if(system.status & SYSTEM_SLEEP) {
		// wake user immediately to reduce alarm time,
		// save power, and extend coin battery life
		alarm.volume = alarm.volume_max;
	    } else {
		// set initial volume for progressive alarm
		alarm.volume = alarm.volume_min;
	    }

	    pizo_setvolume(alarm.volume, 0);
	    pizo_alarm_start();

	    // ensure display is enabled if power present
	    display_onbutton();
	}
    } else if(alarm.status & ALARM_SOUNDING && system.status & SYSTEM_SLEEP) {
	// if alarm sounding and system sleeping, query alarm switch
	// (briefly waking updates alarm.status from the alarm switch)
	alarm_wake();
	alarm_sleep();
    }

    // manage sounding alarm
    if(alarm.status & ALARM_SOUNDING) {
	if(alarm.volume < alarm.volume_max) {
	    // gradually increase volume (progressive alarm)
	    if(alarm.alarm_timer > alarm.ramp_int) {
		++alarm.volume;
		alarm.alarm_timer = 0;
	    }

	    pizo_setvolume(alarm.volume,
		    	   ((uint32_t)alarm.alarm_timer << 8) / alarm.ramp_int);
	} else if(alarm.alarm_timer > ALARM_SOUNDING_TIMEOUT) {
	    // silence alarm on alarm timeout
	    alarm.status &= ~ALARM_SOUNDING;
	    pizo_alarm_stop();
	}

	++alarm.alarm_timer;
    }

    // sound alarm on snooze timeout
    if(alarm.status & ALARM_SNOOZE) {
	if(++alarm.alarm_timer == alarm.snooze_time) {
	    alarm.alarm_timer  = 0;
	    alarm.status &= ~ALARM_SNOOZE;
	    alarm.status |=  ALARM_SOUNDING;

	    if(system.status & SYSTEM_SLEEP) {
		// wake user immediately to reduce alarm time,
		// save power, and extend coin battery life
		alarm.volume = alarm.volume_max;
	    } else {
		// set initial volume for progressive alarm
		alarm.volume = alarm.volume_min;
	    }

	    pizo_setvolume(alarm.volume, 0);

	    pizo_alarm_start();
	}
    }
}


// queries alarm switch and updates alarm status
void alarm_semitick(void) {
    // update alarm status if alarm switch has changed
    static uint8_t alarm_debounce = 0;

    if(PIND & _BV(PD2)) {
	if(alarm.status & ALARM_SET) {
	    alarm_debounce = 0;
	} else {
	    if(++alarm_debounce >= ALARM_DEBOUNCE_TIME) {
		alarm.status |= ALARM_SET;
		mode_alarmset();
		display_onbutton();
	    }
	}
    } else {
	if(alarm.status & ALARM_SET) {
	    if(++alarm_debounce >= ALARM_DEBOUNCE_TIME) {
		if(alarm.status & ALARM_SOUNDING) pizo_alarm_stop();
		alarm.status &= ~ALARM_SET & ~ALARM_SOUNDING & ~ALARM_SNOOZE;
		mode_alarmoff();
		display_onbutton();
	    }
	} else {
	    alarm_debounce = 0;
	}
    }
}

void alarm_loadalarm(uint8_t idx) {
    alarm.hours[idx]   = eeprom_read_byte(&(ee_alarm_hours[idx]))   % 24;
    alarm.minutes[idx] = eeprom_read_byte(&(ee_alarm_minutes[idx])) % 60;
    alarm.days[idx]    = eeprom_read_byte(&(ee_alarm_days[idx]));
}

// set new time and save time to eeprom
void alarm_savealarm(uint8_t idx) {
    eeprom_write_byte(&(ee_alarm_hours[idx]), alarm.hours[idx]);
    eeprom_write_byte(&(ee_alarm_minutes[idx]), alarm.minutes[idx]);
    eeprom_write_byte(&(ee_alarm_days[idx]), alarm.days[idx]);
}


// save alarm volume to eeprom
void alarm_savevolume(void) {
    eeprom_write_byte(&ee_alarm_volume_min, alarm.volume_min);
    eeprom_write_byte(&ee_alarm_volume_max, alarm.volume_max);
}


// save ramp interval to eeprom
void alarm_saveramp(void) {
    eeprom_write_byte(&ee_alarm_ramp_time, alarm.ramp_time);
}


// compute new ramp interval from ramp time
void alarm_newramp(void) {
    alarm.ramp_int = alarm.ramp_time * 60
			 / (alarm.volume_max - alarm.volume_min + 1);
}


// save alarm snooze time (in seconds) to eeprom (in minutes)
void alarm_savesnooze(void) {
    // save snooze time as minutes, not seconds
    eeprom_write_byte(&ee_alarm_snooze_time, alarm.snooze_time / 60);
}


// called on button press; returns true if press should be
// processed as enabling snooze, false otherwise
uint8_t alarm_onbutton(void) {
    if(alarm.snooze_time && alarm.status & ALARM_SOUNDING) {
	alarm.status &= ~ALARM_SOUNDING;
	alarm.status |=  ALARM_SNOOZE;
	alarm.alarm_timer = 0;
	pizo_alarm_stop();
	mode_snoozing();
	return TRUE;
    }

    // extend snooze on any button press
    if(alarm.status & ALARM_SNOOZE) {
	alarm.alarm_timer = 0;
    }

    return FALSE;
}


// returns true if current time is within two seconds of alarm time
uint8_t alarm_nearalarm(void) {
    for(uint8_t i = 0; i < ALARM_COUNT; ++i) {
	int32_t time_diff = alarm.hours[i] - time.hour;
	time_diff *= 60;  // hours to minutes
	time_diff += alarm.minutes[i] - time.minute;
	time_diff *= 60;  // minutes to seconds
	time_diff += 0 - time.second;

	if(time_diff > (int32_t)12 * 60 * 60) {
	    time_diff -= (int32_t)24 * 60 * 60;
	} else if(time_diff < (int32_t)-12 * 60 * 60) {
	    time_diff += (int32_t)24 * 60 * 60;
	}

	if(-ALARM_NEAR_THRESHOLD <= time_diff
		&& time_diff <= ALARM_NEAR_THRESHOLD) {
	    return TRUE;
	}
    }

    return FALSE;
}
