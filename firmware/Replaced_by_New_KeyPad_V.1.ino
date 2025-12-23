#include <Wire.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>  
#include "SPIFFS.h"
#include <Keypad.h>


////////////////////////////////////////////////////////
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

SemaphoreHandle_t lcdMutex;

String lastDisplayedStatus = "";

static bool firstLiveUpdate = true;  // Track the first live update per factor.
////////////////////////////////////////////////////////


// Delay constants (in milliseconds)
#define BROWSING_UPDATE_DELAY 8000       // Inactivity required after the last key press
#define MIN_BROWSING_MODE_TIME 10000       // Must be in browsing mode at least 10 seconds before live update triggers
#define BROWSING_TIMEOUT 300000             // Total time allowed in browsing mode before timeout

unsigned long browsingModeStartTime = 0;   // Time when browsing mode was entered


struct SMSBalance {
  String credit;
  int messagesSendable;
};


//const char* ssid = "Home 2.4 Ghz";
//const char* password = "erfan2281618005erfan";


//const char* ssid = "Erfan";
//const char* password = "e1234567890";

const char* ssid = "Ei.Point";
const char* password = "erfan2281618005iman";


//// Endpoint for processing the factor.
//const char* processUrl = "http://192.168.1.150:8000/process_factor_api/";
//// Endpoint for checking notification status.
//const char* checkStatusUrl = "http://192.168.1.150:8000/check_notification_status/";
//// Endpoint for checking SMS status.
//const char* smsAccountBalance = "http://192.168.1.150:8000/get_sms_balance_api/";


// Endpoint for processing the factor.
const char* processUrl = "http://37.202.169.170:8000/process_factor_api/";
// Endpoint for checking notification status.
const char* checkStatusUrl = "http://37.202.169.170:8000/check_notification_status/";
// Endpoint for checking SMS status.
const char* smsAccountBalance = "http://37.202.169.170:8000/get_sms_balance_api/";


// Global variables for balance tracking
bool balanceFetched = false;
String smsBalanceCredit = "";
int smsMessagesSendable = 0;
bool balanceChecked = false;  // Ensure the balance request runs only once


// API token for authentication.
const char* apiToken = "YOUR_SECRET_TOKEN";

// --- LCD Configuration ---
// LCD connected in 4-bit mode: RS, E, D4, D5, D6, D7 (adjust pins as needed)
LiquidCrystal lcd(15, 4, 16, 17, 18, 19);



// Define custom character (Smiley Face)
byte smiley[8] = {
  0b00000,
  0b01010,
  0b01010,
  0b00000,
  0b10001,
  0b01110,
  0b00000,
  0b00000
};


// Global variable to hold the factor number string
String factorNumber = "";
// We'll store the last factor sent to use for polling.
String lastFactorSent = "";
// Global variable to store the send ID once received (from the final polling response).
String lastSendID = "";

// Global variables for non-blocking polling for notification status.
bool pollingActive = false;          // Are we actively polling?
unsigned long lastPollMillis = 0;    // Last poll time.
const unsigned long POLL_INTERVAL = 5000;  // Poll every 5 seconds.
String pollFactor = "";              // Factor number to poll for.

bool exitPolling = false;



// ----- Keypad Setup -----
const byte ROWS = 4;
const byte COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};


// Based on your wiring, the 4 row pins are:
byte rowPins[ROWS] = {32, 33, 25, 26};  
// And the 4 column pins are:
byte colPins[COLS] = {27, 14, 12, 13};  

Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);


// Function to correct the key mapping
char fixMapping(char k) {
  switch (k) {
    case '1': return '1';
    case '4': return '2';
    case '7': return '3';
    case '*': return 'A';
    case '2': return '4';
    case '5': return '5';
    case '8': return '6';
    case '0': return 'B';
    case '3': return '7';
    case '6': return '8';
    case '9': return '9';
    case '#': return 'C';
    case 'A': return '*';
    case 'B': return '0';
    case 'C': return '#';
    case 'D': return 'D';
    default:  return k;
  }
}


// ----- Definitions for factor history -----
const int MAX_HISTORY = 15;

struct FactorRecord {
  String factorNumber;
  String deliveryStatus;
  unsigned long timestamp;  // in case you want to sort/supplement later
};

// For simplicity we use a fixed-size array. Manage count and insertion accordingly.
FactorRecord factorHistory[MAX_HISTORY];
int factorHistoryCount = 0;  // number of records currently stored
TaskHandle_t pollingTaskHandle = NULL;

