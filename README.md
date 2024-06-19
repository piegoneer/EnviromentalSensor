# README #

### What is this repository for? ###

* This repository lets an ESP32 communicate via I2C to a BME280 IC collecting pressure(Pa), hummidity(%) and temperature readings(deg C). It uploads the read data(along with a timestamp of each measurement in us) via websocket to your selected IP address. As a demostration only a python script is included that uploads the BME280 data and continously updates a csv file which can be used for live displaying of BME280 data from the ESP32
* V0.1

### Hardware Requirements ###
* ESP32 based microcontroller
* BME280 sensor connected to ESP32 via I2C

### How do I get set up? ###
* This setup has only been tested on Ubuntu 22.04.1 LTS but should work on other setups as well
* Please follow the instructions in https://github.com/espressif/idf-eclipse-plugin/blob/master/README.md. This will install esp-idf on eclispe
* Clone this repository in the eclispe workspace directory
* Import this repository from Eclispe. From Eclispe select File->Import. Select Espressif->Exisiting IDF Project
* Build the project.
* Push Run while the ESP32 is connected to the computer. If requested select the serial port that the ESP32 is installed on
* The python scipt "websocket_server.py" located in the base of the repository is for demostation only. Its been tested on Python 3.10.6. Enter the IP address of the websocket and the filename of the csv file you want the data uploaded to inside the ".py" file. Run this script in order to create a Server Websocket. This will upload data from the ESP32 and write it to the csv file you specified
* You can use a program like kst2 or other program to see graphs of the data from the BME280 live
