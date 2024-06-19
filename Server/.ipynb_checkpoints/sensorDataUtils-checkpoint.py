import pandas as pd
import matplotlib.pyplot as plt

import csv
import datetime

def convertTime(microseconds_since_epoch):
    # Convert microseconds to seconds
    seconds_since_epoch = microseconds_since_epoch / 1_000_000
    
    # Convert to a datetime object
    timestamp = datetime.datetime.utcfromtimestamp(seconds_since_epoch)

    return timestamp.strftime('%Y-%m-%d %H:%M:%S.%f')  # Return as a formatted string

def process_csv(input_file, output_file):
    with open(input_file, 'r', newline='') as infile, \
         open(output_file, 'w', newline='') as outfile:
        
        reader = csv.DictReader(infile)
        fieldnames = reader.fieldnames  # Keep the original fieldnames
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        writer.writeheader()

        for row in reader:
            try:
                timestamp = row['Time']
                microseconds_since_epoch = int(timestamp)
                human_time = convertTime(microseconds_since_epoch)  # Convert string to microseconds since the epoch
                row['Time'] = human_time  # Replace the "Time" value with the human-readable timestamp
            except ValueError as e:
                print(f"Skipping row due to error: {e}")
                continue  # Skip rows with conversion issues
            writer.writerow(row)


def plotResults(file_path):
    
    # Read the CSV file into a DataFrame
    df = pd.read_csv(file_path)
    
    # Convert the 'Time' column to datetime
    df['Time'] = pd.to_datetime(df['Time'])
    
    # Plotting
    fig, axs = plt.subplots(3, 1, figsize=(10, 15), sharex=True)
    
    # Plot Temperature vs Time
    axs[0].plot(df['Time'], df['Temperature'], label='Temperature', color='r')
    axs[0].set_title('Temperature vs Time')
    axs[0].set_ylabel('Temperature (°C)')
    axs[0].legend()
    
    # Plot Pressure vs Time
    axs[1].plot(df['Time'], df['Pressure'], label='Pressure', color='b')
    axs[1].set_title('Pressure vs Time')
    axs[1].set_ylabel('Pressure (Pa)')
    axs[1].legend()
    
    # Plot Humidity vs Time
    axs[2].plot(df['Time'], df['Hummidity'], label='Humidity', color='g')
    axs[2].set_title('Humidity vs Time')
    axs[2].set_ylabel('Humidity (%)')
    axs[2].legend()
    
    # Set common labels
    plt.xlabel('Time')
    plt.tight_layout()
    plt.show()