// Browsing mode state:
bool statusBrowsingMode = false;
int currentStatusIndex = 0;

unsigned long browsingModeEnteredTime = 0;


// Timer variables:
unsigned long lastKeyInteractionTime = 0; // Updated on every key press in browsing mode.
bool liveUpdateCalled = false;             // Ensures live update is called only once per 3-sec inactivity.


// Save the history array to SPIFFS as a JSON file.
void saveHistory() {
  // Use a DynamicJsonDocument with enough capacity.
  const size_t capacity = JSON_ARRAY_SIZE(MAX_HISTORY) + MAX_HISTORY * JSON_OBJECT_SIZE(3) + 512;
  DynamicJsonDocument doc(capacity);
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < factorHistoryCount; i++) {
    JsonObject record = arr.createNestedObject();
    record["factorNumber"] = factorHistory[i].factorNumber;
    record["deliveryStatus"] = factorHistory[i].deliveryStatus;
    record["timestamp"] = factorHistory[i].timestamp;
  }
  
  File file = SPIFFS.open("/history.json", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open /history.json for writing");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("History saved!");
}

// Load the history from SPIFFS into your factorHistory array.
void loadHistory() {
  File file = SPIFFS.open("/history.json", FILE_READ);
  if (!file) {
    Serial.println("No history file found");
    return;
  }
  size_t size = file.size();
  if (size > 2048) {
    Serial.println("History file size is too large");
    file.close();
    return;
  }
  std::unique_ptr<char[]> buf(new char[size]);
  file.readBytes(buf.get(), size);
  file.close();

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, buf.get());
  if (error) {
    Serial.println("Failed to parse history file");
    return;
  }
  JsonArray arr = doc.as<JsonArray>();

  factorHistoryCount = 0;  // Reset current history count
  for (JsonObject obj : arr) {
    if (factorHistoryCount < MAX_HISTORY) {
      factorHistory[factorHistoryCount].factorNumber = obj["factorNumber"].as<String>();
      factorHistory[factorHistoryCount].deliveryStatus = obj["deliveryStatus"].as<String>();
      factorHistory[factorHistoryCount].timestamp = obj["timestamp"].as<unsigned long>();
      factorHistoryCount++;
    }
  }
  Serial.println("History loaded: " + String(factorHistoryCount) + " records");
}



// Helper function: Wait for user to press either 'B' (confirm) or 'A' (cancel)
// Uses the new keypad library and applies fixMapping() to convert the raw key.
char waitForKeyDecision() {
  char decision = NO_KEY;
  while (true) {
    // Read a raw key from the keypad.
    char rawKey = keypad.getKey();
    if (rawKey != NO_KEY) {
      // Convert it using fixMapping().
      decision = fixMapping(rawKey);
      // Check if the fixed decision is one of the expected keys.
      if (decision == 'B' || decision == 'A') {
        return decision;
      }
    }
    delay(50);
  }
}


