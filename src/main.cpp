#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <ESPNtpClient.h>
#include <TZ.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define MSG_BUFFER_SIZE 50
#define MAX_SPRAY_COUNT 2650
#define MQTT_RECONNECT_INTERVAL 5000
#define RELAY_ON_DURATION 500

#define LED_PIN 2  // D4 no Wemos D1 Mini

struct ScheduleConfig {
  bool enabled;
  int interval;
  String startTime;
  String endTime;
};

// Classe para gerenciar o LED
class LedManager {
  private:
    const int ledPin = LED_PIN;
    bool wifiConnected = false;
    bool ntpSynced = false;
    bool scheduleEnabled = false;
    unsigned long lastBlinkTime = 0;
    bool ledState = false;  // Estado atual do LED (LOW = aceso, HIGH = apagado)
  
  public:
    LedManager() {
      pinMode(ledPin, OUTPUT);
      digitalWrite(ledPin, HIGH);  // LED apagado inicialmente (ativo em LOW)
    }
  
    // Atualiza os estados do sistema
    void updateState(bool wifi, bool ntp, bool schedule) {
      wifiConnected = wifi;
      ntpSynced = ntp;
      scheduleEnabled = schedule;
    }
  
    // Gerencia o comportamento do LED
    void manage() {
      unsigned long currentTime = millis();
    
      if (scheduleEnabled) {
        digitalWrite(ledPin, LOW);  // LED aceso continuamente
        return;
      }
    
      if (!wifiConnected) {
        digitalWrite(ledPin, HIGH);  // LED apagado se não há Wi-Fi
        return;
      }
    
      // Determina o intervalo de piscar com base no estado do NTP
      unsigned long blinkInterval = (wifiConnected && ntpSynced) ? 1000 : 3000;
    
      if (currentTime - lastBlinkTime >= blinkInterval) {
        ledState = !ledState;
        digitalWrite(ledPin, ledState ? HIGH : LOW);
        lastBlinkTime = currentTime;
      }
    }
  };


class Utils {
public:
  static String loadFile(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) {
      Serial.println("Erro ao abrir arquivo: " + String(path));
      return "";
    }
    String content = file.readString();
    file.close();
    return content;
  }

  static bool saveFile(const char* path, const String& content) {
    File file = LittleFS.open(path, "w");
    if (!file) {
      Serial.println("Erro ao salvar arquivo: " + String(path));
      return false;
    }
    file.print(content);
    file.close();
    return true;
  }

  static void serveFile(ESP8266WebServer& server, const char* path) {
    String content = loadFile(path);
    if (content.isEmpty()) {
      server.send(404, "text/plain", "Página não encontrada");
    } else {
      server.send(200, "text/html", content);
    }
  }
};

class WiFiManager {
private:
  String ssid = "";
  String password = "";
  String hostname = "freshpulse";

  String generateHostname() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String lastFour = mac.substring(mac.length() - 4);
    String generatedHostname = "freshpulse" + lastFour;
    Serial.println("Hostname gerado com base no MAC: " + generatedHostname);
    return generatedHostname;
  }


