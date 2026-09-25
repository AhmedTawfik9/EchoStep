# EchoStep – Feedback Device for Special Needs

EchoStep is a wearable assistive system developed as part of a university project at TU Dortmund University.

The project is designed to support people with hearing impairments by detecting environmental sounds, estimating their direction and intensity, and converting this information into visual feedback on a wearable device.

## Features

- Real-time sound level detection using an Android smartphone
- Stereo-audio based sound direction estimation
- TDOA / GCC-PHAT based sound localization
- Classification of sound direction as LEFT, CENTER or RIGHT
- Sound intensity estimation
- Bluetooth Low Energy (BLE) communication
- Arduino Nano 33 BLE based wearable controller
- LED matrix / RGB visual feedback
- Step counter functionality
- Custom 3D-printed enclosure

## System Architecture

The system consists of three main components:

1. **Android SoundDetector App**
   - Captures audio from the smartphone microphones
   - Calculates sound level
   - Estimates sound direction using stereo audio
   - Uses TDOA and GCC-PHAT for direction estimation
   - Sends the processed information to the wearable device through BLE

2. **Arduino Wearable Device**
   - Based on the Arduino Nano 33 BLE
   - Receives sound information from the Android application
   - Displays sound direction and intensity using LEDs
   - Provides wearable feedback to the user
   - Includes step-counting functionality

3. **3D-Printed Enclosure**
   - Custom enclosure designed for the wearable electronics
   - Contains the Arduino, display/LED components and supporting hardware

## Repository Structure

```text
EchoStep/
├── android-app/     # Android SoundDetector application
├── arduino/         # Arduino firmware
├── 3d-models/       # 3D-printable enclosure files
├── README.md
├── LICENSE
└── .gitignore