// --- Function to send the factor to the Django server ---
// Returns a String with the server's JSON response.
String sendFactorToServer(String factor) {
  String response = "";
  
  // Check for WiFi connectivity.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi not connected");
    delay(1500);
    return "{\"success\": false, \"error\": \"WiFi not connected\"}";
  }

  // Prepare the initial HTTP POST.
  HTTPClient http;
  http.begin(processUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", apiToken);

  // Build JSON payload with key "factorNumber"
  String jsonPayload = "{\"factorNumber\": \"" + factor + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);

  lcd.clear();

  if (httpResponseCode > 0) {
    if (httpResponseCode == 200) {
      response = http.getString();
      Serial.print("Initial Server Response: ");
      Serial.println(response);

      // Check for an invalid factor response.
      if (response.indexOf("\"success\": false") != -1 &&
          response.indexOf("\"error\": \"No data found for the given factor number.\"") != -1) {
        lcd.clear();
        lcd.setCursor(5, 0);
        lcd.print("Error!");
        lcd.setCursor(0, 1);
        lcd.print("Factor Not Found!");
        delay(2000);
        lcd.clear();
        http.end();
        delay(10);
        return response;  // Return error response; do not update history.
      }

      // Check for a warning (e.g., date mismatch).
      if (response.indexOf("\"warning\": true") != -1) {
        // Extract the factor date from the response.
        int startIdx = response.indexOf("\"FCT_Date\": \"") + 13;
        int endIdx = response.indexOf("\"", startIdx);
        String factorDate = response.substring(startIdx, endIdx);

        // Display warning: show factor date and prompt for confirmation.
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("OK:B | Cancel:A");
        lcd.setCursor(0, 1);
        lcd.print("FDate:" + factorDate);

        // Wait for user decision using the new keypad helper function.
        char decision = waitForKeyDecision();
        if (decision == 'B') {  // User confirmed: continue.
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Sending...");
          
          // Send confirmation to Django API.
          HTTPClient confirmHttp;
          confirmHttp.begin(processUrl);
          confirmHttp.addHeader("Content-Type", "application/json");
          confirmHttp.addHeader("Authorization", apiToken);
          String confirmPayload = "{\"factorNumber\": \"" + factor + "\", \"confirmation\": \"OK\"}";
          int confirmResponseCode = confirmHttp.POST(confirmPayload);
          confirmHttp.end();
          
          // Update globals.
          lastFactorSent = factor;
          pollFactor = factor;
          pollingActive = true;
          lastPollMillis = millis();
          
          // Use a confirmation response string.
          response = "{\"success\": true, \"warningHandled\": true}";
        }
        else if (decision == 'A') {  // User cancelled.
          lcd.clear();
          lcd.setCursor(4, 0);
          lcd.print("CanceleD");
          delay(1500);
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Factor:" + factorNumber);
          lcd.setCursor(0, 1);
          lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
          http.end();
          delay(500);
          return "{\"success\": false, \"canceled\": true}";
        }
      }
      else {
        // No warning: proceed normally.
        lcd.setCursor(0, 0);
        lcd.print("Sent Successfully");
        lcd.setCursor(0, 1);
        lcd.print("Factor:" + factor);
        lastFactorSent = factor;
        pollFactor = factor;
        pollingActive = true;
        lastPollMillis = millis();
      }
    }
    else {  // HTTP response code indicates error.
      lcd.setCursor(0, 0);
      lcd.print("HTTP error:");
      lcd.setCursor(0, 1);
      lcd.print(httpResponseCode);
    }
  }
  else {
    lcd.setCursor(0, 0);
    lcd.print("POST failed");
  }
  
  http.end();
  delay(1500);
  return response;
}




// --- Function to perform one poll attempt for final notification status ---
void doPollNotificationStatus() {
  if (!pollingActive)
    return;
  
  HTTPClient http;
  // Build URL with the factor query parameter.
  String pollUrl = String(checkStatusUrl) + "?factorNumber=" + pollFactor;
  http.begin(pollUrl);
  http.addHeader("Authorization", apiToken);
  int httpResponseCode = http.GET();
  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.print("Polling Response: ");
    Serial.println(response);
    

    String smsStatus;

    if (response.indexOf("SMS Processing") != -1) {
        smsStatus = "Proc";
    } else if (response.indexOf("SMS Sent Telecom") != -1) {
        smsStatus = "TLCM";
    } else if (response.indexOf("SMS Delivered") != -1) {
        smsStatus = "Done";
    } else {
        smsStatus = "???";
    }





    // Parse WhatsApp status: if "Message sent successfully" exists then "OK", else "Proc"
    String waStatus = (response.indexOf("Message sent successfully") != -1) ? "Done" : "Proc";
    
    // For live update: update LCD with factor number and current statuses.
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("F:" + pollFactor);  // e.g., "F:104"
    // Create custom character at CGRAM index 0
    lcd.setCursor(8, 0);
    lcd.write((byte)0);  // Display smiley face emoji

    lcd.setCursor(0, 1);
    lcd.print("SMS:" + smsStatus + " WA:" + waStatus);
    
    // If the response contains a sendID, extract and store it.
    int idx = response.indexOf("sendID");
    if (idx != -1) {
      // A very simple extraction: look for the first double-quote after "sendID":
      int start = response.indexOf("\"", idx + 7);
      int end = response.indexOf("\"", start + 1);
      if (start != -1 && end != -1) {
        lastSendID = response.substring(start + 1, end);
        Serial.print("Extracted sendID: ");
        Serial.println(lastSendID);
      }
    }
    
    // If both statuses are final (i.e., not "Proc"), then stop polling.
    if (smsStatus != "Proc" && waStatus != "Proc") {
      pollingActive = false;
    }
  } else {
    Serial.print("Polling HTTP error: ");
    Serial.println(httpResponseCode);
  }
  http.end();
}



