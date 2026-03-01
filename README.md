# AmbilightUI
A modern WinUI 3 based frontend for Ambilight - using Arduino UNO with WS2812B, along with an RGB LED on analog pins 9,10,11 and a LDR on A0.\
Uses Win8 Desktop Duplication for Ambient lighting.\
Uses OpenHardwareMonitor for GPU temperature detection, changes color of external RGB LED accordingly, from Green to Red.
Uses Light Dependent Resistor in pin A0, to automatically turn on lights when room becomes dark.


## Pin Positions 

WS2812B -> D6\
RGB RED ->  D11\
RGB BLUE -> D9\
RGB GREEN -> 10\
LDR -> A0\
Common Ground is necessary!

## Guide
1. Flash the INO sketch to Arduino using Arduino IDE, located in the **Arduino** folder
2. Compile exe, or get a precompiled binary from the **Releases** section
3. After plugging everything in, supplying power to WS2812B from external PSU, run exe.
4. Disable Windows Defender Protection, JUST FOR THIS APP. Mostly observed, OpenHardwareMonitor causes issues with Defender
5. Configure COM port and LED placement configuration from within the app.

## Credits
This program may have errors, as it was partially made using Gemini. Free to redistribute this software. Developed by me.
