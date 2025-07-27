/**
 * @file main.ino
 * @author Naman Kansal 
 * @brief Firmware with instant recording and parallel time-sync for file naming.
 *
 * @version 7.0 PARALLEL_NAMING
 *
 * @description
 * This version provides an instant recording experience by deferring file naming.
 * - When the record button is pressed, recording starts IMMEDIATELY to a temporary file.
 * - In the background, while recording, the device connects to WiFi to fetch the current time.
 * - Once synced, the temporary file is renamed to the final timestamped format.
 * - This eliminates startup delay while still providing accurate filenames.
 */

// --- LIBRARIES ---
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <SPI.h>
#include "driver/i2s_std.h"
#include "mbedtls/md.h"
#include "time.h"
#include "esp_sleep.h"

// ======================================================================================
// --- USER CONFIGURATION ---
// ======================================================================================
#define PUSH_BUTTON_PIN GPIO_NUM_3
#define CHARGING_PIN    GPIO_NUM_1
#define SD_CS_PIN       21
#define LED_PIN         2
#define I2S_WS          5
#define I2S_SD          4
#define I2S_SCK         6

#define MIC_NUMBER 1

const char* ssid = "Airtel_Aman Kansal";
const char* password = "@d1r2m3a4n5";
const char* accessKey = "   ";
const char* secretKey = "";
const char* host = "";
const char* bucket = "";
const char* region = "us-east-1";
const char* service = "s3";
const char* contentType = "audio/wav";

#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define GAIN_BOOSTER_I2S 32

const unsigned long MAX_RECORD_TIME_MS = 15 * 60 * 1000;
const long gmtOffset_sec_IST = 19800;
const int daylightOffset_sec = 0;

// ======================================================================================
// --- GLOBAL VARIABLES & OBJECTS ---
// ======================================================================================
WiFiClientSecure client;
i2s_chan_handle_t rx_handle;
String currentAudioFile = "";
String baseFilename = "";
int segmentCounter = 1;
bool isRecording = false;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 75;
int buttonState;
int lastButtonState = HIGH;
unsigned long segmentStartTime = 0;

RTC_DATA_ATTR int audioSessionCounter = 1;
bool nameIsTemporary = false; // Flag to track if the file needs renaming

struct WAV_HEADER {
    char riff[4] = {'R', 'I', 'F', 'F'}; long flength = 0;
    char wave[4] = {'W', 'A', 'V', 'E'}; char fmt[4] = {'f', 'm', 't', ' '};
    long chunk_size = 16; short format_tag = 1; short num_chans = 1;
    long srate = SAMPLE_RATE; long bytes_per_sec = SAMPLE_RATE * (BITS_PER_SAMPLE / 8);
    short bytes_per_samp = (BITS_PER_SAMPLE / 8); short bits_per_samp = BITS_PER_SAMPLE;
    char dat[4] = {'d', 'a', 't', 'a'}; long dlength = 0;
} myWAV_Header;

// --- FORWARD DECLARATIONS ---
void goToDeepSleep();
void uploadAllFiles();
void checkChargingStatus();
bool I2S_Record_Init();
void createNewSegment(String filename);
void appendAudioData(String filename);
void finalizeAndStopRecording(String filename);
void finalizeSegment(String filename);
void syncTimeUTC();
void handleParallelNaming();
bool uploadFile(const char* filename);

