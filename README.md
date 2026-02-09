Hi, 

I've created some marine displays using ESP32 display boards. The idea behind the project is for them to be easy to build, WebUI-driven, but still customisable to give you a polished look and feel to the unit. At the moment I'm working with two devices, both from Waveshare: 

ESP32-S3 2.8inch Round Display Development Board, 480×480, 32-bit LX7 Dual-core Processor, Supports WiFi & Bluetooth, Options For Touch Function, ESP32 With Display
https://www.waveshare.com/esp32-s3-touch-lcd-2.8c.htm?srsltid=AfmBOoooX16CI5PqCOr3nhNqPt4TPXI3fdQ1leVDT-1jVedYgmw9eRSc

ESP32-S3 4inch Display Development Board, 480×480, 32-Bit LX7 Dual-Core Processor, Up To 240MHz Frequency, Supports WiFi & Bluetooth, With Onboard Antenna, ESP32 With Display
https://www.waveshare.com/esp32-s3-touch-lcd-4.htm

The round display, i'm focusing more on it, listening to SignalK paths and basically creating some sort of smart display, for different engine information, battery, wind etc.

For the square display, the first set of code will build on the round display, with more options for things like number diplays, graphs and meta data pulled in from SK. I will also look at NMEA2000 if there is demand for this.

Both displays will connect via WiFi useing Websocket and subscribe to the data paths for instant updates of values coming in.

Its all avilable for free here, try it, test, tell me what works.

IMG_2602.jpg

Videos and more coming soon.

The Baileys
