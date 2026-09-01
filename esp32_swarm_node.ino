/**
 * =================================================================================
 * SWARM ROBOTICS AUCTION ALLOCATION — ESP32 DECENTRALIZED NODE FIRMWARE
 * =================================================================================
 * Architecture: Masterless Peer-to-Peer Swarm Protocol via ESP-NOW
 * Hardware: ESP32 NodeMCU / WROOM-32 (Node ID: 1, 2, or 3)
 * Shared Logic: calculateBid() cost function identical to JS Web Simulation
 * =================================================================================
 */

#include <WiFi.h>
#include <esp_now.h>
#include <math.h>

// =================================================================================
// HARDWARE & SENSOR PIN CONFIGURATION (Placeholders for Physical Hardware)
// =================================================================================
#define NODE_ID 1                     // Unique Node Identifier: 1, 2, or 3 for each board
#define BATTERY_ADC_PIN 34           // GPIO 34: Voltage divider pin (R1=10k, R2=10k)
#define WHEEL_ENCODER_A_PIN 18       // GPIO 18: Wheel encoder channel A interrupt
#define WHEEL_ENCODER_B_PIN 19       // GPIO 19: Wheel encoder channel B interrupt
#define MOTOR_LEFT_PWM_PIN 25        // GPIO 25: Left motor PWM driver
#define MOTOR_RIGHT_PWM_PIN 26       // GPIO 26: Right motor PWM driver

// =================================================================================
// SHARED ALGORITHM PARAMETERS & CONSTANTS
// (Must match JS simulation constants in swarm_simulation.html exactly)
// =================================================================================
const float WEIGHT_DISTANCE = 0.5f;   // Weight multiplier for distance metric
const float WEIGHT_BATTERY  = 0.3f;   // Weight multiplier for battery loss metric
const float WEIGHT_STATUS   = 0.2f;   // Weight multiplier for busy status metric

const float CRITICAL_BATTERY_THRESHOLD = 15.0f; // Battery % below which bot drops task
const float MAX_ARENA_DIAGONAL = 900.0f;        // Max arena reference distance (mm / px)

// Node Operational States
enum NodeStatus {
  STATUS_IDLE = 0,
  STATUS_MOVING = 1,
  STATUS_WORKING = 2,
  STATUS_CRITICAL = 3
};

// ESP-NOW Mesh Packet Protocol Types
enum MessageType {
  MSG_TASK_ANNOUNCE = 1,  // Broadcast when a new task is spawned
  MSG_TASK_BID      = 2,  // Broadcast node's calculated cost for task
  MSG_WINNER_CLAIM  = 3,  // Broadcast by winning node to declare ownership
  MSG_TASK_DROPPED  = 4   // Broadcast when node's battery drops below 15% mid-task
};

// Task Definition Structure
struct TaskData {
  uint16_t id;
  float x;
  float y;
};

// ESP-NOW Network Transmission Packet
struct SwarmPacket {
  uint8_t msgType;      // MessageType enum
  uint8_t senderNodeId; // Node ID (1, 2, 3)
  TaskData task;        // Target task coordinates
  float bidCost;        // Calculated cost from calculateBid()
};

// Local Node State Data
struct BotState {
  uint8_t id;
  float x;
  float y;
  float battery;        // Remaining battery percentage (0.0 to 100.0)
  NodeStatus status;
  int currentTaskId;
} botState;

// Peer Broadcast MAC Address (FF:FF:FF:FF:FF:FF for all peers)
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Auction Bidding Memory Buffer
float receivedBids[4]; // Stores incoming bids indexed by Node ID (1..3)
bool hasBid[4];        // Tracks which nodes have broadcast their bids

// =================================================================================
// SENSOR HOOK FUNCTIONS (Simulated / Hardware Ready)
// =================================================================================

/**
 * HARDWARE HOOK: Reads current battery voltage percentage.
 * Uses ESP32 ADC on GPIO 34 connected to a 2:1 resistor voltage divider.
 */
float readBatteryPercentage() {
  // --- REAL HARDWARE IMPLEMENTATION ---
  // int rawADC = analogRead(BATTERY_ADC_PIN);
  // float pinVoltage = (rawADC / 4095.0f) * 3.3f;
  // float batteryVoltage = pinVoltage * 2.0f; // 2:1 divider factor
  // float pct = ((batteryVoltage - 3.2f) / (4.2f - 3.2f)) * 100.0f; // LiPo 1S range
  // return constrain(pct, 0.0f, 100.0f);

  // --- SIMULATED IMPLEMENTATION FOR TESTING ---
  return botState.battery; 
}

