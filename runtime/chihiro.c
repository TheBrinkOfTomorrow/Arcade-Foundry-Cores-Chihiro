// The Sega side of a Chihiro: the baseboard on USB (baseboard.c: JVS I/O,
// the cabinet's buttons, guns and coins; its EEPROMs), and the media board
// (the game's image, "mbcom:"; not yet).
#include "xbox.h"
#include "baseboard.h"

void chihiro_init(void)
{
    baseboard_init();
}