public:
  void initConfig() {
    if (LittleFS.exists("/ssid.txt") && LittleFS.exists("/pass.txt")) {
      File ssidFile = LittleFS.open("/ssid.txt", "r");
      File passFile = LittleFS.open("/pass.txt", "r");
      if (ssidFile && passFile) {
        String tempSsid = ssidFile.readStringUntil('\n');
        String tempPass = passFile.readStringUntil('\n');
        tempSsid.trim();
        tempPass.trim();
        ssid = tempSsid;
        password = tempPass;
        Serial.println("Credenciais WiFi carregadas: SSID=" + ssid);
      } else {
        Serial.println("Erro ao carregar credenciais WiFi");
      }
      ssidFile.close();
      passFile.close();
    }

    String loadedHostname = "";
    if (LittleFS.exists("/hostname.txt")) {
      File hostnameFile = LittleFS.open("/hostname.txt", "r");
      if (hostnameFile) {
        loadedHostname = hostnameFile.readStringUntil('\n');
        loadedHostname.trim();
        hostnameFile.close();
        Serial.println("Hostname lido do arquivo: '" + loadedHostname + "'");
      } else {
        Serial.println("Erro ao abrir o arquivo /hostname.txt");
      }
    } else {
      Serial.println("Arquivo /hostname.txt não existe");
    }

    if (loadedHostname.isEmpty()) {
      Serial.println("Hostname vazio ou arquivo não encontrado. Gerando novo hostname...");
      hostname = generateHostname();
      if (Utils::saveFile("/hostname.txt", hostname + "\n")) {
        Serial.println("Novo hostname salvo em /hostname.txt: " + hostname);
      } else {
        Serial.println("Erro ao salvar o novo hostname em /hostname.txt");
      }
    } else {
      hostname = loadedHostname;
      Serial.println("Usando hostname carregado: " + hostname);
    }

    WiFi.hostname(hostname);
  }

  void connect() {
    WiFi.begin(ssid, password);
    Serial.println("Tentando conectar ao Wi-Fi salvo...");
    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
      Serial.println("Conectado com sucesso! IP: " + WiFi.localIP().toString() + ", Hostname: " + WiFi.hostname());
      if (MDNS.begin(hostname.c_str())) {
        Serial.println("mDNS iniciado com sucesso! Acesse via: " + hostname + ".local");
        MDNS.addService("http", "tcp", 80);
      } else {
        Serial.println("Falha ao iniciar o mDNS!");
      }
    } else {
      Serial.println("Falha na conexão. Voltando ao modo de configuração.");
    }
  }

  void startAP(ESP8266WebServer& server) {
    const char* apPassword = "freshpulse1234";
    WiFi.softAP("FreshPulse", apPassword);
    server.on("/", HTTP_GET, [&server]() { Utils::serveFile(server, "/wificonf.html"); });
    
    server.on("/save", HTTP_POST, [&server, this]() {
      String newSsid = server.arg("ssid");
      String newPassword = server.arg("pass");
      if (newSsid.length() < 1 || newPassword.length() < 8) {
        server.send(400, "text/plain", "SSID ou senha inválidos");
        return;
      }
      if (Utils::saveFile("/ssid.txt", newSsid + "\n") && Utils::saveFile("/pass.txt", newPassword + "\n")) {
        Serial.println("Credenciais salvas com sucesso.");
        String hostname = getHostname();
        String mDnsUrl = "http://" + hostname + ".local";
        String html = "<html><body style='background-color: #1a1a1a; color: #e0e0e0; text-align: center; padding: 20px; font-family: Arial, sans-serif;'><h1 style='font-size: 1.5em;'>Configuração Salva!</h1><p>Credenciais salvas para a rede: " + newSsid + "</p><p>O dispositivo está reiniciando. Conecte-se à rede Wi-Fi '" + newSsid + "' e aguardo 60 segundos para acessar:</p><p>mDNS: <a href='" + mDnsUrl + "' style='color: #4CAF50;'>" + mDnsUrl + "</a></p><p>Se a conexão falhar, volte a este endereço (192.168.4.1) para verificar o status em 60 segundos.</p></body></html>";
        server.sendHeader("Content-Type", "text/html; charset=UTF-8");
        server.send(200, "text/html", html);
        Serial.println("mDNS após reinício: " + mDnsUrl);
        Serial.println("Agendando reinício em 5 segundos...");
        WiFi.begin(newSsid, newPassword);
        unsigned long start = millis();
        bool connected = false;
        while (millis() - start < 15000) { // Aguarda até 15 segundos para conectar
          if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
          }
          delay(500);
          ESP.wdtFeed();
        }
        if (connected) {
          String hostname = getHostname();
          String redirectUrl = "http://" + hostname + ".local";
          String html = "<html><body style='background-color: #1a1a1a; color: #e0e0e0; text-align: center; padding: 20px; font-family: Arial, sans-serif;'><h1 style='font-size: 1.5em;'>Conexão Bem-Sucedida!</h1><p>Conectado à rede: " + newSsid + "</p><p>Redirecionando para <a href='" + redirectUrl + "' style='color: #4CAF50;'>" + redirectUrl + "</a> em 5 segundos...</p><meta http-equiv='refresh' content='5; URL=" + redirectUrl + "'></body></html>";
          server.send(200, "text/html", html);
          Serial.println("WiFi configurado. Reiniciando em 10 segundos...");
          start = millis();
          while (millis() - start < 10000) { ESP.wdtFeed(); }
          ESP.restart();
        } else {
          String redirectUrl = "/?error=failed";
          String html = "<html><body style='background-color: #1a1a1a; color: #e0e0e0; text-align: center; padding: 20px; font-family: Arial, sans-serif;'><h1 style='font-size: 1.5em;'>Falha na Conexão</h1><p>Não foi possível conectar à rede: " + newSsid + "</p><p>Redirecionando de volta para a configuração em 5 segundos...</p><meta http-equiv='refresh' content='5; URL=" + redirectUrl + "'></body></html>";
          server.send(200, "text/html", html);
          Serial.println("Falha ao conectar. Retornando à configuração Wi-Fi.");
        }
      } else {
        server.send(500, "text/plain", "Erro ao salvar credenciais");
      }
    });
    server.begin();
  }

  String getHostname() { return hostname; }
  void setHostname(String value) { 
    hostname = value; 
    WiFi.hostname(hostname);
    Utils::saveFile("/hostname.txt", hostname + "\n");
    MDNS.setHostname(hostname.c_str());
    MDNS.begin(hostname.c_str());
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS atualizado! Novo hostname: " + hostname + ".local");
  }

  void setWifiCredentials(String newSsid, String newPassword) {
    ssid = newSsid;
    password = newPassword;
    Utils::saveFile("/ssid.txt", ssid + "\n");
    Utils::saveFile("/pass.txt", password + "\n");
    WiFi.begin(ssid, password);
  }
};

class SprayController {
private:
  int spray = 0;
  String spray_timestamp = "";
  const int botaoPin = 5;
  const int relePin = 14;
  unsigned long botaoPressionadoTempo = 0;
  unsigned long relayStartTime = 0;
  unsigned long lastSaveTime = 0;
  bool botaoPressionado = false;
  bool relayActive = false;
  bool counterChanged = false;

public:
  void setup() {
    pinMode(botaoPin, INPUT_PULLUP);
    pinMode(relePin, OUTPUT);
    digitalWrite(relePin, LOW);
    loadCounter();
  }

  void shortPress() {
    if (spray <= 0) {
      Serial.println("Contador zerado. Não é possível acionar o relé.");
      return;
    }
    Serial.println("Contador não zerado. Acionando o relé...");
    digitalWrite(relePin, HIGH);
    relayActive = true;
    relayStartTime = millis();
    decrementCounter();
    counterChanged = true; // Forçar a atualização
    saveCounter(); // Salvar imediatamente
  }

  void longPress() {
    Serial.println("Executando função de pressionamento longo! Contador resetado!");
    resetCounter();
  }

  void checkButton() {
    if (digitalRead(botaoPin) == LOW) {
      if (!botaoPressionado) {
        botaoPressionado = true;
        botaoPressionadoTempo = millis();
        Serial.println("Botão pressionado...");
      }
    } else if (botaoPressionado) {
      botaoPressionado = false;
      unsigned long tempoPressionado = millis() - botaoPressionadoTempo;
      if (tempoPressionado >= 10000) {
        longPress();
      } else {
        shortPress();
      }
    }
  }

  void updateRelay() {
    if (relayActive && millis() - relayStartTime >= RELAY_ON_DURATION) {
      digitalWrite(relePin, LOW);
      Serial.println("Relé desativado!");
      spray_timestamp = NTP.getLastNTPSync() > 0 ? NTP.getTimeDateString() : "NTP não sincronizado";
      relayActive = false;
    }
  }

  void loadCounter() {
    File file = LittleFS.open("/contador.txt", "r");
    if (file) {
      spray = file.readStringUntil('\n').toInt();
      file.close();
    } else {
      spray = 0;
      Serial.println("Erro ao ler contador, usando 0");
    }
    counterChanged = false;
  }

  void saveCounter() {
    if (counterChanged) {
      if (Utils::saveFile("/contador.txt", String(spray) + "\n")) {
        Serial.println("Contador salvo: " + String(spray));
        counterChanged = false;
        lastSaveTime = millis();
      }
    }
  }

  void decrementCounter() {
    spray = max(0, spray - 1);
    counterChanged = true;
  }

  void resetCounter() {
    spray = MAX_SPRAY_COUNT;
    counterChanged = true;
    saveCounter();
  }

  int getSprayCount() {
    loadCounter(); // Recarrega o contador do arquivo para garantir que o valor esteja atualizado
    return spray;
  }

  String getSprayTimestamp() { return spray_timestamp; }
  bool isRelayActive() { return relayActive; }
};

