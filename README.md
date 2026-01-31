# CanSniffer & Mini Screen Build (MK1)

Hi, 

Welcome to CanSniffer & Mini Screen build MK1, this project came from the desire to interact with my boat’s network, sniff data and display on my own custom screen with custom graphics. 
NOTE: This was all ChatGPT vibe coded, I am not a software engineer, I’m more of a 12V electronics / Cad guy who was feeling like trying something new. As a result this code is likely not optimized and not fit for commercial use. Please use it at your own risk and
DO NOT USE THIS HARDWARE OR CODE FOR ANY CRITICAL INSTUMENTATION DISPLAYS

Now that’s out of the way, the system architecture is split into 2 halves, the Arduino based CanSniffer takes the data off the NMEA2000 Network, using a CAN reading card, grabs data out of the relevant PGN’s and finally outputs a CSV string over USB of 4 values that the Pi is expecting to display on the UI.  

Currently it is set up for single frame reading of the Trim % and RPM PGN. It uses fast packet to decode the PGN for fuel consumption rate and it uses FF12 which is proprietary SUZUKI PGN used for shift indication. That PGN was decoded during the DEV process for my specific setup of a single Fly-by-wire shift and throttle controller, one SMG4 MFD and a DF200 single outboard. This may or may not work with other setups of SUZUKI hardware.This is an experimental hobby project, not production-grade, as I said before and chatgpt handled the code work on this project. I recommend having a laptop collecting the log data when using the software. There is also a little I2C OLED screen on the CanSniffer itself you can use for whatever. 

In the PI I have some helper scipts that work with the 3 buttons on the side of the display case, the references to the name of the main code for the pi display is called PiRudderTach.py you can change that but you will have to adjust everything in the support sofware, the screen also needs to be configured in the PI for portait orientation

In my experience this build has been stable and I am happy with it, of course anyone who has more experience will be able to add features or optimizations. Good luck, I would love to hear from anyone who is working on this project and what you end up making. 

## Getting Started
1. Upload the Arduino sketch to an Arduino Nano  
2. Connect MCP2515 to the NMEA2000 backbone  
3. Plug Arduino into Raspberry Pi via USB  
4. Run the dashboard script on the Pi  

This project was developed and tested on:
- Suzuki DF200 outboard
- Single fly-by-wire shift/throttle controller
- One SMG4 MFD
- Arduino Nano + MCP2515 module
- Small I2C OLED screen on the sniffer unit

The current firmware is configured for:
- **Trim % + RPM** (single-frame decoding)
- **Fuel Rate** (Fast Packet decoding, PGN 127489)
- **Shift / Gear Indicator** via proprietary Suzuki PGN:
- **PGN FF12 (65298)** — Suzuki-specific gear data

## Serial Output Format
The Arduino outputs a 4-field CSV frame over USB:

trim_raw,rpm_raw,gear,fuel_gph
Where `gear` is encoded as:
- `-1` = Reverse  
- `0` = Neutral  
- `1` = Forward  

## Licensing
- Software (Arduino + Pi code): GPL v3  
- Hardware/design files (STLs, wiring): CC BY-SA 4.0

This project is for experimental/educational use only.
