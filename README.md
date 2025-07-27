# AI-Voice-Recorder-AWS-S3
This repository contains production-grade firmware for a low-power, autonomous audio recorder based on the ESP32-S3. The device is designed for battery-powered operation, recording high-quality audio to an SD card and automatically uploading the files to any S3-compatible cloud storage service (like AWS S3, Minio, or Wasabi). 
The entire workflow is optimized for minimal power consumption and autonomous operation. It wakes only to record or upload, spending the rest of the time in a deep sleep state to maximize battery life.

<img width="594" height="420" alt="Image" src="https://github.com/user-attachments/assets/9bd7aef1-ae10-4504-b9d1-e8d02b3e1b98" />

---
## ✨ Key Features

* ⚡️ **Ultra-Low-Power Operation:** Utilizes ESP32's deep sleep to achieve multi-day or multi-week battery life, waking only on specific triggers.

* 🎙️ **Instant & Continuous Recording:**
    * Starts recording **immediately** to a temporary file upon a button press, with **zero WiFi delay**.
    * Audio is captured in 15-minute segments to keep file sizes manageable. Recording continues automatically into a new file part until stopped.

* 🧠 **Smart File Naming:**
    * While recording, the device connects to WiFi in the background to fetch the current time from an NTP server.
    * The temporary audio file is then automatically renamed to a timestamped format (`mic1_YYYY-MM-DD_HH-MM-SS_part1.wav`).
    * If WiFi fails, it falls back to a persistent session-based name (`mic1_audio1_part1.wav`).

* ☁️ **Automatic & Robust S3 Upload:**
    * When connected to a charger, the device automatically wakes up, connects to WiFi, and uploads all recordings.
    * It uses the robust AWS Signature Version 4 protocol for secure authentication.
    * If an upload fails for any reason (e.g., poor network), it will **retry that specific file indefinitely** until it succeeds, ensuring no data is lost.

---
## 🚀 Workflow

The device operates in a simple, state-based cycle:

1.  **Sleep:** The device is normally in deep sleep, consuming microamps of power.
2.  **Record:** A **press of the push button** wakes the device. It instantly begins recording audio to the SD card.
3.  **Stop & Sleep:** A **second press of the button** stops the recording, finalizes the audio file, and puts the device back into deep sleep.
4.  **Upload:** Connecting the device to a **USB charger** wakes it up. It connects to WiFi, finds all `.wav` files on the SD card, and uploads them one by one. Once finished, it returns to deep sleep.

---
## 🔧 Configuration

All user-specific settings are located at the top of the `main.ino` file in the `USER CONFIGURATION` section. You will need to set:
* Hardware pin definitions for your board.
* Your WiFi SSID and password.
* Your S3 service credentials (`accessKey`, `secretKey`, `host`, `bucket`, `region`).

---
## 🛠️ Hardware Requirements

* **Microcontroller:** Seeed Studio XIAO ESP32-S3 (or any other ESP32-S3 board).
* **Microphone:** An I2S microphone (e.g., INMP441).
* **Storage:** A MicroSD card module and a MicroSD card.
* **Power:** A LiPo battery and a charging circuit.
* **Interface:** A single push button and a status LED.