void doPollNotificationStatus_for_dual_core(void * parameter) {
  for (;;) {
    if (statusBrowsingMode && pollFactor.length() > 0 && (millis() - lastKeyInteractionTime >= BROWSING_UPDATE_DELAY)) {
      HTTPClient http;
      String pollUrl = String(checkStatusUrl) + "?factorNumber=" + pollFactor;
      http.begin(pollUrl);
      http.addHeader("Authorization", apiToken);
      int httpResponseCode = http.GET();

      if (httpResponseCode == 200) {
        String response = http.getString();
        Serial.print("Polling Response: ");
        Serial.println(response);

        // Determine the SMS and WhatsApp status.
        String smsStatus = response.indexOf("SMS Processing") != -1 ? "Proc" :
                           response.indexOf("SMS Sent Telecom") != -1 ? "TLCM" :
                           response.indexOf("SMS Delivered") != -1 ? "Done" : "???";

        String waStatus = response.indexOf("Message sent successfully") != -1 ? "Done" :
                          response.indexOf("Task timed out and WhatsApp service was restarted") != -1 ? "RST!" :
                          "Proc";

        // New status string for LCD display.
        String newStatus = "SMS:" + smsStatus + " WA:" + waStatus + "    ";

        // Step 4: Update line 2 of LCD only on first call or when the status changes.
        if (firstLiveUpdate || newStatus != lastDisplayedStatus) {
          if (xSemaphoreTake(lcdMutex, (TickType_t) 100) == pdTRUE) {
            lcd.setCursor(0, 1);
            lcd.print(newStatus);
            delay(50);
            xSemaphoreGive(lcdMutex);
          }
          lastDisplayedStatus = newStatus;
          if (firstLiveUpdate = true) {
            Serial.print("firstLiveUpdate HAS BEEN CALLED");
          }
          firstLiveUpdate = false;  // Disable first update after first successful call.
        }

      } else {
        Serial.print("Polling HTTP error: ");
        Serial.println(httpResponseCode);
      }
      http.end();
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Poll every 1 second.
  }
}



SMSBalance getSMSBalance() {
  HTTPClient http;
  
  // Use POST instead of GET for better security
  String url = smsAccountBalance; // No API key in URL anymore
  http.begin(url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("API-Key", apiToken); // Send API key safely in headers
  
  String payload = "{\"action\":\"credit\"}";
  
  int httpCode = http.POST(payload);  // Send request as POST
  
  String responsePayload;
  
  if (httpCode == 200) {
    responsePayload = http.getString();
  } 
  else {
    responsePayload = "{\"error\":\"HTTP " + String(httpCode) + "\"}";
  }

  http.end();

  // Parse JSON response.
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, responsePayload);

  SMSBalance result;
  
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    result.credit = "Error";
    result.messagesSendable = 0;
    return result;
  }
  
  result.credit = doc["credit"].as<String>();
  result.messagesSendable = doc["messages_sendable"];
  
  return result;
}








// Call this after successfully sending a factor.
// It ensures a factor appears only once. If it exists, update its record and move it to the top.
void updateFactorHistory(String factor, String newStatus) {
  int foundIndex = -1;
  for (int i = 0; i < factorHistoryCount; i++) {
    if (factorHistory[i].factorNumber == factor) {
      foundIndex = i;
      break;
    }
  }
  // If found, shift it to the front.
  if (foundIndex != -1) {
    FactorRecord temp = factorHistory[foundIndex];
    // Move each record above the found record one position down.
    for (int i = foundIndex; i > 0; i--) {
      factorHistory[i] = factorHistory[i - 1];
    }
    temp.deliveryStatus = newStatus;  // update status
    temp.timestamp = millis();        // update timestamp if needed
    factorHistory[0] = temp;
  } else {
    // Not found: insert new record at the beginning.
    // Shift all records one position down if history is full.
    if (factorHistoryCount == MAX_HISTORY) {
      for (int i = MAX_HISTORY - 1; i > 0; i--) {
        factorHistory[i] = factorHistory[i - 1];
      }
      factorHistory[0].factorNumber = factor;
      factorHistory[0].deliveryStatus = newStatus;
      factorHistory[0].timestamp = millis();
    } else {
      // Shift existing records.
      for (int i = factorHistoryCount; i > 0; i--) {
        factorHistory[i] = factorHistory[i - 1];
      }
      factorHistory[0].factorNumber = factor;
      factorHistory[0].deliveryStatus = newStatus;
      factorHistory[0].timestamp = millis();
      factorHistoryCount++;
    }
  }
  
  // Save the updated history to flash (SPIFFS)
  saveHistory();
}



