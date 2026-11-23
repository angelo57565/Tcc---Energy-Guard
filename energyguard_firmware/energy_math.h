
#pragma once
#include <math.h>
#include <stdint.h>

struct Metrics {
  bool valid = false;
  float vrmsMv = 0, irmsMv = 0;
  float vrms = 0, irms = 0, power = 0, pf = 0, freq = 0;
  uint16_t samples = 0;
};

inline Metrics computeMetrics(
  const uint16_t* v,
  const uint16_t* c,
  uint16_t n,
  uint32_t elapsedUs,
  float voltCal,
  float currCal,
  float minVoltage,
  float minCurrent
) {
  Metrics m;
  if (n < 200 || elapsedUs == 0) return m;

  m.samples = n;

  double sumV = 0, sumI = 0;

  for (uint16_t k = 0; k < n; k++) {
    sumV += v[k];
    sumI += c[k];
  }

  const double meanV = sumV / n;
  const double meanI = sumI / n;

  double sV2 = 0, sI2 = 0, sVI = 0;

  for (uint16_t k = 0; k < n; k++) {
    const double dv = v[k] - meanV;
    const double di = c[k] - meanI;

    sV2 += dv * dv;
    sI2 += di * di;
    sVI += dv * di;
  }

  const double vrmsMv = sqrt(sV2 / n);
  const double irmsMv = sqrt(sI2 / n);
  const double pMv2 = sVI / n;

  // Estimativa da frequência
  float freq = 0;

  if (vrmsMv > 20.0) {
    const double sampleRate = (double)n / (elapsedUs * 1e-6);
    const double minPeriodSamples = sampleRate / 70.0;
    const double maxPeriodSamples = sampleRate / 50.0;
    const double hyst = fmax(10.0, 0.15 * vrmsMv);

    int32_t lastCrossing = -1;
    double periodSum = 0;
    uint16_t validPeriods = 0;
    bool high = false;

    for (uint16_t k = 0; k < n; k++) {
      const double dv = v[k] - meanV;

      if (!high && dv > hyst) {
        high = true;

        if (lastCrossing >= 0) {
          const double period = k - lastCrossing;

          if (period >= minPeriodSamples &&
              period <= maxPeriodSamples) {
            periodSum += period;
            validPeriods++;
          }
        }

        lastCrossing = k;
      }
      else if (high && dv < -hyst) {
        high = false;
      }
    }

    if (validPeriods >= 5) {
      const double averagePeriod = periodSum / validPeriods;
      freq = (float)(sampleRate / averagePeriod);
    }
  }

  // Conversão das medições
  float vrms = (float)(vrmsMv * voltCal);
  float irms = (float)(irmsMv * currCal);
  float p = (float)(fabs(pMv2) * voltCal * currCal);

  if (vrms < minVoltage) {
    vrms = 0;
    p = 0;
    freq = 0;
  }

  if (irms < minCurrent) {
    irms = 0;
    p = 0;
  }

  // Fator de potência
  float pf = 0;
  const float apparentPower = vrms * irms;

  if (apparentPower > 0) {
    pf = fminf(1.0f, p / apparentPower);
  }

  if (pf <= 0) {
    p = 0;
  }

  m.valid = true;
  m.vrmsMv = (float)vrmsMv;
  m.irmsMv = (float)irmsMv;
  m.vrms = vrms;
  m.irms = irms;
  m.power = p;
  m.pf = pf;
  m.freq = freq;

  return m;
}
