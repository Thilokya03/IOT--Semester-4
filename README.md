# IoT-Based Automated Chemical Dosing System for Hospitals

This project is an IoT-based automated chemical dosing and dispensing system designed for hospital environments. The system automatically mixes concentrated disinfectant chemical with purified water according to a required ratio and refills multiple dispensing tanks when they request a refill.

The main purpose of this project is to reduce manual refilling work, improve dosing accuracy, reduce human error, and make sure that hospital dispensing tanks are refilled safely and on time.

---

## Project Overview

Hospitals use disinfecting chemicals to clean and sterilize medical instruments. These chemicals are usually supplied in concentrated form and must be diluted with purified water before being sent to dispensing tanks.

In many hospitals, dispensing tanks are placed in different areas of the premises. Manually checking and refilling these tanks can take time and may cause mistakes. This project solves that problem by using an automated IoT system.

The system includes one main controller and multiple dispensing tank edge nodes. Each dispensing tank node monitors its own tank level and sends refill requests to the main controller. The main controller handles the requests using a first-request-first-serve method and refills only one tank at a time.

---

## Main Features

* Automatic refill request handling
* First-request-first-serve refill queue
* Only one dispensing tank is refilled at a time
* Water and chemical mixing based on a selected ratio
* Tank level monitoring
* MQTT-based communication
* Edge node fault detection
* Node-level fault isolation
* Automatic recovery after fault is fixed
* Simulation support using Wokwi
* ESP32-based IoT implementation

---

## System Architecture

The project is divided into two main IoT parts:

### 1. Main Component

The main component is the central controller of the system. It is responsible for:

* Receiving refill requests from dispensing tank nodes
* Managing the refill queue
* Selecting the next tank to refill
* Controlling the dosing and refill process
* Sending valve control commands to the selected tank node
* Monitoring node status and fault conditions
* Continuing operation even if one node has an error

### 2. Dispensing Tank Edge Nodes

Each dispensing tank has a separate edge node. The edge node is responsible for:

* Measuring the dispensing tank level
* Sending telemetry data to the main component
* Sending refill requests when the tank level is low
* Receiving valve open or close commands
* Reporting faults to the main component
* Recovering automatically after a fault is cleared

---

## Repository Structure

```text
IOT--Semester-4/
│
├── Main Compornent/
│   ├── src/
│   ├── diagram.json
│   ├── platformio.ini
│   └── wokwi.toml
│
├── Dispensing Tank Node - T01/
│   ├── src/
│   ├── diagram.json
│   ├── platformio.ini
│   └── wokwi.toml
│
├── Dispensing Tank Node - T02/
│   ├── src/
│   ├── diagram.json
│   ├── platformio.ini
│   └── wokwi.toml
│
├── Dispensing Tank Node - T03/
│   ├── src/
│   ├── diagram.json
│   ├── platformio.ini
│   └── wokwi.toml
│
└── README.md
```

---

## Technologies Used

* ESP32
* Arduino Framework
* PlatformIO
* Wokwi Simulator
* MQTT Communication
* C++
* Node-RED / MQTT Dashboard support

---

## Communication Method

The system uses MQTT communication between the main component and the dispensing tank nodes.

Each dispensing tank node publishes telemetry data such as:

* Tank node ID
* Tank level percentage
* Tank volume
* Fault status
* Refill request status

The main component sends control commands such as:

* Open valve
* Close valve
* Heartbeat message
* Refill command

---

## Example MQTT Topics

```text
shacd/thilokya/demo01/telemetry/dispenser/T01
shacd/thilokya/demo01/telemetry/dispenser/T02
shacd/thilokya/demo01/telemetry/dispenser/T03

shacd/thilokya/demo01/cmd/dispenser/T01/valve
shacd/thilokya/demo01/cmd/dispenser/T02/valve
shacd/thilokya/demo01/cmd/dispenser/T03/valve
```

---

## Refill Operation Algorithm

Each dispensing tank node continuously checks its tank level. When the level becomes low, the node sends a refill request to the main component.

The main component stores incoming refill requests in a queue. If more than one tank requests a refill, the tank that requested first is processed first. This prevents multiple tanks from being refilled at the same time.

When a refill starts, the main component sends a valve open command to the selected tank node. The system then performs the dosing process and transfers the mixed disinfectant solution to the selected dispensing tank.

After the required amount is delivered or the tank reaches the required level, the main component sends a valve close command. Then the system moves to the next tank in the queue.

---

## Fault Handling