void setup() {
  // ---------- Serial + LCD Initialization ----------
  Serial.begin(115200);
  Serial.println("Starting System...");

  // Create a custom character at CGRAM index 0 (make sure 'smiley' array is defined globally)
  lcd.createChar(0, smiley);
  lcd.begin(16, 2);

  ///////////////////////////////////////////////////////////////////

  lcdMutex = xSemaphoreCreateMutex();
    if (lcdMutex == NULL) {
        Serial.println("Failed to create LCD mutex!");
    }

  //////////////////////////////////////////////////////////////////



  //lcd.noAutoscroll();
  // Display initial factor information.
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Factor:");
  lcd.setCursor(3, 0);
  lcd.print(factorNumber);

  // ---------- WiFi Connection ----------
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connecting...");
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected!");
  
  // After connection, re-display the factor prompt.
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Factor:");
  lcd.setCursor(3, 0);
  lcd.print(factorNumber);

  // ---- Fetch Account Balance ONLY Once ----
  if (!balanceChecked) {

    
    SMSBalance balance = getSMSBalance();  // Function to fetch balance from Django
    smsBalanceCredit = balance.credit;
    smsMessagesSendable = balance.messagesSendable;


    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Factor:");
    lcd.setCursor(3, 0);
    lcd.print(factorNumber);
    



    int retryCount = 0;
    while ((balance.credit == "null" || balance.credit.length() == 0) && retryCount < 5) {
        Serial.println("Balance fetch failed! Retrying in 1 second...");
        delay(1000);  // Allow time before retrying
        balance = getSMSBalance();  // Function to fetch balance from Django
        smsBalanceCredit = balance.credit;
        smsMessagesSendable = balance.messagesSendable;
        retryCount++;  // Increment the retry counter
    }

    // If balance is still null after 5 tries, display an error message
    if (balance.credit == "null" || balance.credit.length() == 0) {
        Serial.println("Failed to fetch balance after 5 attempts!");
        lcd.setCursor(12, 1);
        lcd.print("ERR!");
    } else {
        lcd.setCursor(0, 1);
        lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
    }

    lcd.setCursor(0, 1);
    lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));


    Serial.println("Balance: $" + smsBalanceCredit);
    Serial.println("Messages Sendable: " + String(smsMessagesSendable));

    balanceChecked = true;  // Prevent multiple requests
  }
  
  delay(3000);  // Give time for users to see the balance before proceeding

  // ---------- SPIFFS Initialization ----------
  if (!SPIFFS.begin(true)) {  // Format SPIFFS if mounting fails
    Serial.println("Error mounting SPIFFS");
  } else {
    Serial.println("SPIFFS mounted successfully");
    loadHistory();  // Restore factorHistory from flash memory.
  }

  // ---------- Start Polling Task ----------
  if (pollingTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      doPollNotificationStatus_for_dual_core, // Task function (defined elsewhere)
      "PollingTask",    // Task name
      5000,             // Stack size (in words)
      NULL,             // Parameter passed (none)
      1,                // Task priority
      &pollingTaskHandle, // Task handle (global)
      0                 // Run on core 0 (adjust as needed)
    );
  }
}



