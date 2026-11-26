#include <ModbusMaster.h>

// ---- EDIT THESE (keep real values out of any public copy) ----
const char* APN       = "YOUR_APN";                    // your SIM's APN
const char* TB_SERVER = "mqtt.thingsboard.cloud";      // or your own ThingsBoard host
const char* TB_PORT   = "1883";
const char* TB_TOKEN  = "YOUR_DEVICE_ACCESS_TOKEN";    // ThingsBoard device -> Manage credentials

// ---- Board pins ----
#define RS485_RX     PB7
#define RS485_TX     PB6
#define RS485_FC     PB4      
#define BOOST_EN     PA4      // HIGH = sensor supply ON (5V or 12V, set by the AUX jumper)
#define GSM_TX       PA2      // MCU TX -> modem RX
#define GSM_RX       PA3      // MCU RX <- modem TX
#define GSM_POWER    PA1
// The user guide says LOW = modem on, the datasheet says "active high". If the modem does not
// answer "AT", change this to HIGH.
#define GSM_ON_LEVEL LOW

// ---- Settings ----
#define SENSOR_ID       1
#define SENSOR_BAUD     9600
#define SENSOR_SCALE    100.0f
#define SENSOR_WARMUP   2000UL    // ms for the sensor to start after BOOST_EN goes HIGH
#define SEND_PERIOD_MS  60000UL   // time between readings
#define SENSOR_ALWAYS_ON 1       // 1 = keep BOOST_EN (PA4) HIGH all the time; 0 = switch it off after each reading
#define USE_SLEEP       0         // 0 = wait in loop (USB testing); 1 = deep shutdown between readings

#if USE_SLEEP
#include <STM32LowPower.h>
#endif

HardwareSerial RS485(RS485_RX, RS485_TX);   // USART1
HardwareSerial Modem(GSM_RX, GSM_TX);       // USART2
ModbusMaster node;

float temperature = 0, humidity = 0;

// ---------- RS485 direction control ----------
void preTransmission()  { digitalWrite(RS485_FC, HIGH); delay(2); }
void postTransmission() { delay(2); digitalWrite(RS485_FC, LOW); }

// ---------- Modem helpers ----------
// Sends an AT command and waits until "expect" appears or the timeout passes.
bool at(const char* cmd, const char* expect = "OK", uint32_t timeoutMs = 2000, String* reply = nullptr) {
  while (Modem.available()) Modem.read();
  if (cmd && cmd[0]) {
    Serial.print(">> "); Serial.println(cmd);
    Modem.println(cmd);
  }
  String buf;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (Modem.available()) buf += (char)Modem.read();
    if (buf.indexOf(expect) >= 0) {
      Serial.print("<< "); Serial.println(buf);
      if (reply) *reply = buf;
      return true;
    }
  }
  Serial.print("<< (no '"); Serial.print(expect); Serial.print("') "); Serial.println(buf);
  if (reply) *reply = buf;
  return false;
}

void modemPowerOn() {
  digitalWrite(GSM_POWER, GSM_ON_LEVEL);
  delay(3000);
}

void modemPowerOff() {
  digitalWrite(GSM_POWER, !GSM_ON_LEVEL);
}

bool waitForNetwork(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    String r;
    // Registered (home = ,1 or roaming = ,5) on LTE (CEREG) or 2G (CGREG)
    at("AT+CEREG?", "OK", 1000, &r);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) return true;
    at("AT+CGREG?", "OK", 1000, &r);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) return true;
    delay(2000);
  }
  return false;
}

bool modemConnect() {
  bool alive = false;
  for (int i = 0; i < 10 && !alive; i++) alive = at("AT", "OK", 1000);
  if (!alive) { Serial.println("Modem does not answer AT (check GSM_ON_LEVEL)"); return false; }

  at("ATE0");                       // echo off
  at("AT+CFUN=1");
  at("AT+CNMP=2");                  // automatic network mode (LTE Cat-M/NB, with 2G fallback)

  char cmd[160];
  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
  at(cmd);

  Serial.println("Waiting for network...");
  if (!waitForNetwork(90000UL)) { Serial.println("No network registration"); return false; }

  snprintf(cmd, sizeof(cmd), "AT+CNCFG=0,1,\"%s\"", APN);
  at(cmd);
  if (!at("AT+CNACT=0,1", "ACTIVE", 20000UL)) {
    // Some firmware reports only OK; confirm with a status query
    if (!at("AT+CNACT?", "+CNACT: 0,1", 3000)) { Serial.println("Data connection not active"); return false; }
  }
  return true;
}