class NTPManager {
  private:
    String ntpAtivado = "0";
    String ntpServidor = "pool.ntp.org";
    int tzSelecionado = 0;
    unsigned long lastNtpCheck = 0;
    unsigned long lastNtpSync = 0;  // Armazena o momento da última sincronização bem-sucedida
    bool firstSyncDone = false;     // Indica se a primeira sincronização foi bem-sucedida
  
    // Função para tentar sincronizar o NTP
    bool trySyncNTP() {
      if (!NTP.begin()) {
        Serial.println("Falha ao iniciar NTP!");
        return false;
      }
      unsigned long start = millis();
      while (!NTP.getLastNTPSync() && millis() - start < 5000) {
        delay(100);
        ESP.wdtFeed();
      }
      if (NTP.getLastNTPSync()) {
        Serial.println("NTP sincronizado: " + String(NTP.getTimeDateString()));
        lastNtpSync = millis();
        firstSyncDone = true;
        return true;
      } else {
        Serial.println("Falha ao sincronizar NTP. Verifique a conexão ou o servidor.");
        return false;
      }
    }
  
  public:
    void loadConfig() {
      if (!LittleFS.exists("/NTP.txt")) {
        saveConfig();
        return;
      }
      File file = LittleFS.open("/NTP.txt", "r");
      if (file) {
        String tempAtivado = file.readStringUntil('\n');
        String tempServer = file.readStringUntil('\n');
        String tempTz = file.readStringUntil('\n');
        tempAtivado.trim();
        tempServer.trim();
        tempTz.trim();
        ntpAtivado = tempAtivado;
        ntpServidor = tempServer;
        tzSelecionado = tempTz.toInt();
        file.close();
        Serial.println("Configurações NTP carregadas: Ativado=" + ntpAtivado + ", Servidor=" + ntpServidor + ", GMT Offset=" + String(tzSelecionado));
      } else {
        Serial.println("Erro ao carregar /NTP.txt");
      }
    }
  
    void saveConfig() {
      File file = LittleFS.open("/NTP.txt", "w");
      if (file) {
        file.println(ntpAtivado);
        file.println(ntpServidor);
        file.println(tzSelecionado);
        file.close();
        Serial.println("Configurações NTP salvas.");
      } else {
        Serial.println("Erro ao salvar /NTP.txt");
      }
    }
  
    void setupNTP() {
      const char* timeZones[25] = {
        "Etc/GMT+12", "Etc/GMT+11", "Etc/GMT+10", "Etc/GMT+9", "Etc/GMT+8",
        "Etc/GMT+7", "Etc/GMT+6", "Etc/GMT+5", "Etc/GMT+4", "Etc/GMT+3",
        "Etc/GMT+2", "Etc/GMT+1", "Etc/GMT+0",
        "Etc/GMT-1", "Etc/GMT-2", "Etc/GMT-3", "Etc/GMT-4", "Etc/GMT-5",
        "Etc/GMT-6", "Etc/GMT-7", "Etc/GMT-8", "Etc/GMT-9", "Etc/GMT-10",
        "Etc/GMT-11", "Etc/GMT-12"
      };
      int index = tzSelecionado + 12;
      if (index >= 0 && index < sizeof(timeZones) / sizeof(timeZones[0])) {
        NTP.setTimeZone(timeZones[index]);
        Serial.println("Fuso horário configurado: " + String(timeZones[index]));
      } else {
        NTP.setTimeZone("Etc/GMT+0");
        Serial.println("Offset inválido: " + String(tzSelecionado) + ", usando GMT+0");
      }
      NTP.setNtpServerName(ntpServidor.c_str());
      if (ntpAtivado == "1") {
        // Tenta a primeira sincronização na inicialização
        trySyncNTP();
      }
    }
  
    void checkNtpStatus() {
      if (ntpAtivado != "1") return;
  
      unsigned long now = millis();
      unsigned long interval;
  
      if (!firstSyncDone) {
        // Ainda não conseguiu a primeira sincronização: tenta a cada 30 segundos
        interval = 30000;  // 30 segundos
      } else {
        // Já conseguiu a primeira sincronização
        if (NTP.getLastNTPSync() > 0) {
          // Última sincronização foi bem-sucedida: espera 24 horas
          interval = 24UL * 60UL * 60UL * 1000UL;  // 24 horas em milissegundos
        } else {
          // Falhou na última tentativa após a primeira sincronização: tenta a cada 10 minutos
          interval = 10UL * 60UL * 1000UL;  // 10 minutos em milissegundos
        }
      }
  
      if (now - lastNtpCheck >= interval) {
        lastNtpCheck = now;
        if (NTP.getLastNTPSync() > 0) {
          Serial.println("NTP sincronizado: " + String(NTP.getTimeDateString()));
        } else {
          Serial.println("NTP não sincronizado! Tentando novamente...");
          trySyncNTP();
        }
      }
    }
  
    void handleStatus(ESP8266WebServer& server) {
      char json[100];
      if (ntpAtivado == "1") {
        snprintf(json, sizeof(json), "{\"connected\":%s,\"datetime\":\"%s\"}", NTP.getLastNTPSync() > 0 ? "true" : "false", NTP.getTimeDateString());
      } else {
        strcpy(json, "{\"connected\":false}");
      }
      server.send(200, "application/json", json);
    }
  
    String getNtpEnabled() { return ntpAtivado; }
    String getNtpServer() { return ntpServidor; }
    int getTimezone() { return tzSelecionado; }
    bool isNtpActive() { return ntpAtivado == "1" && NTP.getLastNTPSync() > 0; }
    void setNtpEnabled(String value) { 
      if (ntpAtivado != value) {
        ntpAtivado = value; 
        if (value == "1") {
          setupNTP();
        } else {
          NTP.stop();
          firstSyncDone = false;  // Reseta o estado ao desativar
          lastNtpSync = 0;
        }
        saveConfig();
      }
    }
    void setNtpServer(String value) { ntpServidor = value; }
    void setTimezone(int value) { 
      if (value >= -12 && value <= 12) tzSelecionado = value; 
      else Serial.println("Timezone inválido: " + String(value));
    }
  };

class MQTTManager {
private:
  bool enabled;
  String host;
  int port;
  String topic;
  String user;
  String password;
  const char* configFile = "/mqtt.json";
  
