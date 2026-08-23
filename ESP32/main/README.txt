STEP 1: DDC/CI Power Mode
=========================

Existing behavior is retained:
  0x50 EDID
  0x37 DDC/CI brightness VCP 0x10
  GPIO4 5 kHz PWM

Added:
  VCP 0xD6 Power Mode

Behavior:
  01 On       -> fade to remembered brightness over 500 ms
  02 Standby  -> fade to zero over 400 ms
  03 Suspend  -> fade to zero over 400 ms
  04 Off      -> fade to zero over 400 ms
  05 Off cmd  -> fade to zero over 400 ms

Brightness changes while off are remembered without turning the backlight on.
D6=01 restores the remembered brightness.
