# Sleep-Tracker

A wrist-worn sleep tracker built on the [Waveshare ESP32-C6-Touch-AMOLED-1.8](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-1.8) with an external MAX30102 pulse-oximetry sensor. It records heart rate, SpO2, and movement overnight, scores sleep on-device, and shows a morning report on the AMOLED touch display.

See **[PLAN.md](PLAN.md)** for the full project plan: hardware architecture, firmware stack, sleep-scoring approach, power budget, and phased milestones.

See **[FEATURES.md](FEATURES.md)** for the prioritized feature list (P0 core → P2 stretch → hardware add-ons).

See **[INTEGRATION.md](INTEGRATION.md)** for the long-term Home Assistant + CPAP integration design — publishing sleep data to HA over MQTT and showing a combined wrist + CPAP night summary on the display.