  WiFiClient espClient;
  PubSubClient client;
  char msg[MSG_BUFFER_SIZE];
  String topicContador;
  String topicNtpStatus;
  String topicUltimaAtivacao;
  String topicScheduleEnabled;
  String topicScheduleInterval;
  String topicScheduleStartTime;
  String topicScheduleEndTime;
  String topicSpraySet;
  String topicNtpSetEnabled;
  String topicScheduleSetEnabled;
  String topicScheduleSetInterval;
  String topicScheduleSetStartTime;
  String topicScheduleSetEndTime;
  ScheduleConfig* schedule;
  NTPManager* ntp;
  unsigned long lastReconnectAttempt = 0;
  int lastSprayCount = -1;
  String lastTimestamp = "";
  String hostnameSubtopic;
  bool sprayMqttEnabled = false;
  bool lastRelayState = false;

  void publish(const char* topic, const String& message) {
    if (enabled && client.connected()) {
      client.publish(topic, message.c_str());
      Serial.println("Publicado em " + String(topic) + ": " + message);
    } else {
      Serial.println("Falha ao publicar em " + String(topic) + ": MQTT não conectado");
    }
  }

  void publishInitialState(SprayController& sprayCtrl, NTPManager& ntp) {
    if (!client.connected()) {
      Serial.println("MQTT não conectado, abortando publicação inicial");
      return;
    }
  
    Serial.println("Publicando estado inicial no MQTT");
  
    // Publica o contador
    int currentCount = sprayCtrl.getSprayCount();
    publish(topicContador.c_str(), String(currentCount));
    lastSprayCount = currentCount;
  
    publish(topicNtpStatus.c_str(), ntp.isNtpActive() ? "true" : "false");
  
    // Publica a última ativação do spray
    String currentTimestamp = sprayCtrl.getSprayTimestamp();
    publish(topicUltimaAtivacao.c_str(), currentTimestamp);
    lastTimestamp = currentTimestamp;
  
    // Publica o status do agendamento
    publishScheduleStatus();
  
    // Publica o estado do spray via MQTT
    publish(topicSpraySet.c_str(), sprayMqttEnabled ? "true" : "false");
  }

public:
  MQTTManager() : client(espClient), enabled(false), host(""), port(1883), topic("FreshPulse/"), user(""), password("") {
    loadConfig();
  }

  void setSchedule(ScheduleConfig* sched) { schedule = sched; }
  void setNtp(NTPManager* n) { ntp = n; }
  void setHostnameSubtopic(String hostname) {
    hostnameSubtopic = hostname;
    updateTopics();
  }

  void loadConfig() {
    File file = LittleFS.open(configFile, "r");
    if (!file) {
      Serial.println("Arquivo de configuração MQTT não encontrado. Criando novo...");
      saveConfig();
      return;
    }

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
      Serial.println("Erro ao ler arquivo de configuração MQTT: " + String(error.c_str()));
      file.close();
      return;
    }