// ======================================================================================
// --- CORE DEVICE STATE MANAGEMENT ---
// ======================================================================================
void goToDeepSleep() {
    Serial.println("Configuring wake-up sources and entering deep sleep...");
    Serial.flush();
    esp_sleep_enable_ext0_wakeup(PUSH_BUTTON_PIN, 0);
    uint64_t charging_mask = 1ULL << CHARGING_PIN;
    esp_sleep_enable_ext1_wakeup(charging_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}

void checkChargingStatus() {
    if (digitalRead(CHARGING_PIN) == LOW) {
        Serial.println("\n🔌 Charging stopped! Aborting operations and going to sleep.");
        WiFi.disconnect(true);
        client.stop();
        goToDeepSleep();
    }
}

// ======================================================================================
// --- MINIO S3 UPLOAD FUNCTIONS (STABLE) ---
// ======================================================================================
void syncTimeUTC() {
    Serial.println("Syncing time via NTP for upload process...");
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.print("Waiting for time synchronization");
    time_t now = time(nullptr);
    while (now < 1672531200) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.println();
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.printf("✅ UTC Time Synced: %s", asctime(&timeinfo));
}

void hashByteToHex(unsigned char*h,size_t l,char*r){for(int i=0;i<l;i++)sprintf(r+(i*2),"%02x",h[i]);r[l*2]='\0';}
void sha256FileHash(File&f,char*o){mbedtls_md_context_t c;mbedtls_md_type_t t=MBEDTLS_MD_SHA256;unsigned char h[32];mbedtls_md_init(&c);mbedtls_md_setup(&c,mbedtls_md_info_from_type(t),0);mbedtls_md_starts(&c);while(f.available()){uint8_t b[512];size_t l=f.read(b,sizeof(b));mbedtls_md_update(&c,b,l);}mbedtls_md_finish(&c,h);mbedtls_md_free(&c);hashByteToHex(h,32,o);f.seek(0);}

bool uploadFile(const char* filename) {
    checkChargingStatus();
    String path = String("/") + filename;
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        Serial.printf("[ERROR] Failed to open file for upload: %s\n", path.c_str());
        return false;
    }
    size_t contentLength = file.size();
    Serial.println("\n--- 📦 Starting Upload Process ---");
    Serial.printf("[LOG] Filename: %s\n", filename);
    Serial.printf("[LOG] File size: %d bytes\n", contentLength);
    static char fileHashHex[65], canonicalReq[512], stringToSign[512], authHeader[512], httpHeader[1024];
    sha256FileHash(file,fileHashHex);
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("❌ Failed to get local time.");
        file.close();
        return false;
    }
    char date[9],timeStr[7];
    strftime(date,sizeof(date),"%Y%m%d",&timeinfo);
    strftime(timeStr,sizeof(timeStr),"%H%M%S",&timeinfo);
    Serial.printf("[DEBUG] Using timestamp for signature: %sT%sZ\n", date, timeStr);
    snprintf(canonicalReq,sizeof(canonicalReq),"PUT\n/%s/%s\n\nhost:%s\nx-amz-content-sha256:%s\nx-amz-date:%sT%sZ\nx-amz-storage-class:STANDARD\n\nhost;x-amz-content-sha256;x-amz-date;x-amz-storage-class\n%s",bucket,filename,host,fileHashHex,date,timeStr,fileHashHex);
    unsigned char canonHash[32];mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),(const unsigned char*)canonicalReq,strlen(canonicalReq),canonHash);
    char canonHashHex[65];hashByteToHex(canonHash,32,canonHashHex);
    snprintf(stringToSign,sizeof(stringToSign),"AWS4-HMAC-SHA256\n%sT%sZ\n%s/%s/%s/aws4_request\n%s",date,timeStr,date,region,service,canonHashHex);
    char keySecret[100]="AWS4";strcat(keySecret,secretKey);unsigned char kDate[32],kRegion[32],kService[32],kSigning[32],signatureRaw[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),(const unsigned char*)keySecret,strlen(keySecret),(const unsigned char*)date,strlen(date),kDate);mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),kDate,32,(const unsigned char*)region,strlen(region),kRegion);mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),kRegion,32,(const unsigned char*)service,strlen(service),kService);mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),kService,32,(const unsigned char*)"aws4_request",12,kSigning);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),kSigning,32,(const unsigned char*)stringToSign,strlen(stringToSign),signatureRaw);
    char signatureHex[65];hashByteToHex(signatureRaw,32,signatureHex);
    snprintf(authHeader,sizeof(authHeader),"AWS4-HMAC-SHA256 Credential=%s/%s/%s/%s/aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-storage-class, Signature=%s",accessKey,date,region,service,signatureHex);
    snprintf(httpHeader,sizeof(httpHeader),"PUT /%s/%s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\nContent-Type: %s\r\nX-Amz-Date: %sT%sZ\r\nX-Amz-Content-Sha256: %s\r\nX-Amz-Storage-Class: STANDARD\r\nAuthorization: %s\r\nConnection: keep-alive\r\n\r\n",bucket,filename,host,contentLength,contentType,date,timeStr,fileHashHex,authHeader);
    client.setInsecure();
    Serial.printf("[LOG] Connecting to host: %s\n", host);
    if (!client.connect(host, 443)) {
        Serial.println("[ERROR] Connection to host failed!");
        file.close();
        return false;
    }
    Serial.println("[SUCCESS] Connected to host.");
    Serial.println("[LOG] Sending HTTP headers...");
    client.print(httpHeader);
    Serial.println("[LOG] HTTP headers sent.");
    size_t totalBytesSent = 0;
    uint32_t chunkCount = 0;
    const int CHUNK_SIZE = 1024;
    uint8_t buf[CHUNK_SIZE];
    Serial.println("[LOG] Starting file data transmission...");
    while (file.available()) {
        checkChargingStatus();
        if (!client.connected()) {
            Serial.println("[ERROR] Server disconnected during upload. Aborting.");
            break;
        }
        size_t bytesRead = file.read(buf, CHUNK_SIZE);
        if (bytesRead > 0) {
            size_t bytesSent = client.write(buf, bytesRead);
            if (bytesSent != bytesRead) {
                Serial.printf("[ERROR] Network write failed! Expected to send %d, but sent %d. Aborting.\n", bytesRead, bytesSent);
                break;
            }
            totalBytesSent += bytesSent;
            chunkCount++;
            if (chunkCount % 64 == 0) {
                float percentage = (float)totalBytesSent * 100.0 / (float)contentLength;
                Serial.printf("[PROGRESS] Uploaded %u / %u bytes (%.2f%%)\n", totalBytesSent, contentLength, percentage);
            }
        }
        delay(2);
    }
    if (totalBytesSent != contentLength) {
        Serial.printf("[ERROR] File transmission incomplete. Sent %u of %u bytes.\n", totalBytesSent, contentLength);
        client.stop();
        file.close();
        return false;
    }
    Serial.printf("[SUCCESS] File data sent. Total bytes: %u\n", totalBytesSent);
    Serial.println("[LOG] Waiting for server response...");
    String response_header = "";
    unsigned long responseStartTime = millis();
    while (millis() - responseStartTime < 10000) {
        if (client.available()) {
            response_header = client.readStringUntil('\n');
            break;
        }
        delay(10);
    }
    Serial.println("\n--- 📝 Server Response ---");
    if (response_header.length() > 0) {
        Serial.print(response_header);
        while(client.available()) { Serial.print(client.readString()); }
        Serial.println();
    } else {
        Serial.println("[WARN] No response from server or timeout reached.");
    }
    Serial.println("--------------------------\n");
    client.stop();
    file.close();
    Serial.println("[LOG] Connection closed and file handle released.");
    if (response_header.indexOf("200 OK") != -1) {
        Serial.println("✅ Upload success. Deleting file.");
        if (!SD.remove(path.c_str())) {
            Serial.println("⚠️ Failed to delete file.");
        }
        return true;
    } else {
        Serial.println("❌ Upload failed. Please check server response above.");
        return false;
    }
}

