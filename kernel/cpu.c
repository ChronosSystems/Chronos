#include "kern/types.h"
#include "cpu.h"
#include "panic.h"

int x86_check_interrupt(void);

static int cli_count = 0;
void push_cli(void)
{
	/**
	 * If we pushcli and interrupts are disabled, a popcli could 
	 * enable interrupts. To prevent this, we are setting cli_count to 1.
	 */
	if(cli_count == 0 && !x86_check_interrupt()) cli_count = 1;
        if(cli_count < 0) cli_count = 0;
        cli_count++;
        // asm volatile("cli");
	//if(cli_count == 1) cprintf("Interrupts disabled.\n");
}

void pop_cli(void)
{
        cli_count--;
        if(cli_count < 1)
        {
		//cprintf("Interrupts enabled.\n");
                cli_count = 0;
                // asm volatile("sti");
        }
}

void reset_cli(void)
{
	cli_count = 0;
}