    enabled = doc["enabled"].as<bool>();
    host = doc["host"].as<String>();
    port = doc["port"].as<int>();
    topic = doc["topic"].as<String>();
    user = doc["user"].as<String>();
    password = doc["password"].as<String>();
    file.close();
    updateTopics();
  }

  void saveConfig() {
    File file = LittleFS.open(configFile, "w");
    if (!file) {
      Serial.println("Erro ao abrir arquivo de configuração MQTT para escrita");
      return;
    }

    DynamicJsonDocument doc(512);
    doc["enabled"] = enabled;
    doc["host"] = host;
    doc["port"] = port;
    doc["topic"] = topic;
    doc["user"] = user;
    doc["password"] = password;

    if (serializeJson(doc, file) == 0) {
      Serial.println("Erro ao escrever arquivo de configuração MQTT");
    }
    file.close();
  }

  void updateConfig(bool newEnabled, String newHost, int newPort, String newTopic, String newUser, String newPassword) {
    enabled = newEnabled;
    host = newHost;
    port = newPort;
    topic = newTopic;
    user = newUser;
    password = newPassword;
    saveConfig();
    updateTopics();
  }

  void replaceMQTTPlaceholders(String& html) {
    html.replace("{{MQTT_ATIVO}}", enabled ? "checked" : "");
    html.replace("{{MQTT_HOST}}", host);
    html.replace("{{MQTT_PORTA}}", String(port));
    html.replace("{{MQTT_TOPICO}}", topic);
    html.replace("{{MQTT_USUARIO}}", user);
    html.replace("{{MQTT_SENHA}}", password);
  }

  void handleSaveMQTT(ESP8266WebServer& server) {
    bool newEnabled = server.arg("mqtt_ativo") == "on";
    String newHost = server.arg("mqtt_host");
    int newPort = server.arg("mqtt_porta").toInt();
    String newTopic = server.arg("mqtt_topico");
    String newUser = server.arg("mqtt_usuario");
    String newPassword = server.arg("mqtt_senha");

    updateConfig(newEnabled, newHost, newPort, newTopic, newUser, newPassword);
    String response = "Configuração MQTT salva com sucesso! Reiniciando em 10 segundos...";
    server.send(200, "text/plain", response);
    unsigned long restartStart = millis();
    while (millis() - restartStart < 10000) { ESP.wdtFeed(); }
    ESP.restart();
  }

  void handleMqttStatus(ESP8266WebServer& server) {
    DynamicJsonDocument doc(512);
    doc["enabled"] = enabled;
    doc["connected"] = enabled && client.connected(); // Verifica se está habilitado E conectado
    doc["host"] = host;
    doc["topic"] = topic;
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  }

  void setupMQTT(SprayController& sprayCtrl, NTPManager& ntpMgr, ESP8266WebServer& server) {
    if (!enabled) return;

    ntp = &ntpMgr;
    client.setServer(host.c_str(), port);
    client.setKeepAlive(10);
    client.setCallback([this, &sprayCtrl, &server](char* topic, byte* payload, unsigned int length) {
      callback(topic, payload, length, sprayCtrl, server);
    });

    int attempts = 0;
    while (!client.connected() && attempts < 5) {
      if (client.connect("ESP8266Client", user.c_str(), password.c_str())) {
        Serial.println("MQTT conectado com sucesso! Sub-tópico: " + hostnameSubtopic);
        client.subscribe(topicSpraySet.c_str());
        client.subscribe(topicNtpSetEnabled.c_str());
        client.subscribe(topicScheduleSetEnabled.c_str());
        client.subscribe(topicScheduleSetInterval.c_str());
        client.subscribe(topicScheduleSetStartTime.c_str());
        client.subscribe(topicScheduleSetEndTime.c_str());
        publishInitialState(sprayCtrl, ntpMgr);
      } else {
        Serial.println("Falha ao conectar ao MQTT, tentativa " + String(attempts + 1) + " de 5");
        delay(1000);
        attempts++;
      }
    }

    if (!client.connected()) {
      Serial.println("Falha ao conectar ao MQTT após 5 tentativas. Continuando sem MQTT.");
      enabled = false;
      saveConfig();
    }
  }

  bool reconnect() {
    if (client.connected()) return true;
    unsigned long now = millis();
    if (now - lastReconnectAttempt < MQTT_RECONNECT_INTERVAL) return false;
    lastReconnectAttempt = now;

    if (client.connect("ESP8266Client", user.c_str(), password.c_str())) {
      client.subscribe(topicSpraySet.c_str());
      client.subscribe(topicNtpSetEnabled.c_str());
      client.subscribe(topicScheduleSetEnabled.c_str());
      client.subscribe(topicScheduleSetInterval.c_str());
      client.subscribe(topicScheduleSetStartTime.c_str());
      client.subscribe(topicScheduleSetEndTime.c_str());
      Serial.println("MQTT reconectado com sucesso! Sub-tópico: " + hostnameSubtopic);
      return true;
    } else {
      Serial.println("Tentativa de reconexão ao MQTT falhou.");
      if ((millis() - lastReconnectAttempt) / MQTT_RECONNECT_INTERVAL >= 3) {
        enabled = false;
        saveConfig();
        //ESP.restart();
      }
      return false;
    }
  }

  void loop(SprayController& sprayCtrl) {
    if (!enabled) return;
    if (!client.connected()) reconnect();
    client.loop();
  
    static unsigned long lastPeriodicPublish = 0;
    static unsigned long lastEventPublish = 0;
    unsigned long now = millis();
  
    // Publicação periódica a cada 1 minuto (60000ms)
    if (now - lastPeriodicPublish >= 60000) {
      lastPeriodicPublish = now;
      Serial.println("Publicação periódica de todos os tópicos MQTT");
  
      // Publica o contador
      int currentCount = sprayCtrl.getSprayCount();
      publish(topicContador.c_str(), String(currentCount));
      lastSprayCount = currentCount;
  
      // Publica o status do NTP
      publish(topicNtpStatus.c_str(), ntp->isNtpActive() ? "true" : "false");
  
      // Publica a última ativação do spray
      String currentTimestamp = sprayCtrl.getSprayTimestamp();
      publish(topicUltimaAtivacao.c_str(), currentTimestamp);
      lastTimestamp = currentTimestamp;
  
      // Publica o status do agendamento
      publishScheduleStatus();
  
      // Publica o estado do spray via MQTT
      publish(topicSpraySet.c_str(), sprayMqttEnabled ? "true" : "false");
    }
  
    // Publicação imediata para eventos (a cada 500ms para evitar sobrecarga)
    if (now - lastEventPublish >= 500) {
      lastEventPublish = now;
  
      // Verifica mudanças no contador
      int currentCount = sprayCtrl.getSprayCount();
      if (currentCount != lastSprayCount) {
        publish(topicContador.c_str(), String(currentCount));
        lastSprayCount = currentCount;
      }
  
      // Verifica mudanças na última ativação
      String currentTimestamp = sprayCtrl.getSprayTimestamp();
      if (currentTimestamp != lastTimestamp) {
        publish(topicUltimaAtivacao.c_str(), currentTimestamp);
        lastTimestamp = currentTimestamp;
      }
  
      // Verifica mudanças no estado do spray via MQTT
      bool currentRelayState = sprayCtrl.isRelayActive();
      if (lastRelayState && !currentRelayState) {
        sprayMqttEnabled = false;
        publish(topicSpraySet.c_str(), "false");
        Serial.println("Controle do spray via MQTT desativado após ativação");
      }
      lastRelayState = currentRelayState;
    }
  }

  void callback(char* topic, byte* payload, unsigned int length, SprayController& sprayCtrl, ESP8266WebServer& server) {
    String message = String((char*)payload).substring(0, length);
    String vTopic = topic;
    Serial.println("Mensagem recebida em " + vTopic + ": " + message);

    if (vTopic == topicSpraySet) {
      bool newState = (message == "true");
      if (newState && !sprayMqttEnabled) {
        sprayMqttEnabled = true;
        sprayCtrl.shortPress();
        publish(topicSpraySet.c_str(), "true");
        Serial.println("Spray ativado via MQTT");
      } else if (!newState && sprayMqttEnabled) {
        sprayMqttEnabled = false;
        publish(topicSpraySet.c_str(), "false");
        Serial.println("Spray desativado via MQTT");
      }
    } else if (vTopic == topicNtpSetEnabled) {
      String newState = (message == "true") ? "1" : "0";
      if (newState != ntp->getNtpEnabled()) {
        publish(topicNtpStatus.c_str(), ntp->isNtpActive() ? "true" : "false");
        ntp->setNtpEnabled(newState);
      }
    } else if (vTopic == topicScheduleSetEnabled) {
      bool newEnabled = (message == "true");
      if (newEnabled && !ntp->isNtpActive()) {
        Serial.println("Erro: NTP não está ativo. Agendamento não pode ser ativado.");
        publish(topicScheduleEnabled.c_str(), "false");
      } else if (newEnabled != schedule->enabled) {
        schedule->enabled = newEnabled;
        publish(topicScheduleEnabled.c_str(), schedule->enabled ? "true" : "false");
        Serial.println("Agendamento " + String(schedule->enabled ? "ativado" : "desativado") + " via MQTT");
      }
    } else if (vTopic == topicScheduleSetInterval) {
      int newInterval = message.toInt();
      if (newInterval > 0 && newInterval != schedule->interval) {
        schedule->interval = newInterval;
        publish(topicScheduleInterval.c_str(), String(schedule->interval));
        Serial.println("Intervalo do agendamento atualizado para " + String(schedule->interval) + " via MQTT");
      }
    } else if (vTopic == topicScheduleSetStartTime) {
      if (message.length() == 5 && message.indexOf(':') == 2 && message != schedule->startTime) {
        schedule->startTime = message;
        publish(topicScheduleStartTime.c_str(), schedule->startTime);
        Serial.println("Hora de início do agendamento atualizada para " + schedule->startTime + " via MQTT");
      }
    } else if (vTopic == topicScheduleSetEndTime) {
      if (message.length() == 5 && message.indexOf(':') == 2 && message != schedule->endTime) {
        schedule->endTime = message;
        publish(topicScheduleEndTime.c_str(), schedule->endTime);
        Serial.println("Hora de fim do agendamento atualizada para " + schedule->endTime + " via MQTT");
      }
    }
  }

  void publishScheduleStatus() {
    publish(topicScheduleEnabled.c_str(), schedule->enabled ? "true" : "false");
    publish(topicScheduleInterval.c_str(), String(schedule->interval));
    publish(topicScheduleStartTime.c_str(), schedule->startTime);
    publish(topicScheduleEndTime.c_str(), schedule->endTime);
  }

