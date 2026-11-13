// The Chihiro baseboard's inputs and outputs (baseboard.c), set by the front end.
#pragma once
#include <stdint.h>

typedef struct jvs_input_state {
    uint8_t system;          // bit 7 test
    uint16_t player[2];      // bit 15 start, 14 service, 13 up, 12 down, 11 left, 10 right, 9-1 buttons 1-9
    uint16_t coins[2];
    uint16_t analog[8];      // 0x0000-0xffff, centre 0x8000
    uint8_t outputs;         // general-purpose outputs the game sets (lamps, gun recoil)
} jvs_input_state;

extern jvs_input_state jvs_inputs;
void baseboard_init(void);
// the media board's mbcom: device: byte offsets into its partition
void mediaboard_read(uint64_t at, uint8_t *buf, uint32_t len);
void mediaboard_write(uint64_t at, const uint8_t *buf, uint32_t len);
// the cabinet's TEST and SERVICE also go to the baseboard's own buttons
// (the AN2131QC's PINSA): set while a game's own test program runs
extern int baseboard_panel_buttons;