/**
 * HARDWARE HOOK: Updates bot odometry (x, y coordinates).
 * Uses wheel encoders on GPIO 18/19 or optical flow sensors.
 */
void updateOdometry() {
  // --- REAL HARDWARE IMPLEMENTATION ---
  // Read encoder tick counts via interrupts, compute dead reckoning dx & dy
  // botState.x += deltaX;
  // botState.y += deltaY;

  // --- SIMULATED MOVEMENT TOWARDS TARGET TASK ---
  if (botState.status == STATUS_MOVING && botState.currentTaskId > 0) {
    // Slowly drain battery during movement
    botState.battery -= 0.05f;
    if (botState.battery < 0.0f) botState.battery = 0.0f;
  }
}

// =================================================================================
// CORE ALGORITHM: calculateBid()
// SHARED FORMULA: Matches JavaScript version in swarm_simulation.html 100%
// =================================================================================

/**
 * Shared Bidding Cost Formula (C++ ESP32 Implementation)
 * Calculates task execution cost for this bot node. Lower cost is better.
 * @param bot Current state of the robot node
 * @param task Task data structure containing target (x, y) coordinates
 * @return float Calculated bid cost value
 */
float calculateBid(BotState bot, TaskData task) {
  // 1. Distance Metric: Euclidean distance normalized by maximum arena diagonal
  float dx = task.x - bot.x;
  float dy = task.y - bot.y;
  float distance = sqrtf(dx * dx + dy * dy);
  float normDistance = distance / MAX_ARENA_DIAGONAL;
  if (normDistance > 1.0f) normDistance = 1.0f;

  // 2. Battery Metric: Loss fraction (100% -> 0.0 penalty, 0% -> 1.0 penalty)
  float normBatteryPenalty = 1.0f - (bot.battery / 100.0f);

  // 3. Status Metric: Penalty if bot is already engaged or moving
  float statusPenalty = (bot.status != STATUS_IDLE) ? 1.0f : 0.0f;

  // Calculate weighted aggregate cost
  float cost = (WEIGHT_DISTANCE * normDistance) +
               (WEIGHT_BATTERY  * normBatteryPenalty) +
               (WEIGHT_STATUS   * statusPenalty);

  return cost;
}

// =================================================================================
// DECENTRALIZED AUCTION & NETWORK PROTOCOL HANDLERS
// =================================================================================

/**
 * Sends an ESP-NOW broadcast packet to all swarm peers.
 */
void broadcastPacket(SwarmPacket pkt) {
  esp_now_send(broadcastAddress, (uint8_t *)&pkt, sizeof(pkt));
}

/**
 * Evaluates collected bids from all 3 nodes and determines winner node locally.
 * Rule: Lowest cost wins the task. Masterless consensus.
 */
void evaluateAuction(TaskData task) {
  uint8_t winningNode = 0;
  float lowestCost = 9999.0f;

  Serial.println("\n--- [AUCTION EVALUATION] ---");
  for (int id = 1; id <= 3; id++) {
    if (hasBid[id]) {
      Serial.printf("Node %d Bid Cost: %.4f\n", id, receivedBids[id]);
      if (receivedBids[id] < lowestCost) {
        lowestCost = receivedBids[id];
        winningNode = id;
      }
    } else {
      Serial.printf("Node %d: NO BID / INELIGIBLE\n", id);
    }
  }

  if (winningNode == NODE_ID) {
    botState.status = STATUS_MOVING;
    botState.currentTaskId = task.id;
    Serial.printf(">>> LOCAL NODE %d WON TASK #%d with lowest cost %.4f! <<<\n", NODE_ID, task.id, lowestCost);

    // Broadcast winner claim to notify peers
    SwarmPacket claimPkt;
    claimPkt.msgType = MSG_WINNER_CLAIM;
    claimPkt.senderNodeId = NODE_ID;
    claimPkt.task = task;
    claimPkt.bidCost = lowestCost;
    broadcastPacket(claimPkt);
  } else if (winningNode > 0) {
    Serial.printf("Node %d won Task #%d. Local node remains IDLE.\n", winningNode, task.id);
  } else {
    Serial.println("No eligible bids received for Task.");
  }
}