private:
  void updateTopics() {
    topicContador = topic + hostnameSubtopic + "/contador";
    topicNtpStatus = topic + hostnameSubtopic + "/ntp/status";
    topicUltimaAtivacao = topic + hostnameSubtopic + "/ultima_ativacao_spray";
    topicScheduleEnabled = topic + hostnameSubtopic + "/schedule/enabled";
    topicScheduleInterval = topic + hostnameSubtopic + "/schedule/interval";
    topicScheduleStartTime = topic + hostnameSubtopic + "/schedule/startTime";
    topicScheduleEndTime = topic + hostnameSubtopic + "/schedule/endTime";
    topicSpraySet = topic + hostnameSubtopic + "/spray/set";
    topicNtpSetEnabled = topic + hostnameSubtopic + "/ntp/set/enabled";
    topicScheduleSetEnabled = topic + hostnameSubtopic + "/schedule/set/enabled";
    topicScheduleSetInterval = topic + hostnameSubtopic + "/schedule/set/interval";
    topicScheduleSetStartTime = topic + hostnameSubtopic + "/schedule/set/startTime";
    topicScheduleSetEndTime = topic + hostnameSubtopic + "/schedule/set/endTime";
  }
};

class ScheduleManager {
private:
  ScheduleConfig schedule = {false, 10, "00:00", "23:59"};
  const char* scheduleFile = "/agendamento.txt";
  unsigned long lastSprayTime = 0;
  NTPManager* ntp;
  MQTTManager* mqtt;

  int timeToMinutes(String time) {
    int hours = time.substring(0, 2).toInt();
    int minutes = time.substring(3, 5).toInt();
    return hours * 60 + minutes;
  }

public:
  void setNtp(NTPManager* n) { ntp = n; }
  void setMqtt(MQTTManager* m) { mqtt = m; }

  void load() {
    if (!LittleFS.exists(scheduleFile)) return;
    File file = LittleFS.open(scheduleFile, "r");
    if (file) {
      DynamicJsonDocument json(256);
      if (!deserializeJson(json, file)) {
        schedule.enabled = json["enabled"];
        schedule.interval = json["interval"];
        schedule.startTime = json["startTime"].as<String>();
        schedule.endTime = json["endTime"].as<String>();
      }
      file.close();
    } else {
      Serial.println("Erro ao carregar /agendamento.txt");
    }
  }

  void save() {
    DynamicJsonDocument json(256);
    json["enabled"] = schedule.enabled;
    json["interval"] = schedule.interval;
    json["startTime"] = schedule.startTime;
    json["endTime"] = schedule.endTime;
    String jsonStr;
    serializeJson(json, jsonStr);
    Utils::saveFile(scheduleFile, jsonStr);
    if (mqtt) {
      mqtt->publishScheduleStatus();
    }
  }

  void handleGet(ESP8266WebServer& server) {
    DynamicJsonDocument json(256);
    json["enabled"] = schedule.enabled;
    json["interval"] = schedule.interval;
    json["startTime"] = schedule.startTime;
    json["endTime"] = schedule.endTime;
    String response;
    serializeJson(json, response);
    server.send(200, "application/json", response);
  }

  void handleGetStatus(ESP8266WebServer& server) {
    DynamicJsonDocument json(256);
    json["enabled"] = schedule.enabled;
    if (schedule.enabled) {
      json["startTime"] = schedule.startTime;
      json["endTime"] = schedule.endTime;
    }
    String response;
    serializeJson(json, response);
    server.send(200, "application/json", response);
  }

  void handleSave(ESP8266WebServer& server) {
    if (server.hasArg("plain")) {
      DynamicJsonDocument json(256);
      if (!deserializeJson(json, server.arg("plain"))) {
        bool newEnabled = json["enabled"];
        if (newEnabled && !ntp->isNtpActive()) {
          server.send(400, "text/plain", "Erro: NTP não está ativo. Agendamento não pode ser ativado.");
          return;
        }
        String newStartTime = json["startTime"].as<String>();
        String newEndTime = json["endTime"].as<String>();
  
        // Validar formato dos horários (HH:MM)
        if (newStartTime.length() != 5 || newStartTime[2] != ':' ||
            newEndTime.length() != 5 || newEndTime[2] != ':') {
          server.send(400, "text/plain", "Erro: Formato de horário inválido. Use HH:MM.");
          return;
        }
  
        // Validar valores de horas e minutos
        int startHours = newStartTime.substring(0, 2).toInt();
        int startMinutes = newStartTime.substring(3, 5).toInt();
        int endHours = newEndTime.substring(0, 2).toInt();
        int endMinutes = newEndTime.substring(3, 5).toInt();
        if (startHours < 0 || startHours > 23 || startMinutes < 0 || startMinutes > 59 ||
            endHours < 0 || endHours > 23 || endMinutes < 0 || endMinutes > 59) {
          server.send(400, "text/plain", "Erro: Horas devem estar entre 00 e 23, e minutos entre 00 e 59.");
          return;
        }
  
        schedule.enabled = newEnabled;
        schedule.interval = json["interval"];
        schedule.startTime = newStartTime;
        schedule.endTime = newEndTime;
        save();
        server.send(200, "text/plain", "Agendamento salvo com sucesso!");
        return;
      }
    }
    server.send(400, "text/plain", "Erro ao salvar agendamento!");
  }