void handleBrowsingModeKey(char key) {
  // Reset timers on every key press.
  lastKeyInteractionTime = millis();
  liveUpdateCalled = false;
  
  switch (key) {
    case '8': {  // Scroll up.
      currentStatusIndex--;
      firstLiveUpdate = true;  // Reset first update when switching factors.

      if (currentStatusIndex < 0)
        currentStatusIndex = factorHistoryCount - 1;
        
      break;
    }
    case '2': {  // Scroll down.
      currentStatusIndex++;
      firstLiveUpdate = true;  // Reset first update when switching factors.

      if (currentStatusIndex >= factorHistoryCount)
        currentStatusIndex = 0;
        
      break;
    }
    case 'D': {  // Delete history.
      // Clear the display to show deletion prompt.
      firstLiveUpdate = true;  // Reset first update when switching factors.
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Delete History?");
      lcd.setCursor(0, 1);
      lcd.print("B:Yes  A:No");
      
      char decision = waitForKeyDecision();
      if (decision == 'B') {  // Confirm deletion.
        factorHistoryCount = 0;
        if (SPIFFS.exists("/history.json"))
          SPIFFS.remove("/history.json");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("History Deleted");
        delay(1500);
      }
      else if (decision == 'A') {  // Cancel deletion.
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Delete Canceled");
        delay(1000);
      }
      break;
    }
    case 'B': {  // Resend the currently displayed factor.
      if (factorHistoryCount > 0) {
        // Send this specific factor to Django.
        firstLiveUpdate = true;  // Reset first update when switching factors.
        sendFactorToServer(factorHistory[currentStatusIndex].factorNumber);
        updateFactorHistory(factorHistory[currentStatusIndex].factorNumber, "Resent");
      }
      // Exit browsing mode.
      statusBrowsingMode = false;
      break;
    }
    case 'A': {  // Exit browsing mode.

      factorNumber = "";
      statusBrowsingMode = false;  // Ensure browsing mode is off.
      firstLiveUpdate = true;  // Reset first update when switching factors.
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Factor:" + factorNumber);
      lcd.setCursor(0, 1);
      lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
      break;
    }
    default:
      break;
  }
  
  // Update pollFactor for live polling:
  if (factorHistoryCount > 0)
    pollFactor = factorHistory[currentStatusIndex].factorNumber;
      // When updating row 0:
      if (xSemaphoreTake(lcdMutex, (TickType_t) 10) == pdTRUE) {
        // --- Refresh the LCD display without entirely clearing it ---
        // Update row 0 (history overview) so that live update on row 1 is not wiped out.
        // First, update row 0:
        lcd.setCursor(0, 0);
        String pos = String(currentStatusIndex + 1) + "/" + String(factorHistoryCount);
        String factorText = "Factor:" + factorHistory[currentStatusIndex].factorNumber;
        lcd.print(factorText);
        // Clear any trailing characters in row 0 by printing spaces:
        int factorTextLength = factorText.length();
        for (int i = factorTextLength; i < 16; i++) {
          lcd.print(" ");
        }
        // Write the position on the same row (right-justified):
        int posStart = 16 - pos.length();
        lcd.setCursor(posStart, 0);
        lcd.print(pos);
        lcd.setCursor(10, 0);
        lcd.print("|");

        // Update row 1 (status) only if not overridden by live update.
        lcd.setCursor(0, 1);
        String statusText = factorHistory[currentStatusIndex].deliveryStatus;
        lcd.print(statusText);
        int stLen = statusText.length();
        for (int i = stLen; i < 16; i++) {
          lcd.print(" ");
        }
        delay(10);
        xSemaphoreGive(lcdMutex);
  }
}