/**
 * Handles incoming ESP-NOW packets from peer swarm nodes.
 */
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  SwarmPacket pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));

  switch (pkt.msgType) {
    case MSG_TASK_ANNOUNCE:
    case MSG_TASK_DROPPED: {
      if (pkt.msgType == MSG_TASK_DROPPED) {
        Serial.printf("\n[RE-AUCTION ALERT] Node %d DROPPED Task #%d (Low Battery)!\n", pkt.senderNodeId, pkt.task.id);
      } else {
        Serial.printf("\n[NEW TASK] Announcing Task #%d at (%.1f, %.1f)\n", pkt.task.id, pkt.task.x, pkt.task.y);
      }

      // Reset bid buffer
      memset(hasBid, 0, sizeof(hasBid));

      // Calculate local bid if battery is above threshold
      botState.battery = readBatteryPercentage();
      if (botState.battery >= CRITICAL_BATTERY_THRESHOLD) {
        float myCost = calculateBid(botState, pkt.task);
        receivedBids[NODE_ID] = myCost;
        hasBid[NODE_ID] = true;

        Serial.printf("Local Node %d calculated bid cost: %.4f\n", NODE_ID, myCost);

        // Broadcast bid to peers
        SwarmPacket bidPkt;
        bidPkt.msgType = MSG_TASK_BID;
        bidPkt.senderNodeId = NODE_ID;
        bidPkt.task = pkt.task;
        bidPkt.bidCost = myCost;
        broadcastPacket(bidPkt);
      } else {
        Serial.printf("Local Node %d INELIGIBLE (Battery %.1f%% < %.1f%% threshold)\n", 
                      NODE_ID, botState.battery, CRITICAL_BATTERY_THRESHOLD);
      }

      // Wait 100ms for peer bids, then evaluate locally
      delay(100);
      evaluateAuction(pkt.task);
      break;
    }

    case MSG_TASK_BID: {
      receivedBids[pkt.senderNodeId] = pkt.bidCost;
      hasBid[pkt.senderNodeId] = true;
      Serial.printf("Received Bid from Node %d: %.4f\n", pkt.senderNodeId, pkt.bidCost);
      break;
    }

    case MSG_WINNER_CLAIM: {
      if (pkt.senderNodeId != NODE_ID) {
        Serial.printf("Node %d claimed Task #%d.\n", pkt.senderNodeId, pkt.task.id);
      }
      break;
    }
  }
}

// =================================================================================
// ARDUINO SETUP & MAIN LOOP
// =================================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n=============================================\n");
  Serial.printf(" SWARM ROBOT NODE 0x0%d INITIALIZING...\n", NODE_ID);
  Serial.printf("=============================================\n");

  // 1. Initialize Bot State
  botState.id = NODE_ID;
  botState.x = 100.0f * NODE_ID;
  botState.y = 150.0f;
  botState.battery = 100.0f;
  botState.status = STATUS_IDLE;
  botState.currentTaskId = 0;

  // 2. Initialize WiFi in Station Mode for ESP-NOW
  WiFi.mode(WIFI_STA);
  Serial.print("Node MAC Address: ");
  Serial.println(WiFi.macAddress());

  // 3. Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW!");
    return;
  }

  // Register Receive Callback
  esp_now_register_recv_cb(OnDataRecv);

  // Register Broadcast Peer (FF:FF:FF:FF:FF:FF)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer!");
    return;
  }

  Serial.println("ESP-NOW Peer-to-Peer Mesh Ready.");
}

void loop() {
  // Update simulated position & sensors
  updateOdometry();
  botState.battery = readBatteryPercentage();

  // Failure/Reassignment Rule: Check if battery dropped mid-task
  if (botState.status == STATUS_MOVING && botState.battery < CRITICAL_BATTERY_THRESHOLD) {
    Serial.printf("\n[CRITICAL FAILURE] Battery dropped to %.1f%% (< %.1f%%) mid-task!\n", 
                  botState.battery, CRITICAL_BATTERY_THRESHOLD);
    
    uint16_t droppedId = botState.currentTaskId;
    botState.status = STATUS_CRITICAL;
    botState.currentTaskId = 0;

    // Broadcast TASK_DROPPED to force peer re-auction
    SwarmPacket dropPkt;
    dropPkt.msgType = MSG_TASK_DROPPED;
    dropPkt.senderNodeId = NODE_ID;
    dropPkt.task.id = droppedId;
    dropPkt.task.x = 400.0f; // Task position
    dropPkt.task.y = 300.0f;
    dropPkt.bidCost = 0.0f;

    Serial.printf("Broadcasting MSG_TASK_DROPPED for Task #%d...\n", droppedId);
    broadcastPacket(dropPkt);
  }

  delay(200); // Main loop timing tick
}