  bool isWithinSchedule() {
    if (!ntp->isNtpActive()) {
      Serial.println("NTP não está ativo. Agendamento não pode ser verificado.");
      return false;
    }
  
    // Obtém a string completa do NTP
    String ntpTimeString = NTP.getTimeDateString();
    Serial.println("String completa do NTP: " + ntpTimeString);
  
    // Tenta extrair o horário (HH:MM) em diferentes posições
    String currentTime;
    if (ntpTimeString.length() >= 8 && ntpTimeString.substring(2, 3) == "/") {
      // Formato "DD/MM/YYYY HH:MM:SS" (ex.: "27/03/2025 14:30:00")
      currentTime = ntpTimeString.substring(11, 16); // Pega "HH:MM" a partir da posição 11
    } else if (ntpTimeString.length() >= 8 && ntpTimeString.substring(2, 3) == ":") {
      // Formato "HH:MM:SS DD/MM/YYYY" (ex.: "14:30:00 27/03/2025")
      currentTime = ntpTimeString.substring(0, 5); // Pega "HH:MM" a partir da posição 0
    } else {
      Serial.println("Formato de horário NTP desconhecido: " + ntpTimeString);
      return false;
    }
  
    // Valida o formato de currentTime (deve ser HH:MM)
    if (currentTime.length() != 5 || currentTime[2] != ':') {
      Serial.println("Formato de horário inválido: " + currentTime);
      return false;
    }
  
    int currentMinutes = timeToMinutes(currentTime);
    int startMinutes = timeToMinutes(schedule.startTime);
    int endMinutes = timeToMinutes(schedule.endTime);
  
    Serial.println("Horário atual: " + currentTime + " (" + String(currentMinutes) + " minutos)");
    Serial.println("Horário de início: " + schedule.startTime + " (" + String(startMinutes) + " minutos)");
    Serial.println("Horário de fim: " + schedule.endTime + " (" + String(endMinutes) + " minutos)");
  
    bool withinSchedule;
    if (startMinutes <= endMinutes) {
      // Caso normal: início antes do fim (ex.: 08:00 às 17:00)
      withinSchedule = currentMinutes >= startMinutes && currentMinutes <= endMinutes;
    } else {
      // Caso que cruza a meia-noite: início depois do fim (ex.: 22:00 às 02:00)
      withinSchedule = currentMinutes >= startMinutes || currentMinutes <= endMinutes;
    }
  
    Serial.println("Dentro do horário? " + String(withinSchedule ? "Sim" : "Não"));
    return withinSchedule;
  }

  void manage(SprayController& sprayCtrl) {
    if (schedule.enabled && ntp->isNtpActive()) {
      unsigned long now = millis();
      //Serial.println("Tempo desde o último spray: " + String(now - lastSprayTime) + "ms (necessário: " + String(schedule.interval * 60000UL) + "ms)");
      if (now - lastSprayTime >= (schedule.interval * 60000UL)) {
        if (isWithinSchedule()) {
          sprayCtrl.shortPress();
          lastSprayTime = now;
          Serial.println("SPRAYYYY");
        } else {
          Serial.println("Fora do horário de agendamento. Spray não acionado.");
        }
      }
    }
  }

  ScheduleConfig& getSchedule() { return schedule; }
};

class FreshPulseServer {
  private:
    ESP8266WebServer server;
    WiFiManager wifi;
    SprayController sprayCtrl;
    NTPManager ntp;
    MQTTManager mqtt;
    ScheduleManager schedule;
    LedManager led;
    
  public:
  FreshPulseServer() : server(80) {}
  
    void setup() {
      Serial.begin(115200);
      if (!LittleFS.begin()) {
        Serial.println("Erro ao inicializar LittleFS");
        return;
      }
    
      sprayCtrl.setup();
      wifi.initConfig();
      wifi.connect();
    
      if (WiFi.waitForConnectResult() != WL_CONNECTED) {
        wifi.startAP(server);
      } else {
        setupRoutes();
        server.begin();
        Serial.println("Servidor iniciado.");
    
        // Carrega as configurações do agendamento antes de inicializar o MQTT
        schedule.load();
        schedule.setNtp(&ntp);
        schedule.setMqtt(&mqtt);
    
        // Carrega as configurações do MQTT e inicializa
        mqtt.loadConfig();
        mqtt.setSchedule(&schedule.getSchedule());
        mqtt.setNtp(&ntp);
        mqtt.setHostnameSubtopic(wifi.getHostname());
    
        // Carrega o contador e configura o NTP
        sprayCtrl.loadCounter();
        ntp.loadConfig();
        ntp.setupNTP();  // A sincronização inicial é gerenciada pelo NTPManager
    
        // Inicializa o MQTT e publica os valores iniciais
        mqtt.setupMQTT(sprayCtrl, ntp, server);
        // Publica os valores do agendamento imediatamente após a inicialização
        mqtt.publishScheduleStatus();
  
        // Inicializa o estado do LED
        led.updateState(WiFi.status() == WL_CONNECTED, ntp.isNtpActive(), schedule.getSchedule().enabled);
      }
    }
  
