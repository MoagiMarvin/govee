#include <WiFi.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>

// ---- Config ----
const char* AP_SSID = "Clinic-Feedback";   // what shows up in phone's WiFi list
// Change these credentials before flashing the device.
const char* MANAGER_USER = "marvin";
const char* MANAGER_PASSWORD = "123456";
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
const size_t MAX_MESSAGE_LENGTH = 2000;
const size_t MAX_IMAGE_LENGTH = 150000;

DNSServer dnsServer;
AsyncWebServer server(80);
String managerSessionToken;
uint32_t managerSessionExpiresAt = 0;

struct ImageUpload {
  File file;
  String path;
  size_t size = 0;
  bool failed = false;
};

String imageExtension(const String& filename) {
  const int dot = filename.lastIndexOf('.');
  if (dot < 0) return "";
  String extension = filename.substring(dot);
  extension.toLowerCase();
  if (extension == ".jpg" || extension == ".jpeg" || extension == ".png" ||
      extension == ".gif" || extension == ".webp") {
    return extension;
  }
  return "";
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length());
  const char hex[] = "0123456789abcdef";

  for (size_t i = 0; i < value.length(); i++) {
    const uint8_t character = static_cast<uint8_t>(value[i]);
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(character);
    } else if (character == '\b') {
      escaped += "\\b";
    } else if (character == '\f') {
      escaped += "\\f";
    } else if (character == '\n') {
      escaped += "\\n";
    } else if (character == '\r') {
      escaped += "\\r";
    } else if (character == '\t') {
      escaped += "\\t";
    } else if (character < 0x20) {
      escaped += "\\u00";
      escaped += hex[character >> 4];
      escaped += hex[character & 0x0f];
    } else {
      escaped += static_cast<char>(character);
    }
  }

  return escaped;
}

