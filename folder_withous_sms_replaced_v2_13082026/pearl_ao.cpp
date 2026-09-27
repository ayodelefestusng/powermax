#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ─── Pin Configurations ──────────────────────────────────────
const int ADC_PIN_RED    = 36; // VP (GPIO 36)
const int ADC_PIN_YELLOW = 39; // VN (GPIO 39)
const int ADC_PIN_BLUE   = 34; // GPIO 34

// ─── Thresholds & State Logic ────────────────────────────────
const float VOLTAGE_THRESHOLD_ON  = 95.0f;  
const float VOLTAGE_THRESHOLD_OFF = 65.0f;  
const float MAX_VALID_VOLTAGE     = 280.0f;

// ─── Calibration Gains & Offsets ─────────────────────────────
// Red gain adjusted from 11.900f -> 13.642f to align ~196V telemetry with ~224.7V line voltage
const float CAL_GAIN_R   = 13.642f;
const float CAL_OFFSET_R = 0.00f;

const float CAL_GAIN_Y   = 0.4598f; 
const float CAL_OFFSET_Y = 0.00f;

const float CAL_GAIN_B   = 0.4802f; 
const float CAL_OFFSET_B = 0.00f;

// Red's higher gain amplifies its disconnected-input noise into a false reading.
const float NOISE_FLOOR_CUTOFF = 5.0f;
const float RED_NOISE_FLOOR_CUTOFF = 10.0f;

// ─── State Variables ─────────────────────────────────────────
struct PhaseReadings {
    float voltsR, voltsY, voltsB;
    bool stateR, stateY, stateB;
    bool readSuccess;
};

float smoothR = 0.0f;
float smoothY = 0.0f;
float smoothB = 0.0f;

bool lastStateR = false;
bool lastStateY = false;
bool lastStateB = false;

// ════════════════════════════════════════════════════════════
// TRUE RMS CALCULATOR WITH DYNAMIC MIDPOINT BIAS
// ════════════════════════════════════════════════════════════
float calculateRMS(const std::vector<int>& samples, float gain, float offset, const char* phaseLabel) {
    if (samples.empty()) return 0.0f;

    try {
        // Dynamic DC offset tracking
        double sumSamples = 0.0;
        for (int raw : samples) {
            sumSamples += raw;
        }
        double dynamicMidpoint = sumSamples / static_cast<double>(samples.size());

        // RMS Calculation against dynamic center
        double sumSquaredDiff = 0.0;
        for (int raw : samples) {
            double diff = static_cast<double>(raw) - dynamicMidpoint;
            sumSquaredDiff += (diff * diff);
        }

        double meanSquare = sumSquaredDiff / static_cast<double>(samples.size());
        float rawRms = static_cast<float>(std::sqrt(meanSquare));

        // Apply the stricter cutoff only to the high-gain red channel.
        float noiseFloorCutoff = NOISE_FLOOR_CUTOFF;
        if (phaseLabel != nullptr && phaseLabel[0] == 'R') {
            noiseFloorCutoff = RED_NOISE_FLOOR_CUTOFF;
        }

        // Raw noise floor check before scaling
        if (rawRms < noiseFloorCutoff) {
            return 0.0f;
        }

        // Linear scaling
        float calculatedVolts = (rawRms * gain) + offset;

        return calculatedVolts;
    } catch (...) {
        Serial.println(F("[ERROR] Exception trapped during RMS calculation."));
        return 0.0f;
    }
}

// Optional optimization: Instantly zero out smoothing on extreme sudden drops
float updatePhaseFilter(float currentSmooth, float rawVolts) {
    try {
        if (currentSmooth < 10.0f && rawVolts >= 15.0f) {
            return rawVolts;
        }

        // Fast decay on collapse
        if (rawVolts < 15.0f && currentSmooth > 50.0f) {
            return rawVolts; 
        }

        if (rawVolts < 10.0f && currentSmooth < 15.0f) {
            return 0.0f;
        }

        float diff = std::fabs(rawVolts - currentSmooth);
        float alpha = 0.15f; 

        if (diff > 50.0f) {
            alpha = 0.85f; 
        } else if (diff > 20.0f) {
            alpha = 0.45f;
        }

        return (currentSmooth * (1.0f - alpha)) + (rawVolts * alpha);
    } 
    catch (...) {
        Serial.println(F("[ERROR] Exception trapped in updatePhaseFilter."));
        return currentSmooth;
    }
}
bool evaluatePhaseState(float volts, bool currentState) {
    try {
        if (currentState) {
            return (volts >= VOLTAGE_THRESHOLD_OFF);
        } else {
            return (volts >= VOLTAGE_THRESHOLD_ON);
        }
    } catch (...) {
        Serial.println(F("[ERROR] Exception trapped in evaluatePhaseState."));
        return currentState;
    }
}

