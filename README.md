# MetaFirmware
Firmware for MetaSense, to program the EFR32MG24C microcontroller from Silicon Labs to do the following:
- Take sensor readings from the AD5941 electrochemical frontend
- Transmit data over the air to a target device (for the moment, devices running iOS or MacOS at the moment)

The "finalver" folder contains all of firmware necessary for that purpose. The folder should be imported into the Simplicity Studio IDE desktop app from Silicon Labs. The user should first import the Bootloader Apploader OTA example project from Silicon Labs, and build + flash that onto the EFR microcontroller. Without the OTA apploader the MCU will not be able to run BLE code successfully. After that, the user can build + flash this project onto the EFR microcontroller on the MetaSense board. Once that is done, if the user has the [corresponding Flutter app](https://github.com/amnzer/MetaSensePilot) running, they should be able to see bluetooth transmissions being received on the app. 

A note on the project design. The finalver folder carries the typical project structure for embedded projects on SiLabs MCUs. For instance, the app.c file contains almost all relevant firmware code, whereas the other files are largely libraries or auto-generated based on project configs. Besides app.c, the other two key files are efr32mg24port.c and ad5940.c, which together constitute the driver for communication between the EFR32MG24C and the AD5941 chips. 