void setup() {
  Serial.begin(115200);

  // Start LittleFS (your website files + saved submissions live here)
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  // Start WiFi Access Point (open network, no password, no internet)
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  Serial.print("AP address: ");
  Serial.println(WiFi.softAPIP());

  // DNS: redirect ALL domain lookups to our own IP -> triggers captive portal popup
  dnsServer.start(DNS_PORT, "*", apIP);

  server.addMiddleware([](AsyncWebServerRequest *request, ArMiddlewareNext next) {
    const AsyncWebHeader *userAgent = request->getHeader("User-Agent");
    Serial.printf("HTTP %s host=%s path=%s ua=%s\n",
      request->methodToString(), request->host().c_str(), request->url().c_str(),
      userAgent ? userAgent->value().c_str() : "-");
    next();
  });

  // Serve the website files from LittleFS
  auto sendKioskPage = [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response =
      request->beginResponse(LittleFS, "/index.html", "text/html; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  };
  server.on("/", HTTP_GET, sendKioskPage);
  server.on("/", HTTP_HEAD, sendKioskPage);

  auto requireManager = [](AsyncWebServerRequest *request) {
    bool authenticated = false;
    if (request->hasHeader("Cookie")) {
      const String cookies = request->getHeader("Cookie")->value();
      const String sessionCookie = "manager_session=" + managerSessionToken;
      const int cookieStart = cookies.indexOf(sessionCookie);
      const int cookieEnd = cookieStart < 0 ? -1 : cookieStart + sessionCookie.length();
      authenticated = !managerSessionToken.isEmpty() &&
      static_cast<int32_t>(managerSessionExpiresAt - millis()) > 0 &&
      cookieStart >= 0 && (cookieEnd == cookies.length() || cookies[cookieEnd] == ';');
    }
    if (authenticated) return true;

    AsyncWebServerResponse *response = request->beginResponse(401, "text/plain", "Manager login required");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    return false;
  };
  server.on("/manager", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response =
      request->beginResponse(LittleFS, "/manager.html", "text/html");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.on("/manager-login", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("username", true) || !request->hasParam("password", true) ||
        request->getParam("username", true)->value() != MANAGER_USER ||
        request->getParam("password", true)->value() != MANAGER_PASSWORD) {
      request->send(401, "text/plain", "Incorrect username or password");
      return;
    }

    char token[33];
    snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
      static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
      static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    managerSessionToken = token;
    managerSessionExpiresAt = millis() + 8UL * 60UL * 60UL * 1000UL;
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Logged in");
    response->addHeader("Set-Cookie",
      "manager_session=" + managerSessionToken + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.on("/manager-logout", HTTP_POST, [requireManager](AsyncWebServerRequest *request) {
    if (!requireManager(request)) return;
    managerSessionToken = "";
    managerSessionExpiresAt = 0;
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Logged out");
    response->addHeader("Set-Cookie", "manager_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.on("/api/feedback", HTTP_GET, [requireManager](AsyncWebServerRequest *request) {
    if (!requireManager(request)) return;
    if (!LittleFS.exists("/records.jsonl")) {
      request->send(200, "application/x-ndjson", "");
      return;
    }
    AsyncWebServerResponse *response =
      request->beginResponse(LittleFS, "/records.jsonl", "application/x-ndjson");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.on("/manager-image", HTTP_GET, [requireManager](AsyncWebServerRequest *request) {
    if (!requireManager(request)) return;
    if (!request->hasParam("path")) {
      request->send(400, "text/plain", "Missing image path");
      return;
    }

    const String path = request->getParam("path")->value();
    if (!path.startsWith("/images/") || path.indexOf("..") >= 0 ||
        !LittleFS.exists(path)) {
      request->send(404, "text/plain", "Image not found");
      return;
    }

    const String extension = imageExtension(path);
    const char* contentType = extension == ".png" ? "image/png" :
      extension == ".gif" ? "image/gif" :
      extension == ".webp" ? "image/webp" : "image/jpeg";
    request->send(LittleFS, path, contentType);
  });

  server.on("/upload-image", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      ImageUpload *upload = static_cast<ImageUpload *>(request->_tempObject);
      request->_tempObject = nullptr;
      if (upload == nullptr || upload->failed || upload->size == 0) {
        if (upload != nullptr) {
          if (upload->file) upload->file.close();
          if (!upload->path.isEmpty()) LittleFS.remove(upload->path);
          delete upload;
        }
        request->send(400, "text/plain", "Could not save image. Use a JPG, PNG, GIF, or WebP under 150 KB.");
        return;
      }

      if (upload->file) upload->file.close();
      const String response = String("{\"path\":\"") + upload->path + "\"}";
      delete upload;
      request->send(200, "application/json", response);
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (filename.isEmpty()) return;

      ImageUpload *upload = static_cast<ImageUpload *>(request->_tempObject);
      if (index == 0) {
        upload = new ImageUpload();
        request->_tempObject = upload;
        const String extension = imageExtension(filename);
        if (extension.isEmpty()) {
          upload->failed = true;
        } else {
          if (!LittleFS.exists("/images") && !LittleFS.mkdir("/images")) {
            upload->failed = true;
          } else {
            upload->path = "/images/feedback-" + String(micros()) + extension;
            upload->file = LittleFS.open(upload->path, "w");
            if (!upload->file) upload->failed = true;
          }
        }
      }

      if (upload == nullptr || upload->failed) return;
      if (len > 0) {
        if (upload->size + len > MAX_IMAGE_LENGTH || upload->file.write(data, len) != len) {
          upload->failed = true;
          if (upload->file) upload->file.close();
          LittleFS.remove(upload->path);
          return;
        }
        upload->size += len;
      }
      if (final && upload->file) upload->file.close();
    }
  );

  server.on("/submit", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("audience", true) || !request->hasParam("message", true)) {
      request->send(400, "text/plain", "Missing feedback audience or message");
      return;
    }

    const String audience = request->getParam("audience", true)->value();
    const bool isPatient = audience == "patient";
    const bool isRatingForm = isPatient;
    if (!isRatingForm && (!request->hasParam("type", true) || !request->hasParam("topic", true))) {
      request->send(400, "text/plain", "Missing feedback type or topic");
      return;
    }
    const String type = isPatient ? "patient_review" : request->getParam("type", true)->value();
    const String topic = isPatient ? "visit_experience" : request->getParam("topic", true)->value();
    const String message = request->getParam("message", true)->value();
    String heading = request->hasParam("heading", true)
      ? request->getParam("heading", true)->value() : "";
    String reportAbout = request->hasParam("reportAbout", true)
      ? request->getParam("reportAbout", true)->value() : "";
    String rating = request->hasParam("rating", true)
      ? request->getParam("rating", true)->value() : "";
    String patientWaitingRating = request->hasParam("patientWaitingRating", true)
      ? request->getParam("patientWaitingRating", true)->value() : "";
    String patientStaffRating = request->hasParam("patientStaffRating", true)
      ? request->getParam("patientStaffRating", true)->value() : "";
    String patientCleanlinessRating = request->hasParam("patientCleanlinessRating", true)
      ? request->getParam("patientCleanlinessRating", true)->value() : "";
    String patientCommunicationRating = request->hasParam("patientCommunicationRating", true)
      ? request->getParam("patientCommunicationRating", true)->value() : "";
    String imagePath = request->hasParam("imagePath", true)
      ? request->getParam("imagePath", true)->value() : "";
    String submittedAt = request->hasParam("submittedAt", true)
      ? request->getParam("submittedAt", true)->value() : "";
    String trimmedMessage = message;
    trimmedMessage.trim();
    heading.trim();
    reportAbout.trim();

    bool validSubmittedAt = submittedAt.length() == 13;
    for (size_t i = 0; validSubmittedAt && i < submittedAt.length(); i++) {
      validSubmittedAt = submittedAt[i] >= '0' && submittedAt[i] <= '9';
    }
    if (validSubmittedAt) {
      const unsigned long long timestamp = strtoull(submittedAt.c_str(), nullptr, 10);
      validSubmittedAt = timestamp >= 946684800000ULL && timestamp <= 4102444800000ULL;
    }

    const bool isStaff = audience == "staff";
    const auto validOptionalRating = [](const String& value) {
      return value.isEmpty() || value == "1" || value == "2" ||
        value == "3" || value == "4" || value == "5";
    };
    const bool validType = isStaff
      ? (type == "complaint" || type == "suggestion")
      : (isPatient &&
         (type == "complaint" || type == "compliment" || type == "suggestion"));
    const bool validStaffTopic = topic == "schedule" || topic == "facilities" ||
      topic == "supplies" || topic == "staffing" || topic == "communication" ||
      topic == "safety" || topic == "other";
    const bool validRating = rating.isEmpty() || rating == "1" || rating == "2" ||
      rating == "3" || rating == "4" || rating == "5";

    if (!isStaff && !isPatient) {
      request->send(400, "text/plain", "Invalid feedback audience");
      return;
    }
    if (!isRatingForm && !validType) {
      request->send(400, "text/plain", "Invalid feedback type");
      return;
    }
    if (!isRatingForm && isStaff && !validStaffTopic) {
      request->send(400, "text/plain", "Invalid feedback topic");
      return;
    }
    if (!isRatingForm && topic == "other" && heading.isEmpty()) {
      request->send(400, "text/plain", "Please enter a heading for your topic");
      return;
    }
    if (heading.length() > 80 || reportAbout.length() > 80) {
      request->send(413, "text/plain", "A heading or location field is too long");
      return;
    }
    if (!isRatingForm && trimmedMessage.isEmpty()) {
      request->send(400, "text/plain", "Please enter a message");
      return;
    }
    if (message.length() > MAX_MESSAGE_LENGTH) {
      request->send(413, "text/plain", "Message is too long");
      return;
    }
    if (isRatingForm && trimmedMessage.isEmpty() &&
        patientWaitingRating.isEmpty() &&
        patientStaffRating.isEmpty() && patientCleanlinessRating.isEmpty() &&
        patientCommunicationRating.isEmpty()) {
      request->send(400, "text/plain", "Please add a rating or comment");
      return;
    }
    if ((isPatient && !validRating) ||
        (isStaff && !rating.isEmpty()) ||
        (isPatient && (!validOptionalRating(patientWaitingRating) ||
          !validOptionalRating(patientStaffRating) ||
          !validOptionalRating(patientCleanlinessRating) ||
          !validOptionalRating(patientCommunicationRating)))) {
      request->send(400, "text/plain", "Invalid experience rating");
      return;
    }
    if (!imagePath.isEmpty() &&
        (!isStaff || !imagePath.startsWith("/images/") || imagePath.indexOf("..") >= 0 ||
         !LittleFS.exists(imagePath))) {
      request->send(400, "text/plain", "Image attachment is invalid");
      return;
    }

    if (isStaff && topic != "other") {
      if (topic == "schedule") heading = "Scheduling and working hours";
      else if (topic == "facilities") heading = "Facilities";
      else if (topic == "supplies") heading = "Supplies and equipment";
      else if (topic == "staffing") heading = "Staffing and workload";
      else if (topic == "communication") heading = "Communication and teamwork";
      else if (topic == "safety") heading = "Workplace safety";
    }

    File file = LittleFS.open("/records.jsonl", "a");
    if (!file) {
      Serial.println("Could not open feedback records file");
      request->send(500, "text/plain", "Could not save feedback");
      return;
    }

    String record = String("{\"audience\":\"") + audience +
      "\",\"type\":\"" + type +
      "\",\"topic\":\"" + topic +
      "\",\"heading\":\"" + jsonEscape(heading) +
      "\",\"reportAbout\":\"" + jsonEscape(reportAbout) +
      "\",\"message\":\"" + jsonEscape(message) +
      "\",\"submittedAt\":" + (validSubmittedAt ? submittedAt : "null");
    if (isPatient) {
      record += ",\"rating\":";
      record += rating.isEmpty() ? "null" : rating;
    }
    if (isPatient) {
      record += ",\"ratings\":{\"waiting_time\":";
      record += patientWaitingRating.isEmpty() ? "null" : patientWaitingRating;
      record += ",\"staff_kindness\":";
      record += patientStaffRating.isEmpty() ? "null" : patientStaffRating;
      record += ",\"cleanliness\":";
      record += patientCleanlinessRating.isEmpty() ? "null" : patientCleanlinessRating;
      record += ",\"communication\":";
      record += patientCommunicationRating.isEmpty() ? "null" : patientCommunicationRating;
      record += "}";
    }
    if (!imagePath.isEmpty()) {
      record += ",\"image\":\"" + jsonEscape(imagePath) + "\"";
    }
    record += "}";

    const size_t written = file.println(record);
    file.close();
    if (written != record.length() + 2) {
      Serial.println("Could not write complete feedback record");
      request->send(500, "text/plain", "Could not save feedback");
      return;
    }

    request->send(200, "text/plain", "Feedback submitted");
  });

  server.on("/generate_204", HTTP_GET, sendKioskPage);
  server.on("/generate_204", HTTP_HEAD, sendKioskPage);
  server.on("/gen_204", HTTP_GET, sendKioskPage);
  server.on("/gen_204", HTTP_HEAD, sendKioskPage);
  server.on("/hotspot-detect.html", HTTP_GET, sendKioskPage);
  server.on("/hotspot-detect.html", HTTP_HEAD, sendKioskPage);
  server.on("/connecttest.txt", HTTP_GET, sendKioskPage);
  server.on("/connecttest.txt", HTTP_HEAD, sendKioskPage);
  server.on("/ncsi.txt", HTTP_GET, sendKioskPage);
  server.on("/ncsi.txt", HTTP_HEAD, sendKioskPage);
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);
  });

  // Captive portals request different connectivity-check paths on different phones.
  server.onNotFound(sendKioskPage);

  server.begin();
  Serial.println("Feedback box ready. Connect to WiFi: " + String(AP_SSID));
}

void loop() {
  dnsServer.processNextRequest();
}