The system is designed to continue working even if one dispensing tank node has an error.

If a node reports a fault, that node is isolated from the refill process. The main component does not stop the full system. Other healthy dispensing tank nodes can still request refills and continue operating normally.

When the faulty node is fixed, it starts sending normal telemetry again. The main component detects that the error has been cleared and allows that node to join the refill process again.

This improves reliability because one node failure does not stop the whole hospital chemical dosing system.

---

## Node-Level Fault Isolation and Automatic Recovery

If one dispensing tank node has a sensor error, communication problem, or abnormal reading, only that node is marked as faulty. The main controller skips that node during refill selection and continues processing other available nodes. After the fault is corrected, the node sends normal data again. The main controller then automatically updates the node status and allows it to operate normally without restarting the full system.

---

## Simulation

This project can be simulated using Wokwi. Each component has its own Wokwi configuration files.

To run a simulation:

1. Open the required component folder.
2. Open the Wokwi project or use the `diagram.json` and `wokwi.toml` files.
3. Run the simulation.
4. Open the serial monitor.
5. Check MQTT messages and refill status.
6. Test refill requests, tank levels, and fault recovery.

---

## How to Run Using PlatformIO

### Requirements

Install the following software:

* Visual Studio Code
* PlatformIO extension
* Wokwi extension, if simulation is required
* MQTT broker or public MQTT broker access

### Steps

1. Clone the repository:

```bash
git clone https://github.com/Thilokya03/IOT--Semester-4.git
```

2. Open the project folder in Visual Studio Code.

3. Open one component folder, for example:

```text
Main Compornent
```

4. Open `platformio.ini`.

5. Build the project:

```bash
pio run
```

6. Upload to ESP32, if using real hardware:

```bash
pio run --target upload
```

7. Open serial monitor:

```bash
pio device monitor
```

---

## Main Component Responsibilities

The main component performs the following operations:

* Connect to Wi-Fi
* Connect to MQTT broker
* Subscribe to dispensing tank telemetry topics
* Read tank status messages
* Detect refill requests
* Maintain refill queue
* Process only one refill at a time
* Send valve control commands
* Detect node errors
* Skip faulty nodes
* Detect recovered nodes
* Continue system operation safely

---

## Dispensing Tank Node Responsibilities

Each dispensing tank node performs the following operations:

* Connect to Wi-Fi
* Connect to MQTT broker
* Read tank level sensor value
* Calculate tank level percentage
* Calculate remaining volume
* Publish telemetry to MQTT
* Send refill request when level is low
* Receive valve control commands
* Open or close local valve
* Report sensor or node fault
* Return to normal operation after fault is fixed

---

## Example Serial Monitor Output

```text
EDGE TELEMETRY UPDATED SUCCESSFULLY
Node   : T01
Level  : 48.81%
Volume : 366.07 mL
Fault  : NO

State=DOSING | W=72.2% | C=84.1% | Node=T01 | Edge=48.8% / 366mL | Delivered=489mL | Gain=239mL
MQTT publish -> shacd/thilokya/demo01/cmd/dispenser/T01/valve : HEARTBEAT
```

---

## Safety Considerations

This project includes several safety-related design ideas:

* Only one tank is refilled at a time.
* Faulty nodes are isolated.
* Other tanks continue operating when one node fails.
* Refill requests are handled in order.
* Valves are controlled by the main component.
* The system can detect abnormal tank/node conditions.
* The dosing ratio can be adjusted through the backend or control system.

---

## Future Improvements

The system can be improved further by adding:

* Web dashboard for real-time monitoring
* Mobile notification system
* Database logging
* Admin login system
* Automatic report generation
* Flow meter calibration support
* Cloud dashboard integration
* Battery backup monitoring
* LoRa-based long-range communication
* Industrial-grade sensors and controllers

---

## Project Status

This project is developed as a Semester 4 IoT project. The current version focuses on simulation, MQTT communication, refill control logic, node-level fault isolation, and automatic recovery.

---

## Author

Developed by:

**Thilokya Angeesa**

GitHub Repository:

```text
https://github.com/Thilokya03/IOT--Semester-4
```

---

## License

This project is created for academic purposes. 
This project is licensed under the MIT License.  
You are free to use, modify, and distribute this project for educational and development purposes, as long as the original copyright notice is included.

See the `LICENSE` file for more details.

---

## Note

This system is a prototype and simulation-based academic project. For real hospital use, the design must be tested, validated, calibrated, and approved according to medical, electrical, chemical safety, and hospital engineering standards.