void uploadAllFiles(){
    Serial.println("📡 Connecting to WiFi...");
    WiFi.begin(ssid,password);
    int r=0;
    while(WiFi.status() != WL_CONNECTED && r < 40){
        checkChargingStatus();
        delay(500);
        Serial.print(".");
        r++;
    }
    if(WiFi.status() != WL_CONNECTED){
        Serial.println("\n❌ WiFi failed.");
        return;
    }
    Serial.println("\n✅ WiFi Connected");
    syncTimeUTC();
    if(!SD.begin(SD_CS_PIN)){
        Serial.println("❌ SD init failed.");
        return;
    }
    Serial.println("💾 SD card initialized.");
    File root = SD.open("/");
    if(!root){
        Serial.println("❌ Failed to open root.");
        return;
    }
    while(true){
        checkChargingStatus();
        File e = root.openNextFile();
        if(!e) break;
        String fn = e.name();
        if(!e.isDirectory() && fn.endsWith(".wav")){
            Serial.printf("🚀 Found .wav file: %s. Beginning upload attempts.\n", fn.c_str());
            while (!uploadFile(fn.c_str())) {
                checkChargingStatus();
                Serial.printf("--> Upload failed for '%s'. Retrying in 10 seconds...\n", fn.c_str());
                delay(10000);
            }
        }
        e.close();
    }
    root.close();
    Serial.println("🎉 All files processed.");
}

