import asyncio
import websockets
import csv
from datetime import datetime
import os

def read_config(file_path):
    config = {}
    with open(file_path, 'r') as file:
        for line in file:
            name, value = line.strip().split('=')
            config[name] = value
    return config

config_file_path = 'config.txt' # Path to the configuration file
config = read_config(config_file_path) # Read configuration

# Extract IP address and port
websocketIpAddress = config.get('IP_ADDRESS')
websocketIpPort = config.get('PORT')

#Settings that are specific to your needs required to setup the Websocket server
csvfile = 'AC_Measurements_S2.csv' # ENTER YOUR CSV FILENAME HERE

async def echo(websocket):
    async for message in websocket:
        #print(message)
        await websocket.send("a")
        with open(csvfile, 'a') as f:
            current_time = datetime.now() # Get the current system time
            formatted_time = current_time.strftime('%Y-%m-%d %H:%M:%S') # Format the time in yyyy-mm-dd HH-mm-ss
            entry = str(formatted_time)  + ',' + str(message,'UTF-8')
            # print(entry)
            f.write(entry)
        #await websocket.send(message)

async def main():
    async with websockets.serve(echo, websocketIpAddress, websocketIpPort):
        await asyncio.Future()  # run forever

with open(csvfile, 'a') as f:
        f.write("Computer Time,Sensor Time,Temperature,Pressure,Humidity\n")
asyncio.run(main())