bool mqttPublish(const char* payload) {
  char cmd[160];
  snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"URL\",\"%s\",\"%s\"", TB_SERVER, TB_PORT);
  at(cmd);
  snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"USERNAME\",\"%s\"", TB_TOKEN);   // ThingsBoard: token as username
  at(cmd);
  at("AT+SMCONF=\"PASSWORD\",\"\"");
  at("AT+SMCONF=\"CLIENTID\",\"ec-m12\"");
  at("AT+SMCONF=\"KEEPTIME\",60");
  at("AT+SMCONF=\"CLEANSS\",1");
  at("AT+SMCONF=\"QOS\",1");

  if (!at("AT+SMCONN", "OK", 20000UL)) { Serial.println("MQTT connect failed"); return false; }

  snprintf(cmd, sizeof(cmd), "AT+SMPUB=\"v1/devices/me/telemetry\",%u,1,0", (unsigned)strlen(payload));
  bool ok = false;
  if (at(cmd, ">", 5000)) {
    Modem.print(payload);
    ok = at("", "OK", 10000UL);
  }
  at("AT+SMDISC");
  return ok;
}

// ---------- Sensor ----------
bool readSensor() {
  for (int attempt = 0; attempt < 3; attempt++) {
    uint8_t r = node.readInputRegisters(0x0001, 2);
    if (r == node.ku8MBSuccess) {
      temperature = (int16_t)node.getResponseBuffer(0) / SENSOR_SCALE;   // signed
      humidity    = node.getResponseBuffer(1) / SENSOR_SCALE;
      Serial.print("Sensor: "); Serial.print(temperature, 2);
      Serial.print(" C, ");     Serial.print(humidity, 2); Serial.println(" %RH");
      return true;
    }
    Serial.print("Modbus read failed, error 0x"); Serial.println(r, HEX);   // 0xE2 = timeout
    delay(500);
  }
  return false;
}

// Formats a value with 2 decimals without float printf, e.g. 32.75 -> "32.75", -3.5 -> "-3.50"
void fmt2(char* out, size_t n, float v) {
  long h = lroundf(v * 100.0f);
  const char* sign = h < 0 ? "-" : "";
  if (h < 0) h = -h;
  snprintf(out, n, "%s%ld.%02ld", sign, h / 100, h % 100);
}

void cycle() {
  digitalWrite(BOOST_EN, HIGH);          // power the sensor
  delay(SENSOR_WARMUP);

  bool ok = readSensor();
#if !SENSOR_ALWAYS_ON
  digitalWrite(BOOST_EN, LOW);           // sensor off to save battery
#endif
  if (!ok) { Serial.println("No sensor reading, nothing sent"); return; }

  char t[16], h[16], payload[64];
  fmt2(t, sizeof(t), temperature);
  fmt2(h, sizeof(h), humidity);
  snprintf(payload, sizeof(payload), "{\"temperature\":%s,\"humidity\":%s}", t, h);
  Serial.print("Payload: "); Serial.println(payload);

  modemPowerOn();
  if (modemConnect() && mqttPublish(payload)) Serial.println("Published OK");
  else                                        Serial.println("Publish FAILED");
  modemPowerOff();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("EC-M12 + XY-MD02 -> ThingsBoard");

  pinMode(BOOST_EN, OUTPUT);   digitalWrite(BOOST_EN, LOW);
  pinMode(RS485_FC, OUTPUT);   digitalWrite(RS485_FC, LOW);   // receive mode
  pinMode(GSM_POWER, OUTPUT);  digitalWrite(GSM_POWER, !GSM_ON_LEVEL);

  RS485.begin(SENSOR_BAUD);
  Modem.begin(9600);
  node.begin(SENSOR_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

#if USE_SLEEP
  LowPower.begin();
#endif
}

void loop() {
  cycle();
#if USE_SLEEP
  // Deep shutdown: the MCU restarts from setup() when the timer ends (USB serial goes away)
  LowPower.shutdown(SEND_PERIOD_MS);
#else
  delay(SEND_PERIOD_MS);
#endif
}