// ======================================================================================
// --- I2S & NAMING FUNCTIONS ---
// ======================================================================================
bool I2S_Record_Init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    i2s_std_config_t std_cfg = {
        .clk_cfg = {.sample_rate_hz = SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_MONO, .slot_mask = I2S_STD_SLOT_RIGHT, .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true},
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_SCK, .ws = (gpio_num_t)I2S_WS, .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)I2S_SD, .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
    };
    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    i2s_channel_enable(rx_handle);
    return true;
}

void handleParallelNaming() {
    Serial.println("Attempting to get time for filename in background...");
    setCpuFrequencyMhz(240); // Use full speed for WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    char filename_buffer[50];
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi connected for time sync.");
        configTime(gmtOffset_sec_IST, daylightOffset_sec, "pool.ntp.org");
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10000)) {
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d_%H-%M-%S", &timeinfo);
            snprintf(filename_buffer, sizeof(filename_buffer), "mic%d_%s", MIC_NUMBER, time_str);
            baseFilename = String(filename_buffer);
        } else {
             Serial.println("❌ NTP sync failed. Using fallback name.");
            snprintf(filename_buffer, sizeof(filename_buffer), "mic%d_fallback_%d", MIC_NUMBER, audioSessionCounter);
            baseFilename = String(filename_buffer);
        }
    } else {
        Serial.println("\n❌ WiFi connection failed. Using fallback name.");
        snprintf(filename_buffer, sizeof(filename_buffer), "mic%d_fallback_%d", MIC_NUMBER, audioSessionCounter);
        baseFilename = String(filename_buffer);
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setCpuFrequencyMhz(80); // Go back to power-saving mode
    Serial.println("Shutting down WiFi. Renaming file...");

    String newFilename = "/" + baseFilename + "_part" + String(segmentCounter) + ".wav";
    if (SD.rename(currentAudioFile, newFilename)) {
        Serial.printf("✅ File renamed to: %s\n", newFilename.c_str());
        currentAudioFile = newFilename;
    } else {
        Serial.printf("❌ Failed to rename temporary file. It will be saved as %s\n", currentAudioFile.c_str());
        baseFilename = "temp_audio"; // Use a generic base for future parts
    }
    nameIsTemporary = false;
}

void createNewSegment(String filename) {
    File f = SD.open(filename.c_str(), FILE_WRITE);
    if (!f) {
        Serial.println("❌ Failed to create new segment file.");
        isRecording = false;
        return;
    }
    f.write((uint8_t*)&myWAV_Header, sizeof(myWAV_Header));
    f.close();
    Serial.printf("🎤 Recording new segment to %s...\n", filename.c_str());
}

void appendAudioData(String filename) {
    File f = SD.open(filename.c_str(), FILE_APPEND);
    if (!f) { return; }
    int16_t* b = (int16_t*)malloc(2048);
    if (!b) { f.close(); return; }
    size_t r = 0;
    i2s_channel_read(rx_handle, b, 2048, &r, pdMS_TO_TICKS(100));
    if (r > 0) {
        if (GAIN_BOOSTER_I2S > 1) {
            for (int i = 0; i < r / 2; ++i) b[i] = (int16_t)constrain((long)b[i] * GAIN_BOOSTER_I2S, -32768, 32767);
        }
        f.write((uint8_t*)b, r);
    }
    f.close();
    free(b);
}

