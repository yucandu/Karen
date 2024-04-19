#include <WiFi.h>
#include <SimplePgSQL.h>
#include "time.h"
#include <ESP32Time.h>
ESP32Time rtc(0);  // offset in seconds, use 0 because NTP already offset
#include <Adafruit_ADS1X15.h>
Adafruit_ADS1115 ads;
int16_t adc0, adc1, adc2, adc3;
float volts0, volts1, volts2, volts3;

#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 2
#define LED_BUILTIN 8

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

RTC_DATA_ATTR int readingCnt = -1;

typedef struct {
  float temp;
  unsigned long   time;
  float volts;
} sensorReadings;

#define maximumReadings 60 // The maximum number of readings that can be stored in the available space
#define sleeptimeSecs   60 // Every 10-mins of sleep 10 x 60-secs
#define WIFI_TIMEOUT 10000

RTC_DATA_ATTR sensorReadings Readings[maximumReadings];

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -18000;  //Replace with your GMT offset (secs)
const int daylightOffset_sec = 3600;   //Replace with your daylight offset (secs)
int hours, mins, secs;
float tempC;
bool sent = false;

IPAddress PGIP(192,168,50,197);        // your PostgreSQL server IP

const char ssid[] = "mikesnet";      //  your network SSID (name)
const char pass[] = "springchicken";      // your network password

const char user[] = "test";       // your database user
const char password[] = "test";   // your database password
const char dbname[] = "blynk_reporting";         // your database name


int WiFiStatus;
WiFiClient client;


char buffer[1024];
PGconnection conn(&client, 0, 1024, buffer);

char tosend[128];
String tosendstr;


#ifndef USE_ARDUINO_ETHERNET
void checkConnection()
{
    int status = WiFi.status();
    if (status != WL_CONNECTED) {
        if (WiFiStatus == WL_CONNECTED) {
            Serial.println("Connection lost");
            WiFiStatus = status;
        }
    }
    else {
        if (WiFiStatus != WL_CONNECTED) {
            Serial.println("Connected");
            WiFiStatus = status;
        }
    }
}

#endif

static PROGMEM const char query_rel[] = "\
SELECT a.attname \"Column\",\
  pg_catalog.format_type(a.atttypid, a.atttypmod) \"Type\",\
  case when a.attnotnull then 'not null ' else 'null' end as \"null\",\
  (SELECT substring(pg_catalog.pg_get_expr(d.adbin, d.adrelid) for 128)\
   FROM pg_catalog.pg_attrdef d\
   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef) \"Extras\"\
 FROM pg_catalog.pg_attribute a, pg_catalog.pg_class c\
 WHERE a.attrelid = c.oid AND c.relkind = 'r' AND\
 c.relname = %s AND\
 pg_catalog.pg_table_is_visible(c.oid)\
 AND a.attnum > 0 AND NOT a.attisdropped\
    ORDER BY a.attnum";

static PROGMEM const char query_tables[] = "\
SELECT n.nspname as \"Schema\",\
  c.relname as \"Name\",\
  CASE c.relkind WHEN 'r' THEN 'table' WHEN 'v' THEN 'view' WHEN 'm' THEN 'materialized view' WHEN 'i' THEN 'index' WHEN 'S' THEN 'sequence' WHEN 's' THEN 'special' WHEN 'f' THEN 'foreign table' END as \"Type\",\
  pg_catalog.pg_get_userbyid(c.relowner) as \"Owner\"\
 FROM pg_catalog.pg_class c\
     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\
 WHERE c.relkind IN ('r','v','m','S','f','')\
      AND n.nspname <> 'pg_catalog'\
      AND n.nspname <> 'information_schema'\
      AND n.nspname !~ '^pg_toast'\
  AND pg_catalog.pg_table_is_visible(c.oid)\
 ORDER BY 1,2";

int pg_status = 0;