    void loop() {
      server.handleClient();
      mqtt.loop(sprayCtrl);
      schedule.manage(sprayCtrl);
      sprayCtrl.updateRelay();
      ntp.checkNtpStatus();  // A nova lógica de sincronização é chamada aqui
      sprayCtrl.checkButton();
      MDNS.update();
      ESP.wdtFeed();
  
      // Atualiza o estado do LED com base nas condições atuais
      led.updateState(WiFi.status() == WL_CONNECTED, ntp.isNtpActive(), schedule.getSchedule().enabled);
      led.manage();

    }
  
private:


// Função para formatar o tempo ligado (uptime)
String formatUptime() {
  unsigned long uptime = millis() / 1000;  // Tempo em segundos
  unsigned long days = uptime / 86400;
  uptime %= 86400;
  unsigned long hours = uptime / 3600;
  uptime %= 3600;
  unsigned long minutes = uptime / 60;
  unsigned long seconds = uptime % 60;
  return String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
}

// Nova rota para fornecer informações de hardware
void handleHardwareInfo() {
  DynamicJsonDocument doc(512);
  doc["chipModel"] = ESP.getChipId() ? String(ESP.getChipId()) : "ESP8266";
  doc["cpuFreq"] = ESP.getCpuFreqMHz();
  doc["uptime"] = formatUptime();
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

  void setupRoutes() {
    server.on("/", HTTP_GET, [this]() {
      String html = Utils::loadFile("/index.html");
      if (!html.isEmpty()) {
        html.replace("<span id=\"counterValue\">Carregando...</span>", String(sprayCtrl.getSprayCount()));
        server.send(200, "text/html", html);
      } else {
        server.send(404, "text/plain", "Página não encontrada");
      }
    });

    server.on("/scheduleStatus", HTTP_GET, [this]() { schedule.handleGetStatus(server); });

    server.on("/sprayButton", HTTP_GET, [this]() {
      Serial.println("Botão web pressionado...");
      sprayCtrl.shortPress();
      server.send(200, "text/plain", "Spray acionado!");
    });

    server.on("/getCounter", HTTP_GET, [this]() {
      DynamicJsonDocument json(100);
      json["counter"] = sprayCtrl.getSprayCount();
      String response;
      serializeJson(json, response);
      server.send(200, "application/json", response);
    });

    server.on("/agendamento.html", HTTP_GET, [this]() { Utils::serveFile(server, "/agendamento.html"); });

    server.on("/ntp.html", HTTP_GET, [this]() {
      String html = Utils::loadFile("/ntp.html");
      if (!html.isEmpty()) {
        ntp.loadConfig();
        html.replace("{{NTP_ENABLED}}", ntp.getNtpEnabled() == "1" ? "checked" : "");
        html.replace("{{NTP_SERVER}}", ntp.getNtpServer());
  
        // Gerar as opções do select com a opção correta marcada como 'selected'
        String gmtOptions = "";
        for (int i = -12; i <= 12; i++) {
          String selected = (i == ntp.getTimezone()) ? "selected" : "";
          gmtOptions += "<option value=\"" + String(i) + "\" " + selected + ">GMT" + (i >= 0 ? "+" : "") + String(i) + "</option>";
        }
        html.replace("{{GMT_OPTIONS}}", gmtOptions);
  
        server.send(200, "text/html", html);
      } else {
        server.send(404, "text/plain", "Página não encontrada");
      }
    });

    server.on("/wifi.html", HTTP_GET, [this]() {
      String html = Utils::loadFile("/wifi.html");
      if (!html.isEmpty()) {
        html.replace("{{HOSTNAME}}", wifi.getHostname());
        server.send(200, "text/html", html);
      } else {
        server.send(404, "text/plain", "Página não encontrada");
      }
    });

    server.on("/mqtt.html", HTTP_GET, [this]() {
      String html = Utils::loadFile("/mqtt.html");
      if (!html.isEmpty()) {
        mqtt.loadConfig(); // Carrega as configurações mais recentes
        mqtt.replaceMQTTPlaceholders(html); // Substitui os placeholders
        server.send(200, "text/html", html);
      } else {
        server.send(404, "text/plain", "Página não encontrada");
      }
    });

    server.on("/saveMQTT", HTTP_POST, [this]() {
      mqtt.handleSaveMQTT(server); // O método já inclui o reinício
    });

    server.on("/mqttStatus", HTTP_GET, [this]() { mqtt.handleMqttStatus(server); });

    server.on("/saveNTPConfig", HTTP_POST, [this]() {
      Serial.println("Requisição recebida em /saveNTPConfig");
      String newNtpEnabled = server.arg("ntpEnabled") == "on" ? "1" : "0";
      ntp.setNtpServer(server.arg("ntpServer"));
      ntp.setTimezone(server.arg("gmt_offset").toInt());
      ntp.saveConfig();
      unsigned long start = millis();
      while (millis() - start < 1000) { ESP.wdtFeed(); }
      ntp.setNtpEnabled(newNtpEnabled);
      String response = "Configuração NTP salva com sucesso! Reiniciando em 10 segundos...";
      Serial.println("Resposta enviada: " + response);
      server.send(200, "text/plain", response);
      unsigned long restartStart = millis();
      while (millis() - restartStart < 10000) { ESP.wdtFeed(); }
      ESP.restart();
    });

    server.on("/saveHostname", HTTP_POST, [this]() {
      String newHostname = server.arg("hostname");
      if (newHostname.length() > 0) {
        wifi.setHostname(newHostname);
        mqtt.setHostnameSubtopic(newHostname);
        server.send(200, "text/plain", "Hostname salvo com sucesso! Reiniciando em 10 segundos...");
        unsigned long start = millis();
        while (millis() - start < 10000) { ESP.wdtFeed(); }
        ESP.restart();
      } else {
        server.send(400, "text/plain", "Hostname inválido!");
      }
    });

    server.on("/saveWifiConfig", HTTP_POST, [this]() {
      String newSsid = server.arg("ssid");
      String newPassword = server.arg("pass");
      if (newSsid.length() < 1 || newPassword.length() < 8) {
        server.send(400, "text/plain", "SSID ou senha inválidos!");
        return;
      }
      wifi.setWifiCredentials(newSsid, newPassword);
      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        server.send(200, "text/plain", "Configuração Wi-Fi salva com sucesso! Reiniciando em 10 segundos...");
        unsigned long start = millis();
        while (millis() - start < 10000) { ESP.wdtFeed(); }
        ESP.restart();
      } else {
        server.send(500, "text/plain", "Falha ao conectar com as novas credenciais!");
      }
    });

    server.on("/getHostname", HTTP_GET, [this]() {
      server.send(200, "text/plain", wifi.getHostname());
    });

    server.on("/getSchedule", HTTP_GET, [this]() { schedule.handleGet(server); });
    server.on("/saveSchedule", HTTP_POST, [this]() { schedule.handleSave(server); });

    server.on("/getScheduleStatus", HTTP_GET, [this]() { schedule.handleGetStatus(server); });

    server.on("/ntpStatus", HTTP_GET, [this]() { ntp.handleStatus(server); });

    server.on("/restart", HTTP_GET, [this]() {
      Serial.println("Reiniciando o ESP...");
      server.send(200, "text/plain", "Reiniciando o ESP... Aguarde 30 segundos.");
      unsigned long start = millis();
      while (millis() - start < 2000) { ESP.wdtFeed(); }
      ESP.restart();
    });

    server.on("/getWifiInfo", HTTP_GET, [this]() {
      DynamicJsonDocument json(256);
      json["ssid"] = WiFi.SSID();
      long rssi = WiFi.RSSI();
      json["rssi"] = rssi;
      String signalStrength;
      if (rssi >= -50) {
        signalStrength = "Bom";
      } else if (rssi >= -70) {
        signalStrength = "Médio";
      } else {
        signalStrength = "Ruim";
      }
      json["signalStrength"] = signalStrength;
      json["ip"] = WiFi.localIP().toString();
      json["hostname"] = wifi.getHostname();
      String response;
      serializeJson(json, response);
      server.send(200, "application/json", response);
    });

    // Nova rota para informações de hardware
    server.on("/getHardwareInfo", HTTP_GET, [this]() { handleHardwareInfo(); });

  }
};

FreshPulseServer FreshPulse;

void setup() {
  FreshPulse.setup();
}

void loop() {
  FreshPulse.loop();
}