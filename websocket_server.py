import asyncio
import websockets
#import websocket
import csv

#Settings that are specific to your needs required to setup the Websocket server
csvfile = 'bme_sensor_values_AC_3.csv' # ENTER YOUR CSV FILENAME HERE
#websocketIpAddress = "192.168.0.104"               # ENTER YOUR IP ADDRESS HERE
websocketIpAddress = "192.168.0.101"
websocketIpPort = 8765                   # ENTER YOUR IP PORT HERE

async def echo(websocket):
    async for message in websocket:
        #print(message)
        await websocket.send("a")
        with open(csvfile, 'a') as f:
            f.write(str(message,'UTF-8'))
        #await websocket.send(message)

async def main():
    async with websockets.serve(echo, websocketIpAddress, websocketIpPort):
        await asyncio.Future()  # run forever

with open(csvfile, 'a') as f:
        f.write("Time,Temperature,Pressure,Hummidity\n")
asyncio.run(main())