void doPg(void)
{
    char *msg;
    int rc;
    if (!pg_status) {
        conn.setDbLogin(PGIP,
            user,
            password,
            dbname,
            "utf8");
        pg_status = 1;
        return;
    }

    if (pg_status == 1) {
        rc = conn.status();
        if (rc == CONNECTION_BAD || rc == CONNECTION_NEEDED) {
            char *c=conn.getMessage();
            if (c) Serial.println(c);
            pg_status = -1;
        }
        else if (rc == CONNECTION_OK) {
            pg_status = 2;
            Serial.println("Enter query");
        }
        return;
    }
    if (pg_status == 2) {
        if (!Serial.available()) return;
        char inbuf[128];
        int n = Serial.readBytesUntil('\n',inbuf,127);
        while (n > 0) {
            if (isspace(inbuf[n-1])) n--;
            else break;
        }
        inbuf[n] = 0;

        if (!strcmp(inbuf,"\\d")) {
            if (conn.execute(query_tables, true)) goto error;
            Serial.println("Working...");
            pg_status = 3;
            return;
        }
        if (!strncmp(inbuf,"\\d",2) && isspace(inbuf[2])) {
            char *c=inbuf+3;
            while (*c && isspace(*c)) c++;
            if (!*c) {
                if (conn.execute(query_tables, true)) goto error;
                Serial.println("Working...");
                pg_status = 3;
                return;
            }
            if (conn.executeFormat(true, query_rel, c)) goto error;
            Serial.println("Working...");
            pg_status = 3;
            return;
        }

        if (!strncmp(inbuf,"exit",4)) {
            conn.close();
            Serial.println("Thank you");
            pg_status = -1;
            return;
        }
        if (conn.execute(inbuf)) goto error;
        Serial.println("Working...");
        pg_status = 3;
    }
    if (pg_status == 3) {
        rc=conn.getData();
        int i;
        if (rc < 0) goto error;
        if (!rc) return;
        if (rc & PG_RSTAT_HAVE_COLUMNS) {
            for (i=0; i < conn.nfields(); i++) {
                if (i) Serial.print(" | ");
                Serial.print(conn.getColumn(i));
            }
            Serial.println("\n==========");
        }
        else if (rc & PG_RSTAT_HAVE_ROW) {
            for (i=0; i < conn.nfields(); i++) {
                if (i) Serial.print(" | ");
                msg = conn.getValue(i);
                if (!msg) msg=(char *)"NULL";
                Serial.print(msg);
            }
            Serial.println();
        }
        else if (rc & PG_RSTAT_HAVE_SUMMARY) {
            Serial.print("Rows affected: ");
            Serial.println(conn.ntuples());
        }
        else if (rc & PG_RSTAT_HAVE_MESSAGE) {
            msg = conn.getMessage();
            if (msg) Serial.println(msg);
        }
        if (rc & PG_RSTAT_READY) {
            pg_status = 2;
            Serial.println("Enter query");
        }
    }
    return;
error:
    msg = conn.getMessage();
    if (msg) Serial.println(msg);
    else Serial.println("UNKNOWN ERROR");
    if (conn.status() == CONNECTION_BAD) {
        Serial.println("Connection is bad");
        pg_status = -1;
    }
}

int i;

void gotosleep() {
      WiFi.disconnect();
      delay(1);
      esp_sleep_enable_timer_wakeup(sleeptimeSecs * 1000000);
      esp_deep_sleep_start();
      delay(1000);
}


void setup(void)
{
  setCpuFrequencyMhz(80);
  ads.setGain(GAIN_ONE);  // 1x gain   +/- 4.096V  1 bit = 2mV      0.125mV
  ads.begin();

  Serial.begin(115200);

  if ((readingCnt == -1)) {
      WiFi.begin((char *)ssid, pass);
      while (WiFi.status() != WL_CONNECTED && millis() < WIFI_TIMEOUT) {
        delay(250);
      }
      if (WiFi.status() == WL_CONNECTED) {
          configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
          
          struct tm timeinfo;
          getLocalTime(&timeinfo);
          rtc.setTimeStruct(timeinfo);
          WiFi.disconnect(); 
          readingCnt++;
          delay(1);
          esp_sleep_enable_timer_wakeup(1 * 1000000);
          esp_deep_sleep_start();
          delay(1000);
      }
  }

  sensors.begin();  
  sensors.requestTemperatures(); 
  tempC = sensors.getTempCByIndex(0);
  adc0 = ads.readADC_SingleEnded(0);
  volts0 = ads.computeVolts(adc0)*2.0;
  Readings[readingCnt].temp = tempC;       // Units °C
  Readings[readingCnt].time = rtc.getEpoch(); 
  Readings[readingCnt].volts = volts0;
  readingCnt++;

  if (readingCnt >= maximumReadings) {
      readingCnt = maximumReadings; 
      WiFi.begin((char *)ssid, pass);

      while (WiFi.status() != WL_CONNECTED && millis() < WIFI_TIMEOUT) {
        delay(500);
      }
      if (WiFi.status() != WL_CONNECTED && millis() >= WIFI_TIMEOUT) {
        for (int i = 0; i < maximumReadings; i++) {
          Readings[i].temp = Readings[i+1].temp;
          Readings[i].time = Readings[i+1].time;
          Readings[i].volts = Readings[i+1].volts;
        }
          Readings[maximumReadings].temp = tempC;       // Units °C
          Readings[maximumReadings].time = rtc.getEpoch(); 
          Readings[maximumReadings].volts = volts0;
        gotosleep();
      }

      while (i<maximumReadings) {
        checkConnection();
        doPg();
        if ((pg_status == 2) && (i<maximumReadings)){
          tosendstr = "insert into burst values (24,1," + String(Readings[i].time) + "," + String(Readings[i].temp,3) + "), (24,2," + String(Readings[i].time) + "," + String(Readings[i].volts,4) + ")";
          conn.execute(tosendstr.c_str());
          pg_status = 3;
          delay(50);
          i++;
        }
        delay(50);
      }

      readingCnt = -1;
      gotosleep();

  } 

    if (readingCnt < maximumReadings) {
        gotosleep();
    }

}


void loop()
{

    checkConnection();
doPg();
/*if ((pg_status == 2) && (i<maximumReadings)){
    tosendstr = "insert into burst values(24,1," + String(Readings[i].time) + "," + String(Readings[i].temp) + ")";
    conn.execute(tosendstr.c_str());
    pg_status = 3;
    delay(50);
    i++;
  }*/
delay(50);
}
