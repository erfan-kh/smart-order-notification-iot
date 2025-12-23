## Smart Order Notification System (IoT + Backend)

### 📌 Overview
This project is a real-world IoT system designed for fast food restaurants to notify customers when their orders are ready.

Instead of using traditional speakers or monitors (e.g., calling out order numbers), this system sends **SMS and WhatsApp messages (with text, photos, or videos)** directly to customers based on their order number.

The system has been **actively used in a real fast food business for over 6 months** and is stable, tested, and fully functional.

---

### 🎯 Problem Statement
In classic fast food systems:
- Customers must constantly listen to speakers or watch monitors
- Noisy environments reduce effectiveness
- Customers cannot leave the store while waiting

This project solves those problems by notifying customers **directly on their phones**, even if they are outside the restaurant.

---

### 🛠 System Architecture

---

### 🔧 Hardware
- **Microcontroller:** ESP32 (Arduino-based)
- **Input:** 4×4 Keypad (order number entry)
- **Display:** 16×2 Character LCD
- **Connectivity:** WiFi
- **Sensors:** None (user-input based system)

---

### 💻 Firmware (ESP32)
- Written in **C** using basic Arduino/ESP32 libraries
- Responsibilities:
  - Connect to WiFi
  - Read order number from 4×4 keypad
  - Display status and responses on LCD
  - Send order number to backend via REST API
  - Receive success/error responses from server
- Includes **complete error handling** (network, server, invalid order)

---

### 🌐 Backend
- **Framework:** Django (Python)

#### Backend Responsibilities:
- Provide a **setup panel** to:
  - Define message text
  - Upload photos or videos
- Accept order numbers from:
  - ESP32 device (via API)
  - Web UI (manual entry)
- Query **cashier application databases** to:
  - Find customer name
  - Retrieve phone number by order number
- Send notifications via:
  - **SMS panel API**
  - **WhatsApp automation (Selenium)**
- Report results back to ESP32 device
- Full **error handling and reporting**

> The system is designed as a **plugin-style service** and is compatible with most cashier database structures commonly used in Iran.

---

### 🎨 Frontend
- Simple and user-friendly UI
- Features:
  - Message configuration
  - Media upload (photo/video)
  - Manual order sending
- Enhanced with small JavaScript features for smoother UX

---

### 🚀 Real-World Usage
- Deployed and used daily in a real fast food restaurant
- Running continuously for **6+ months**
- Benefits:
  - No need for speakers or monitors
  - Customers can leave the restaurant while waiting
  - Friendly communication via WhatsApp media messages
  - Improved customer experience and satisfaction

---

### 🧰 Technologies Used
- ESP32 / Arduino
- Embedded C
- Python (Django)
- REST APIs
- HTML / CSS / JavaScript
- Selenium
- SQL Databases
- WiFi Networking

---

### 📌 Status
✅ Production-ready  
✅ Actively used  
✅ Stable and tested