// ════════════════════════════════════════════════════════════
// SENSOR SAMPLING LOOP
// ════════════════════════════════════════════════════════════
PhaseReadings readPhaseSensors() {
    PhaseReadings data = {0.0f, 0.0f, 0.0f, false, false, false, false};

    try {
        std::vector<int> samplesR;
        std::vector<int> samplesY;
        std::vector<int> samplesB;

        samplesR.reserve(300);
        samplesY.reserve(300);
        samplesB.reserve(300);

        // Sample across 200ms (~10 full 50Hz cycles)
        unsigned long start = millis();
        while (millis() - start < 200UL) {
            int r = analogRead(ADC_PIN_RED);
            int y = analogRead(ADC_PIN_YELLOW);
            int b = analogRead(ADC_PIN_BLUE);

            if (r >= 0 && r <= 4095) samplesR.push_back(r);
            if (y >= 0 && y <= 4095) samplesY.push_back(y);
            if (b >= 0 && b <= 4095) samplesB.push_back(b);

            delayMicroseconds(150);
        }

        // Calculate RMS per phase
        float rawVoltsR = calculateRMS(samplesR, CAL_GAIN_R, CAL_OFFSET_R, "R");
        float rawVoltsY = calculateRMS(samplesY, CAL_GAIN_Y, CAL_OFFSET_Y, "Y");
        float rawVoltsB = calculateRMS(samplesB, CAL_GAIN_B, CAL_OFFSET_B, "B");

        // Cap at upper limit
        rawVoltsR = std::min(rawVoltsR, MAX_VALID_VOLTAGE);
        rawVoltsY = std::min(rawVoltsY, MAX_VALID_VOLTAGE);
        rawVoltsB = std::min(rawVoltsB, MAX_VALID_VOLTAGE);

        // Exponential smoothing filter
        smoothR = updatePhaseFilter(smoothR, rawVoltsR);
        smoothY = updatePhaseFilter(smoothY, rawVoltsY);
        smoothB = updatePhaseFilter(smoothB, rawVoltsB);

        data.voltsR = smoothR;
        data.voltsY = smoothY;
        data.voltsB = smoothB;

        lastStateR = evaluatePhaseState(data.voltsR, lastStateR);
        lastStateY = evaluatePhaseState(data.voltsY, lastStateY);
        lastStateB = evaluatePhaseState(data.voltsB, lastStateB);

        data.stateR = lastStateR;
        data.stateY = lastStateY;
        data.stateB = lastStateB;

        data.readSuccess = true;
        return data;

    } catch (...) {
        Serial.println(F("[ERROR] Exception in readPhaseSensors execution."));
        data.readSuccess = false;
        return data;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    try {
        analogReadResolution(12);
        
        analogSetPinAttenuation(ADC_PIN_RED, ADC_11db);
        analogSetPinAttenuation(ADC_PIN_YELLOW, ADC_11db);
        analogSetPinAttenuation(ADC_PIN_BLUE, ADC_11db);

        pinMode(ADC_PIN_RED, INPUT);
        pinMode(ADC_PIN_YELLOW, INPUT);
        pinMode(ADC_PIN_BLUE, INPUT);

        for (int i = 0; i < 3; i++) {
            readPhaseSensors();
            delay(100);
        }

        Serial.println(F("[INFO] System setup completed. Red phase gain updated to 13.642f."));
    } catch (...) {
        Serial.println(F("[FATAL] Hardware initialization failed."));
    }
}

void loop() {
    try {
        PhaseReadings p = readPhaseSensors();

        if (p.readSuccess) {
            Serial.printf("[READING] R:%.2fV Y:%.2fV B:%.2fV | states:%d%d%d\n",
                          p.voltsR, p.voltsY, p.voltsB,
                          p.stateR ? 1 : 0, p.stateY ? 1 : 0, p.stateB ? 1 : 0);
            Serial.flush();
        } else {
            Serial.println(F("[WARN] Sensor read skipped."));
        }
    } catch (...) {
        Serial.println(F("[ERROR] Main loop exception trapped."));
    }

    delay(300);
}