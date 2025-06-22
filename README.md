# Orchestrator Master Firmware for M5Stack Cardputer

![M5Stack Cardputer](https://m5stack.oss-cn-shenzhen.aliyuncs.com/image/m5-docs_homepage/core/cardputer/Cardputer_01.webp)

## Overview
The Orchestrator Master firmware turns the M5Stack Cardputer into a powerful controller for a network of ESP32-based slave devices. It uses ESP-NOW for fast, connectionless communication and provides a rich command interface through the built-in keyboard and display.

## Key Features

### Core Functionality
- **ESP-NOW Network Management**: Automatically pairs with slave devices
- **Distributed Command System**: Controls multiple slaves simultaneously
- **Real-time Feedback**: Displays network status and command results

### Deauthentication Commands
- `deauthA`/`deauthB`: Toggle group deauthentication
- `deauthClient [MAC]`: Target specific client devices
- `deauthPattern [SSID]`: Deauth based on SSID pattern matching
- `deauthHop [ms]`: Set channel hopping interval
- `deauthRate [pkts/s]`: Set packet rate limit
- `deauthProb [%]`: Set probabilistic deauth percentage
- `deauthWindow [start]-[end]`: Set scheduled deauth window

### Monitoring & Visualization
- **Channel Statistics**: Tracks packet counts per channel
- **RSSI Heatmap**: Visualizes signal strength of clients
- **Network Graph**: Shows slave-client relationships
- **Real-time Logging**: Displays command outputs and events

### Additional Commands
- `scan`: Initiate distributed Wi-Fi scan
- `ping`: Check slave connectivity
- `follow [MAC]`: Track specific client across channels
- `clear`: Clear the command log
- `help`: Show available commands

## Hardware Requirements
- M5Stack Cardputer (ESP32-S3)
- Slave devices (ESP32-based)

## Installation
1. Clone this repository
2. Open in PlatformIO
3. Build and upload to Cardputer

## Usage
1. Power on the Cardputer
2. Connect slave devices (they will auto-pair)
3. Use keyboard to enter commands
4. View results on display

## Technical Details
- **Communication Protocol**: ESP-NOW (connectionless)
- **Default Channel**: 1 (configurable)
- **Max Slaves**: 16 (configurable)
- **Security**: Open (no encryption)

## UI Components
- **Slave Panel**: Shows connected devices
- **Command Log**: Displays recent activity
- **Stats Graph**: Channel usage visualization
- **RSSI Heatmap**: Signal strength indicators

## License
MIT License - See LICENSE file for details