void handleNormalModeKey(char key) {


  switch (key) {
    case '*': {  // Enter browsing mode.
      if (factorHistoryCount > 0) {
        currentStatusIndex = 0;
        statusBrowsingMode = true;
        browsingModeStartTime = millis();    // Set browsing mode start time.
        lastKeyInteractionTime = millis();     // Reset inactivity timer.
        liveUpdateCalled = false;
        pollFactor = factorHistory[currentStatusIndex].factorNumber;
        
        lcd.clear();
        String pos = String(currentStatusIndex + 1) + "/" + String(factorHistoryCount);
        lcd.setCursor(0, 0);
        lcd.print("Factor:" + factorHistory[currentStatusIndex].factorNumber);
        int posStart = 16 - pos.length();
        lcd.setCursor(posStart, 0);
        lcd.print(pos);
        lcd.setCursor(10, 0);
        lcd.print("|");
        lcd.setCursor(0, 1);
        lcd.print(factorHistory[currentStatusIndex].deliveryStatus);
      }
      break;
    }
    
    case 'A': {  // Clear the current factor entry.
      factorNumber = "";
      statusBrowsingMode = false;  // Ensure browsing mode is off.
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Factor:" + factorNumber);
      lcd.setCursor(0, 1);
      lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
      break;
    }
    
    case 'B': {  // Send factor.
      if (factorNumber.length() > 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Sending...");
        lcd.setCursor(0, 1);
        lcd.print(factorNumber);
        // ✅ **Call balance function ONLY AFTER factor submission**
        SMSBalance balance = getSMSBalance();
        smsBalanceCredit = balance.credit;
        smsMessagesSendable = balance.messagesSendable;
      }
      String response = sendFactorToServer(factorNumber);
      Serial.println("Server Response: " + response);
      
      // Declare JSON document and related variables in this block.
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, response);
      bool valid = false;
      
      if (err) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("JSON Error");
        lcd.setCursor(0, 1);
        lcd.print("Send Failed");
        delay(10);
      }
      else {
        bool success = doc["success"];
        if (!success) {
          String errMsg = doc["error"].as<String>();
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Error:");
          lcd.setCursor(0, 1);
          lcd.print(errMsg);
          delay(10);
        }
        else {
          if (doc.containsKey("warning") && doc["warning"].as<bool>() == true) {
            String warnMsg = doc["message"].as<String>();
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print(warnMsg);
            lcd.setCursor(0, 1);
            lcd.print("B:cont A:cancel");
            
            bool decisionMade = false;
            while (!decisionMade) {
              char decision = waitForKeyDecision();
              if (decision == 'B') {
                valid = true;
                decisionMade = true;
              }
              else if (decision == 'A') {
                valid = false;
                decisionMade = true;
              }
              delay(300);
            }
          }
          else {
            valid = true;
          }
        }
      }
      
      if (valid) {
        updateFactorHistory(factorNumber, "Sent To Server");
      }
      factorNumber = "";
      break;
    }
    
    case 'C': {  // Manual balance refresh.
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("ACC Balance $:");
      
      SMSBalance balance = getSMSBalance();
      smsBalanceCredit = balance.credit;
      smsMessagesSendable = balance.messagesSendable;
      
      lcd.setCursor(0, 1);
      lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
      
      Serial.println("Balance: $" + smsBalanceCredit);
      Serial.println("Msgs Left: " + String(smsMessagesSendable));
      delay(1500);
      break;
    }
    
    default: {
      // For numeric keys, append to factorNumber.
      if (key >= '0' && key <= '9') {
        factorNumber += key;
      }
      break;
    }
  } // End of switch
  
  // If not currently browsing, update the LCD in normal mode.
  if (!statusBrowsingMode) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Factor:" + factorNumber);
    lcd.setCursor(0, 1);
    lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
  }
}


void loop() {
  // ----- Live Polling via Dual-Core Task -----
  if (pollingActive && (millis() - lastPollMillis >= POLL_INTERVAL)) {
    lastPollMillis = millis();
    doPollNotificationStatus();
  }
  
  // ----- Check for a Key Press using the new keypad library -----
  char key = keypad.getKey();  
  if (key != NO_KEY) {
    // Immediately fix the key mapping.
    char fixedKey = fixMapping(key);
    Serial.print("Key Pressed: ");
    Serial.println(fixedKey);
    
    // Disable polling when a key is pressed.
    pollingActive = false;
    
    // Delegate handling based on the current mode using the fixed key.
    if (statusBrowsingMode)
      handleBrowsingModeKey(fixedKey);
    else
      handleNormalModeKey(fixedKey);
      
    delay(10);  // Debounce delay.
  }
  
  // ----- Browsing Mode Idle Handling -----
  if (statusBrowsingMode) {
    unsigned long currentTime = millis();
    unsigned long inactivity = currentTime - lastKeyInteractionTime;
    unsigned long browsingDuration = currentTime - browsingModeStartTime;
    
    if (browsingDuration >= BROWSING_TIMEOUT) {
      statusBrowsingMode = false;
      firstLiveUpdate = true;
      lcd.clear();
      lcd.setCursor(4, 0);
      lcd.print("TimeouT!");
      delay(1000);
      // Restore Normal Mode Display (for example):
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Factor:" + factorNumber);
      lcd.setCursor(0, 1);
      lcd.print("$" + smsBalanceCredit + " M:" + String(smsMessagesSendable));
    }
    // Only update live status if browsing mode has been active for enough time,
    // and the user has been idle for at least BROWSING_UPDATE_DELAY.
    else if ((browsingDuration >= MIN_BROWSING_MODE_TIME) &&
             (inactivity >= BROWSING_UPDATE_DELAY))
    {
      // Instead of updating here on the main loop, let the dual-core
      // task handle live update. (But if you must, update only row 1.)
      lcd.setCursor(0, 1);
      lcd.print("");
      // You could also call a function to update live status here instead.
    }
  }
  
  delay(10);  // General delay to avoid flooding.
}


