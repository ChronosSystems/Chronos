#ifndef _PIT_H_
#define _PIT_H_

/**
 * Initilize the Programmable Interrupt Timer. The frequency, ticks, is defined 
 * in number of interrupts per second. 
 * WARNING: This will generate spurious interrupts so make sure interrupts
 * 	are disabled before initilizing pit. 
 */
void pitinit(void);

#endif