void finalizeSegment(String filename) {
    File f = SD.open(filename.c_str(), "r+");
    if (!f) {
        Serial.printf("❌ Failed to finalize segment %s.\n", filename.c_str());
        return;
    }
    long s = f.size();
    myWAV_Header.flength = s - 8;
    myWAV_Header.dlength = s - sizeof(myWAV_Header);
    f.seek(0);
    f.write((uint8_t*)&myWAV_Header, sizeof(myWAV_Header));
    f.close();
    float d = (float)myWAV_Header.dlength / (SAMPLE_RATE * BITS_PER_SAMPLE / 8);
    Serial.printf("✅ Segment '%s' finalized. Duration: %.2f sec\n", filename.c_str(), d);
}

void finalizeAndStopRecording(String filename) {
    isRecording = false;
    Serial.println("⏹️ Stopping recording session...");
    finalizeSegment(filename);
}

// ======================================================================================
// --- MAIN SETUP & LOOP ---
// ======================================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    pinMode(LED_PIN, OUTPUT);
    pinMode(PUSH_BUTTON_PIN, INPUT_PULLUP);
    pinMode(CHARGING_PIN, INPUT);
    Serial.println("\n=======================================");
    Serial.println("ESP32-S3 Voice Recorder Booting");
    Serial.println("=======================================");
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("Wakeup: Button Press. Starting recording immediately.");
            
            setCpuFrequencyMhz(80);
            Serial.println("⚡️ Power saving mode enabled.");

            digitalWrite(LED_PIN, HIGH);
            if (!SD.begin(SD_CS_PIN) || !I2S_Record_Init()) {
                Serial.println("❌ SD/I2S Init Failed.");
                goToDeepSleep();
            }
            
            isRecording = true;
            // Start recording to a temporary file IMMEDIATELY
            currentAudioFile = "/temp_audio.wav";
            if (SD.exists(currentAudioFile)) {
                SD.remove(currentAudioFile);
            }
            createNewSegment(currentAudioFile);
            segmentStartTime = millis();
            
            // Set a flag to handle the renaming in the background
            nameIsTemporary = true;
            break;

        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("Wakeup: Charging Pin HIGH. Starting upload session.");
            digitalWrite(LED_PIN, HIGH);
            setCpuFrequencyMhz(240);
            uploadAllFiles();
            digitalWrite(LED_PIN, LOW);
            Serial.println("Upload process finished or was aborted.");
            goToDeepSleep();
            break;

        default:
            Serial.println("Wakeup: First boot or reset.");
            audioSessionCounter = 1;
            goToDeepSleep();
            break;
    }
}

void loop() {
    // --- Parallel File Renaming Logic ---
    if (nameIsTemporary && isRecording) {
        // This block will run once in the background. Recording is briefly paused.
        isRecording = false; // Pause recording
        finalizeSegment(currentAudioFile); // Finalize the temporary file so it can be renamed
        
        handleParallelNaming();
        
        isRecording = true; // Resume recording
    }


    if (isRecording) {
        if (millis() - segmentStartTime >= MAX_RECORD_TIME_MS) {
            Serial.println("\n⏰ 15 minute limit reached. Starting new file segment...");
            finalizeSegment(currentAudioFile);
            
            segmentCounter++;
            currentAudioFile = "/" + baseFilename + "_part" + String(segmentCounter) + ".wav";
            createNewSegment(currentAudioFile);
            
            segmentStartTime = millis();
        }
        
        appendAudioData(currentAudioFile);
    }

    int reading = digitalRead(PUSH_BUTTON_PIN);
    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
                if (isRecording || nameIsTemporary) {
                    nameIsTemporary = false; // Cancel any pending rename
                    finalizeAndStopRecording(currentAudioFile);
                    
                    audioSessionCounter++;
                    
                    digitalWrite(LED_PIN, LOW);

                    Serial.println("Recording stopped. Release button to sleep.");
                    while(digitalRead(PUSH_BUTTON_PIN) == LOW) {
                      delay(50);
                    }
                    
                    goToDeepSleep();
                }
            }
        }
    }
    lastButtonState = reading;
}
