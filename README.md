Hi, 

I've created some marine displays using ESP32 display boards. The idea behind the project is for them to be easy to build, WebUI-driven, but still customisable to give you a polished look and feel to the unit. At the moment I'm working with two devices, both from Waveshare: 

ESP32-S3 2.8inch Round Display Development Board, 480×480, 32-bit LX7 Dual-core Processor, Supports WiFi & Bluetooth, Options For Touch Function, ESP32 With Display
https://www.waveshare.com/esp32-s3-touch-lcd-2.8c.htm?srsltid=AfmBOoooX16CI5PqCOr3nhNqPt4TPXI3fdQ1leVDT-1jVedYgmw9eRSc

ESP32-S3 4inch Display Development Board, 480×480, 32-Bit LX7 Dual-Core Processor, Up To 240MHz Frequency, Supports WiFi & Bluetooth, With Onboard Antenna, ESP32 With Display
https://www.waveshare.com/esp32-s3-touch-lcd-4.htm

The round display, i'm focusing more on it following a gauge-type idea, listening to SignalK paths and creating a smart display, for different engine information, battery, wind etc.

For the square display, the first set of code will build on the round display, with more options for things like number diplays, graphs and meta data pulled in from SK. 

Both displays will connect via WiFi useing Websocket and subscribe to the data paths for instant updates of values coming in.

There is a NMEA2000 version now of the square display; more on this soon.

It's all available for free here, try it, test, tell me what works.

![IMG_2602.jpg](https://github.com/Boatingwiththebaileys/Marine-Displays/blob/b56cb05db780b8a2692e31b83edfad973bb852f3/IMG_2602.jpg)

Video of the round display - Building Custom Marine Displays with ESP32 & SignalK (Gauges, Dynamic Icons & Alerts)
https://youtu.be/d4MlVSbn_wI

Video of the square display - DIY ESP32 Marine MFD – Square Multifunction Boat Display using SignalK Data https://youtu.be/FAPvdz6oN7A

The Baileys
